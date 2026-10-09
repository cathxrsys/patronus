package server

import "net/http"

func (s *Server) handler() http.Handler {
	mux := http.NewServeMux()
	s.registerRoutes(mux)
	return s.withMiddlewares(mux)
}

func (s *Server) registerRoutes(mux *http.ServeMux) {
	mux.Handle("GET /fcm/client-config", s.withMiddlewares(s.requireSessionToken(http.HandlerFunc(s.handleFCMClientConfig))))
	mux.Handle("POST /files", s.withMiddlewares(s.requireSessionToken(http.HandlerFunc(s.handleFileUpload))))
	mux.Handle("GET /files/{fileID}", s.withMiddlewares(s.requireSessionToken(http.HandlerFunc(s.handleFileDownload))))
	mux.Handle("HEAD /files/{fileID}", s.withMiddlewares(s.requireSessionToken(http.HandlerFunc(s.handleFileDownload))))
	mux.Handle("/", s.withMiddlewares(http.HandlerFunc(s.handleRoot)))
}
