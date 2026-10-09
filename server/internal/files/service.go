package files

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"patronus/server/internal/config"
	"patronus/server/internal/storage"
)

var (
	ErrInvalidRecipients = errors.New("invalid recipients")
	ErrInvalidFileID     = errors.New("invalid file id")
	ErrFileTooLarge      = errors.New("file too large")
	ErrStorageFull       = errors.New("insufficient storage")
	ErrFileNotFound      = errors.New("file not found")
)

type Service struct {
	cfg    config.Config
	repo   *storage.Repository
	logger *slog.Logger
}

func New(cfg config.Config, repo *storage.Repository, logger *slog.Logger) *Service {
	return &Service{
		cfg:    cfg,
		repo:   repo,
		logger: logger,
	}
}

func (s *Service) Prepare() error {
	return os.MkdirAll(s.cfg.FileStoragePath, 0o755)
}

func (s *Service) Store(ctx context.Context, size int64, recipients []string, body io.Reader) (*storage.FileRecord, error) {
	if size < 0 {
		return nil, fmt.Errorf("invalid content length: %d", size)
	}

	normalizedRecipients, err := NormalizeRecipients(recipients)
	if err != nil {
		return nil, err
	}

	if s.cfg.MaxFileSize > 0 && size > s.cfg.MaxFileSize {
		return nil, ErrFileTooLarge
	}

	if err := s.ensureCapacity(ctx, size); err != nil {
		return nil, err
	}

	fileID, err := generateFileID()
	if err != nil {
		return nil, fmt.Errorf("generate file id: %w", err)
	}

	record := storage.FileRecord{
		ID:          fileID,
		Size:        size,
		StoragePath: s.filePath(fileID),
		UploadedAt:  time.Now().UTC(),
	}

	if err := os.MkdirAll(filepath.Dir(record.StoragePath), 0o755); err != nil {
		return nil, fmt.Errorf("create file directory: %w", err)
	}

	tempFile, err := os.CreateTemp(s.cfg.FileStoragePath, ".upload-*")
	if err != nil {
		return nil, fmt.Errorf("create temp file: %w", err)
	}
	tempPath := tempFile.Name()
	cleanupTemp := true
	defer func() {
		if cleanupTemp {
			_ = os.Remove(tempPath)
		}
	}()

	hasher := sha256.New()
	written, err := io.Copy(io.MultiWriter(tempFile, hasher), body)
	if closeErr := tempFile.Close(); closeErr != nil && err == nil {
		err = closeErr
	}
	if err != nil {
		return nil, fmt.Errorf("write blob: %w", err)
	}
	if written != size {
		return nil, fmt.Errorf("unexpected upload size: got %d bytes, expected %d", written, size)
	}

	record.SHA256 = hex.EncodeToString(hasher.Sum(nil))

	if err := os.Rename(tempPath, record.StoragePath); err != nil {
		return nil, fmt.Errorf("move blob into storage: %w", err)
	}
	cleanupTemp = false

	if err := s.repo.StoreFile(ctx, record, normalizedRecipients); err != nil {
		_ = os.Remove(record.StoragePath)
		if errors.Is(err, storage.ErrFileIDConflict) {
			return nil, fmt.Errorf("store file metadata: %w", err)
		}
		return nil, err
	}

	return &record, nil
}

func (s *Service) LookupForRecipient(ctx context.Context, fileID uint64, recipient string) (*storage.FileRecord, error) {
	record, err := s.repo.GetFileForRecipient(ctx, fileID, strings.ToLower(recipient))
	if err != nil {
		return nil, err
	}
	if record == nil {
		return nil, ErrFileNotFound
	}
	return record, nil
}

func (s *Service) MarkDelivered(ctx context.Context, fileID uint64, recipient string) error {
	return s.repo.DeleteFileRecipient(ctx, fileID, strings.ToLower(recipient))
}

func NormalizeRecipients(recipients []string) ([]string, error) {
	unique := make(map[string]struct{}, len(recipients))
	normalized := make([]string, 0, len(recipients))

	for _, recipient := range recipients {
		recipient = strings.ToLower(strings.TrimSpace(recipient))
		if recipient == "" {
			continue
		}
		if len(recipient) != 64 {
			return nil, ErrInvalidRecipients
		}
		if _, err := hex.DecodeString(recipient); err != nil {
			return nil, ErrInvalidRecipients
		}
		if _, exists := unique[recipient]; exists {
			continue
		}
		unique[recipient] = struct{}{}
		normalized = append(normalized, recipient)
	}

	if len(normalized) == 0 {
		return nil, ErrInvalidRecipients
	}

	return normalized, nil
}

