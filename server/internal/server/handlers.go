package server

import (
	"log/slog"
	"net/http"
	"strings"

	"github.com/gorilla/websocket"

	"patronus/server/internal/protocol"
)

func (s *Server) handleRoot(w http.ResponseWriter, r *http.Request) {
	if websocket.IsWebSocketUpgrade(r) {
		s.handleWebSocket(w, r)
		return
	}

	s.handleDecoy(w)
}

func (s *Server) handleDecoy(w http.ResponseWriter) {
	w.Header().Set("Server", "nginx")
	w.Header().Set("Content-Type", "text/html")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(protocol.DecoyHTML))
}

func (s *Server) handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := s.upgrader.Upgrade(w, r, nil)
	if err != nil {
		s.logger.Warn("websocket upgrade failed", slog.Any("error", err))
		return
	}

	conn.SetReadLimit(protocol.MaxWSMessageSize)

	clientProtocolVersion := strings.TrimSpace(r.Header.Get("X-Protocol-Version"))
	session := NewSession(s, conn, r.RemoteAddr, clientProtocolVersion)
	go session.Run(s.baseCtx)
}
