package storage

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
)

var ErrFileIDConflict = errors.New("file id conflict")

type FileRecord struct {
	ID          uint64
	Size        int64
	SHA256      string
	StoragePath string
	UploadedAt  time.Time
}

func (r *Repository) StoreFile(ctx context.Context, record FileRecord, recipients []string) error {
	tx, err := r.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("begin store file tx: %w", err)
	}
	defer tx.Rollback(ctx)

	tag, err := tx.Exec(
		ctx,
		`INSERT INTO files (id, size, sha256, storage_path, uploaded_at) VALUES ($1, $2, $3, $4, $5) ON CONFLICT DO NOTHING`,
		int64(record.ID), record.Size, record.SHA256, record.StoragePath, record.UploadedAt,
	)
	if err != nil {
		return fmt.Errorf("insert file metadata: %w", err)
	}
	if tag.RowsAffected() == 0 {
		return ErrFileIDConflict
	}

	for _, recipient := range recipients {
		if _, err := tx.Exec(
			ctx,
			`INSERT INTO file_recipients (file_id, recipient_addr) VALUES ($1, $2)`,
			int64(record.ID), recipient,
		); err != nil {
			return fmt.Errorf("insert file recipient: %w", err)
		}
	}

	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("commit store file tx: %w", err)
	}

	return nil
}

func (r *Repository) GetFileForRecipient(ctx context.Context, fileID uint64, recipient string) (*FileRecord, error) {
	row := r.pool.QueryRow(
		ctx,
		`SELECT f.id, f.size, f.sha256, f.storage_path, f.uploaded_at
		 FROM files f
		 JOIN file_recipients fr ON fr.file_id = f.id
		 WHERE f.id = $1 AND fr.recipient_addr = $2`,
		int64(fileID), recipient,
	)

	record, err := scanFileRecord(row)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return nil, fmt.Errorf("query file for recipient: %w", err)
	}

	return record, nil
}

func (r *Repository) DeleteFileRecipient(ctx context.Context, fileID uint64, recipient string) error {
	_, err := r.pool.Exec(
		ctx,
		`DELETE FROM file_recipients WHERE file_id = $1 AND recipient_addr = $2`,
		int64(fileID), recipient,
	)
	if err != nil {
		return fmt.Errorf("delete file recipient: %w", err)
	}
	return nil
}

func (r *Repository) DeleteFileMetadata(ctx context.Context, fileID uint64) error {
	_, err := r.pool.Exec(ctx, `DELETE FROM files WHERE id = $1`, int64(fileID))
	if err != nil {
		return fmt.Errorf("delete file metadata: %w", err)
	}
	return nil
}

func (r *Repository) UsedStorageBytes(ctx context.Context) (int64, error) {
	var used int64
	if err := r.pool.QueryRow(ctx, `SELECT COALESCE(SUM(size), 0) FROM files`).Scan(&used); err != nil {
		return 0, fmt.Errorf("sum file sizes: %w", err)
	}
	return used, nil
}

func (r *Repository) ListFilesWithoutRecipients(ctx context.Context) ([]FileRecord, error) {
	rows, err := r.pool.Query(
		ctx,
		`SELECT f.id, f.size, f.sha256, f.storage_path, f.uploaded_at
		 FROM files f
		 LEFT JOIN file_recipients fr ON fr.file_id = f.id
		 WHERE fr.file_id IS NULL
		 ORDER BY f.size DESC, f.uploaded_at ASC`,
	)
	if err != nil {
		return nil, fmt.Errorf("list files without recipients: %w", err)
	}
	defer rows.Close()

	return collectFileRecords(rows)
}

func (r *Repository) ListExpiredFiles(ctx context.Context, uploadedBefore time.Time) ([]FileRecord, error) {
	rows, err := r.pool.Query(
		ctx,
		`SELECT id, size, sha256, storage_path, uploaded_at
		 FROM files
		 WHERE uploaded_at < $1
		 ORDER BY size DESC, uploaded_at ASC`,
		uploadedBefore,
	)
	if err != nil {
		return nil, fmt.Errorf("list expired files: %w", err)
	}
	defer rows.Close()

	return collectFileRecords(rows)
}

func (r *Repository) ListOldestFiles(ctx context.Context) ([]FileRecord, error) {
	rows, err := r.pool.Query(
		ctx,
		`SELECT id, size, sha256, storage_path, uploaded_at
		 FROM files
		 ORDER BY uploaded_at ASC, size DESC`,
	)
	if err != nil {
		return nil, fmt.Errorf("list oldest files: %w", err)
	}
	defer rows.Close()

	return collectFileRecords(rows)
}

type fileScanner interface {
	Scan(dest ...any) error
}

func scanFileRecord(scanner fileScanner) (*FileRecord, error) {
	var id int64
	var record FileRecord
	if err := scanner.Scan(&id, &record.Size, &record.SHA256, &record.StoragePath, &record.UploadedAt); err != nil {
		return nil, err
	}
	record.ID = uint64(id)
	return &record, nil
}

func collectFileRecords(rows pgx.Rows) ([]FileRecord, error) {
	records := make([]FileRecord, 0)
	for rows.Next() {
		record, err := scanFileRecord(rows)
		if err != nil {
			return nil, fmt.Errorf("scan file record: %w", err)
		}
		records = append(records, *record)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate file records: %w", err)
	}
	return records, nil
}