func FormatFileID(fileID uint64) string {
	return fmt.Sprintf("%016x", fileID)
}

func ParseFileID(value string) (uint64, error) {
	if len(value) != 16 {
		return 0, ErrInvalidFileID
	}
	parsed, err := strconv.ParseUint(value, 16, 64)
	if err != nil {
		return 0, ErrInvalidFileID
	}
	return parsed, nil
}

func (s *Service) ensureCapacity(ctx context.Context, incomingSize int64) error {
	if s.cfg.MaxFileSize > 0 && incomingSize > s.cfg.MaxFileSize {
		return ErrFileTooLarge
	}

	hasCapacity, err := s.hasCapacity(ctx, incomingSize)
	if err != nil {
		return err
	}
	if hasCapacity {
		return nil
	}

	cleanupStages := []func(context.Context) ([]storage.FileRecord, error){
		s.repo.ListFilesWithoutRecipients,
		func(ctx context.Context) ([]storage.FileRecord, error) {
			if s.cfg.FileTTLSeconds <= 0 {
				return nil, nil
			}
			expiredBefore := time.Now().UTC().Add(-time.Duration(s.cfg.FileTTLSeconds) * time.Second)
			return s.repo.ListExpiredFiles(ctx, expiredBefore)
		},
		s.repo.ListOldestFiles,
	}

	for _, stage := range cleanupStages {
		candidates, err := stage(ctx)
		if err != nil {
			return err
		}

		for _, candidate := range candidates {
			if err := s.deleteStoredFile(ctx, candidate); err != nil {
				s.logger.Warn("failed to delete stored file during cleanup",
					slog.String("file_id", FormatFileID(candidate.ID)),
					slog.Any("error", err),
				)
				continue
			}

			hasCapacity, err = s.hasCapacity(ctx, incomingSize)
			if err != nil {
				return err
			}
			if hasCapacity {
				return nil
			}
		}
	}

	hasCapacity, err = s.hasCapacity(ctx, incomingSize)
	if err != nil {
		return err
	}
	if !hasCapacity {
		return ErrStorageFull
	}

	return nil
}

func (s *Service) hasCapacity(ctx context.Context, incomingSize int64) (bool, error) {
	if incomingSize < 0 {
		return false, nil
	}

	freeBytes, err := freeSpaceBytes(s.cfg.FileStoragePath)
	if err != nil {
		return false, fmt.Errorf("read free disk space: %w", err)
	}

	if s.cfg.MaxStorageSize > 0 {
		usedBytes, err := s.repo.UsedStorageBytes(ctx)
		if err != nil {
			return false, err
		}
		if usedBytes+incomingSize > s.cfg.MaxStorageSize {
			return false, nil
		}
		return incomingSize <= freeBytes, nil
	}

	reserveBytes := freeBytes / 10
	availableBytes := freeBytes - reserveBytes
	if availableBytes < 0 {
		availableBytes = 0
	}
	return incomingSize <= availableBytes, nil
}

func (s *Service) deleteStoredFile(ctx context.Context, record storage.FileRecord) error {
	trashPath := record.StoragePath + ".deleting"
	renamed := false

	if err := os.Rename(record.StoragePath, trashPath); err != nil {
		if !os.IsNotExist(err) {
			return fmt.Errorf("move stored file to trash: %w", err)
		}
	} else {
		renamed = true
	}

	if err := s.repo.DeleteFileMetadata(ctx, record.ID); err != nil {
		if renamed {
			_ = os.Rename(trashPath, record.StoragePath)
		}
		return err
	}

	if renamed {
		if err := os.Remove(trashPath); err != nil && !os.IsNotExist(err) {
			return fmt.Errorf("remove trashed file: %w", err)
		}
	}

	return nil
}

func (s *Service) filePath(fileID uint64) string {
	name := FormatFileID(fileID)
	return filepath.Join(s.cfg.FileStoragePath, name[:2], name[2:4], name+".blob")
}

func generateFileID() (uint64, error) {
	var randomPart [4]byte
	if _, err := rand.Read(randomPart[:]); err != nil {
		return 0, err
	}

	timestampPart := uint32(time.Now().UTC().Unix())
	return uint64(timestampPart)<<32 | uint64(binary.BigEndian.Uint32(randomPart[:])), nil
}

func freeSpaceBytes(path string) (int64, error) {
	var stats syscall.Statfs_t
	if err := syscall.Statfs(path, &stats); err != nil {
		return 0, err
	}
	return int64(stats.Bavail) * int64(stats.Bsize), nil
}
