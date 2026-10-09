package server

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"patronus/server/internal/config"
	filesvc "patronus/server/internal/files"
	"patronus/server/internal/push"
	"patronus/server/internal/storage"
)

type Server struct {
	cfg         config.Config
	repo        *storage.Repository
	logger      *slog.Logger
	sessions    *Registry
	files       *filesvc.Service
	notifier    *push.Service
	mws         []Middleware
	baseCtx     context.Context
	upgrader    websocket.Upgrader
	gateEnabled bool

	prekeyRatePerMin int
	prekeyLimiterMu  sync.Mutex
	prekeyLimiters   map[string]*rateLimiter

	provisionalDepositBudget int
}

func New(cfg config.Config, repo *storage.Repository, logger *slog.Logger) (*Server, error) {
	notifier, err := push.New(push.Config{
		ProjectID:          cfg.FCMProjectID,
		APIKey:             cfg.FCMAPIKey,
		AppID:              cfg.FCMAppID,
		ServiceAccountFile: cfg.FCMServiceAccountFile,
		RequestTimeout:     time.Duration(cfg.FCMRequestTimeoutSeconds) * time.Second,
	}, logger)
	if err != nil {
		return nil, fmt.Errorf("configure push service: %w", err)
	}

	logger.Info("fcm notifier state", slog.Bool("enabled", notifier != nil))

	return &Server{
		cfg:         cfg,
		repo:        repo,
		logger:      logger,
		sessions:    NewRegistry(),
		files:       filesvc.New(cfg, repo, logger),
		notifier:    notifier,
		gateEnabled:              cfg.MembershipGateEnabled(),
		prekeyRatePerMin:         cfg.PrekeyRatePerMinute,
		prekeyLimiters:           make(map[string]*rateLimiter),
		provisionalDepositBudget: cfg.ProvisionalDepositBudget,
		upgrader: websocket.Upgrader{
			ReadBufferSize:    32 * 1024,
			WriteBufferSize:   32 * 1024,
			EnableCompression: false,
			CheckOrigin: func(r *http.Request) bool {
				return true
			},
		},
	}, nil
}

// isWhitelisted reports whether identityHex may use this server as a full
// member. When the membership gate is disabled (no root members configured)
// every authenticated identity is allowed. identityHex is expected lowercase
// (as produced by hex.EncodeToString). The source of truth is the DB whitelist,
// seeded from config and grown as members enroll their new contacts.
func (s *Server) isWhitelisted(ctx context.Context, identityHex string) (bool, error) {
	if !s.gateEnabled {
		return true, nil
	}
	return s.repo.IsWhitelisted(ctx, identityHex)
}

func (s *Server) Run(ctx context.Context) error {
	s.baseCtx = ctx
	if err := s.files.Prepare(); err != nil {
		return fmt.Errorf("prepare file storage: %w", err)
	}

	httpServer := &http.Server{
		Addr:              net.JoinHostPort(s.cfg.IP, fmt.Sprintf("%d", s.cfg.Port)),
		Handler:           s.handler(),
		ReadHeaderTimeout: 10 * time.Second,
		TLSConfig: &tls.Config{
			MinVersion: tls.VersionTLS12,
		},
	}

	errCh := make(chan error, 1)
	go func() {
		s.logger.Info("server starting", slog.String("ip", s.cfg.IP), slog.Int("port", s.cfg.Port))
		errCh <- httpServer.ListenAndServeTLS(s.cfg.SSLCertFile, s.cfg.SSLKeyFile)
	}()

	select {
	case <-ctx.Done():
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		return httpServer.Shutdown(shutdownCtx)
	case err := <-errCh:
		if err == http.ErrServerClosed {
			return nil
		}
		return err
	}
}

func (s *Server) notifyNewMessage(identityHex string) {
	s.dispatchPush(identityHex, "new_message", func(ctx context.Context, token string) error {
		return s.notifier.NotifyNewMessage(ctx, token)
	})
}

// notifyIncomingCall wakes an offline recipient with a data-only push that
// carries no sensitive data — only the fact that a call is being placed. The
// app comes online, receives the (E2E-encrypted) call_request through the normal
// message channel and rings.
func (s *Server) notifyIncomingCall(identityHex string) {
	s.dispatchPush(identityHex, "call", func(ctx context.Context, token string) error {
		return s.notifier.NotifyIncomingCall(ctx, token)
	})
}

// notifyCallCancelled tells an offline recipient that the caller hung up before
// they answered, so a ringing call notification raised by an earlier call push
// stops and is dismissed. Like the call push, it carries no sensitive data.
func (s *Server) notifyCallCancelled(identityHex string) {
	s.dispatchPush(identityHex, "call_cancel", func(ctx context.Context, token string) error {
		return s.notifier.NotifyCallCancelled(ctx, token)
	})
}

// dispatchPush loads the recipient's FCM tokens and fans the given push out to
// each of them, pruning tokens the FCM backend reports as permanently invalid.
func (s *Server) dispatchPush(identityHex, kind string, send func(context.Context, string) error) {
	if s.notifier == nil {
		s.logger.Info("skipping fcm notification because notifier is disabled",
			slog.String("identity", identityHex), slog.String("kind", kind))
		return
	}

	go func() {
		s.logger.Info("starting fcm notification flow", slog.String("identity", identityHex), slog.String("kind", kind))

		baseCtx := s.baseCtx
		if baseCtx == nil {
			baseCtx = context.Background()
		}

		ctx, cancel := context.WithTimeout(baseCtx, time.Duration(s.cfg.FCMRequestTimeoutSeconds+5)*time.Second)
		defer cancel()

		tokens, err := s.repo.ListFCMTokens(ctx, identityHex)
		if err != nil {
			s.logger.Warn("failed to load fcm tokens", slog.Any("error", err), slog.String("identity", identityHex))
			return
		}

		s.logger.Info("loaded fcm tokens", slog.String("identity", identityHex), slog.String("kind", kind), slog.Int("token_count", len(tokens)))
		if len(tokens) == 0 {
			s.logger.Info("no fcm tokens registered for identity", slog.String("identity", identityHex))
			return
		}

		for idx, token := range tokens {
			s.logger.Info("attempting fcm notification",
				slog.String("identity", identityHex),
				slog.String("kind", kind),
				slog.Int("token_index", idx),
				slog.Int("token_count", len(tokens)))

			err := send(ctx, token)
			if err == nil {
				s.logger.Info("fcm notification delivered", slog.String("identity", identityHex), slog.String("kind", kind), slog.Int("token_index", idx))
				continue
			}

			var sendErr *push.SendError
			if errors.As(err, &sendErr) && sendErr.ShouldDeleteToken() {
				s.logger.Warn("deleting stale fcm token",
					slog.String("identity", identityHex),
					slog.Int("token_index", idx),
					slog.String("reason", sendErr.Code))
				if deleteErr := s.repo.DeleteFCMToken(ctx, token); deleteErr != nil {
					s.logger.Warn("failed to delete stale fcm token", slog.Any("error", deleteErr), slog.String("identity", identityHex))
				}
			}

			s.logger.Warn("failed to send fcm push", slog.Any("error", err), slog.String("identity", identityHex), slog.String("kind", kind), slog.Int("token_index", idx))
		}

		s.logger.Info("finished fcm notification flow", slog.String("identity", identityHex), slog.String("kind", kind))
	}()
}
