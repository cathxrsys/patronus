package server

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"strconv"
	"strings"

	filesvc "patronus/server/internal/files"
	"patronus/server/internal/storage"
)

type uploadFileResponse struct {
	ID         string `json:"id"`
	Size       int64  `json:"size"`
	SHA256     string `json:"sha256"`
	UploadedAt string `json:"uploaded_at"`
}

type byteRange struct {
	start      int64
	end        int64
	statusCode int
}

func (s *Server) handleFileUpload(w http.ResponseWriter, r *http.Request) {
	if _, ok := principalFromContext(r.Context()); !ok {
		http.NotFound(w, r)
		return
	}

	if r.ContentLength < 0 {
		w.WriteHeader(http.StatusLengthRequired)
		return
	}

	recipients := strings.Split(r.Header.Get("X-Core-Recipients"), ",")
	record, err := s.files.Store(r.Context(), r.ContentLength, recipients, r.Body)
	if err != nil {
		s.writeFileError(w, r, err)
		return
	}

	response := uploadFileResponse{
		ID:         filesvc.FormatFileID(record.ID),
		Size:       record.Size,
		SHA256:     record.SHA256,
		UploadedAt: record.UploadedAt.UTC().Format(http.TimeFormat),
	}

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Location", "/files/"+response.ID)
	w.WriteHeader(http.StatusCreated)
	if err := json.NewEncoder(w).Encode(response); err != nil {
		s.logger.Warn("failed to encode file upload response", slog.Any("error", err))
	}
}

func (s *Server) handleFileDownload(w http.ResponseWriter, r *http.Request) {
	principal, ok := principalFromContext(r.Context())
	if !ok {
		http.NotFound(w, r)
		return
	}

	fileID, err := filesvc.ParseFileID(r.PathValue("fileID"))
	if err != nil {
		http.NotFound(w, r)
		return
	}

	record, err := s.files.LookupForRecipient(r.Context(), fileID, principal.IdentityHex)
	if err != nil {
		if errors.Is(err, filesvc.ErrFileNotFound) {
			http.NotFound(w, r)
			return
		}
		s.logger.Error("failed to resolve file metadata", slog.Any("error", err), slog.String("file_id", filesvc.FormatFileID(fileID)))
		w.WriteHeader(http.StatusInternalServerError)
		return
	}

	file, err := os.Open(record.StoragePath)
	if err != nil {
		if os.IsNotExist(err) {
			http.NotFound(w, r)
			return
		}
		s.logger.Error("failed to open stored file", slog.Any("error", err), slog.String("file_id", filesvc.FormatFileID(fileID)))
		w.WriteHeader(http.StatusInternalServerError)
		return
	}
	defer file.Close()

	selectedRange, err := parseByteRange(r.Header.Get("Range"), record.Size)
	if err != nil {
		w.Header().Set("Content-Range", fmt.Sprintf("bytes */%d", record.Size))
		w.WriteHeader(http.StatusRequestedRangeNotSatisfiable)
		return
	}

	contentLength := rangeLength(selectedRange)
	setDownloadHeaders(w, *record, selectedRange, contentLength)

	if r.Method == http.MethodHead {
		w.WriteHeader(selectedRange.statusCode)
		return
	}

	w.WriteHeader(selectedRange.statusCode)
	if contentLength == 0 {
		if err := s.files.MarkDelivered(r.Context(), record.ID, principal.IdentityHex); err != nil {
			s.logger.Warn("failed to remove recipient after empty file download", slog.Any("error", err), slog.String("file_id", filesvc.FormatFileID(record.ID)))
		}
		return
	}

	if _, err := file.Seek(selectedRange.start, io.SeekStart); err != nil {
		s.logger.Error("failed to seek stored file", slog.Any("error", err), slog.String("file_id", filesvc.FormatFileID(record.ID)))
		return
	}

	written, err := io.CopyN(w, file, contentLength)
	if err != nil {
		s.logger.Warn("file download interrupted", slog.Any("error", err), slog.String("file_id", filesvc.FormatFileID(record.ID)))
		return
	}

	if written == contentLength && selectedRange.end == record.Size-1 {
		if err := s.files.MarkDelivered(r.Context(), record.ID, principal.IdentityHex); err != nil {
			s.logger.Warn("failed to remove file recipient after download", slog.Any("error", err), slog.String("file_id", filesvc.FormatFileID(record.ID)))
		}
	}
}

