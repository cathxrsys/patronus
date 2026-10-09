package server

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"os"
)

func (s *Server) handleFCMClientConfig(w http.ResponseWriter, r *http.Request) {
	principal, ok := principalFromContext(r.Context())
	if !s.cfg.FCMEnabled() {
		s.logger.Info("fcm client config requested while fcm is disabled")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-FCM-Status", "disabled")
		w.WriteHeader(http.StatusNoContent)
		return
	}

	attrs := []any{slog.String("path", s.cfg.FCMClientConfigFile)}
	if ok {
		attrs = append(attrs, slog.String("identity", principal.IdentityHex))
	}
	s.logger.Info("fcm client config requested", attrs...)

	data, err := os.ReadFile(s.cfg.FCMClientConfigFile)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			s.logger.Warn("fcm client config file not found", slog.String("path", s.cfg.FCMClientConfigFile))
			http.NotFound(w, r)
			return
		}
		s.logger.Error("failed to read fcm client config file", slog.Any("error", err))
		w.WriteHeader(http.StatusInternalServerError)
		return
	}

	if looksLikeServiceAccountJSON(data) {
		s.logger.Error("refusing to serve fcm client config because file looks like a service account", slog.String("path", s.cfg.FCMClientConfigFile))
		w.WriteHeader(http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
	s.logger.Info("fcm client config served", slog.String("path", s.cfg.FCMClientConfigFile), slog.Int("bytes", len(data)))
}

func looksLikeServiceAccountJSON(data []byte) bool {
	var payload map[string]any
	if err := json.Unmarshal(data, &payload); err != nil {
		return false
	}
	_, hasPrivateKey := payload["private_key"]
	_, hasClientEmail := payload["client_email"]
	return hasPrivateKey || hasClientEmail
}