func (s *Server) writeFileError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, filesvc.ErrInvalidRecipients):
		w.WriteHeader(http.StatusBadRequest)
	case errors.Is(err, filesvc.ErrFileTooLarge):
		w.WriteHeader(http.StatusRequestEntityTooLarge)
	case errors.Is(err, filesvc.ErrStorageFull):
		w.WriteHeader(http.StatusInsufficientStorage)
	default:
		s.logger.Error("file operation failed", slog.Any("error", err), slog.String("path", r.URL.Path))
		w.WriteHeader(http.StatusInternalServerError)
	}
}

func setDownloadHeaders(w http.ResponseWriter, record storage.FileRecord, selectedRange byteRange, contentLength int64) {
	w.Header().Set("Accept-Ranges", "bytes")
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(contentLength, 10))
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=\"%s.blob\"", filesvc.FormatFileID(record.ID)))
	w.Header().Set("X-File-Id", filesvc.FormatFileID(record.ID))
	w.Header().Set("X-File-Sha256", record.SHA256)
	w.Header().Set("X-File-Size", strconv.FormatInt(record.Size, 10))
	w.Header().Set("Last-Modified", record.UploadedAt.UTC().Format(http.TimeFormat))

	if selectedRange.statusCode == http.StatusPartialContent {
		w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", selectedRange.start, selectedRange.end, record.Size))
	}
}

func parseByteRange(headerValue string, size int64) (byteRange, error) {
	if size < 0 {
		return byteRange{}, errors.New("invalid file size")
	}
	if headerValue == "" {
		if size == 0 {
			return byteRange{start: 0, end: -1, statusCode: http.StatusOK}, nil
		}
		return byteRange{start: 0, end: size - 1, statusCode: http.StatusOK}, nil
	}
	if size == 0 {
		return byteRange{}, errors.New("range not satisfiable")
	}
	if !strings.HasPrefix(headerValue, "bytes=") {
		return byteRange{}, errors.New("unsupported range unit")
	}

	spec := strings.TrimSpace(strings.TrimPrefix(headerValue, "bytes="))
	if spec == "" || strings.Contains(spec, ",") {
		return byteRange{}, errors.New("multiple ranges not supported")
	}

	startPart, endPart, ok := strings.Cut(spec, "-")
	if !ok {
		return byteRange{}, errors.New("invalid range format")
	}

	if startPart == "" {
		suffixLength, err := strconv.ParseInt(endPart, 10, 64)
		if err != nil || suffixLength <= 0 {
			return byteRange{}, errors.New("invalid suffix range")
		}
		if suffixLength > size {
			suffixLength = size
		}
		return byteRange{start: size - suffixLength, end: size - 1, statusCode: http.StatusPartialContent}, nil
	}

	start, err := strconv.ParseInt(startPart, 10, 64)
	if err != nil || start < 0 || start >= size {
		return byteRange{}, errors.New("invalid range start")
	}

	end := size - 1
	if endPart != "" {
		parsedEnd, err := strconv.ParseInt(endPart, 10, 64)
		if err != nil || parsedEnd < start {
			return byteRange{}, errors.New("invalid range end")
		}
		if parsedEnd < size {
			end = parsedEnd
		}
	}

	return byteRange{start: start, end: end, statusCode: http.StatusPartialContent}, nil
}

func rangeLength(selectedRange byteRange) int64 {
	if selectedRange.end < selectedRange.start {
		return 0
	}
	return selectedRange.end - selectedRange.start + 1
}
