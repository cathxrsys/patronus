package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"patronus/server/internal/config"
	"patronus/server/internal/logging"
	coreserver "patronus/server/internal/server"
	"patronus/server/internal/storage"
)

func main() {
	fmt.Print(`


   ▄███████▄    ▄████████     ███        ▄████████  ▄██████▄  ███▄▄▄▄   ███    █▄     ▄████████ 
  ███    ███   ███    ███ ▀█████████▄   ███    ███ ███    ███ ███▀▀▀██▄ ███    ███   ███    ███ 
  ███    ███   ███    ███    ▀███▀▀██   ███    ███ ███    ███ ███   ███ ███    ███   ███    █▀  
  ███    ███   ███    ███     ███   ▀  ▄███▄▄▄▄██▀ ███    ███ ███   ███ ███    ███   ███        
▀█████████▀  ▀███████████     ███     ▀▀███▀▀▀▀▀   ███    ███ ███   ███ ███    ███ ▀███████████ 
  ███          ███    ███     ███     ▀███████████ ███    ███ ███   ███ ███    ███          ███ 
  ███          ███    ███     ███       ███    ███ ███    ███ ███   ███ ███    ███    ▄█    ███ 
 ▄████▀        ███    █▀     ▄████▀     ███    ███  ▀██████▀   ▀█   █▀  ████████▀   ▄████████▀  
                                        ███    ███                                              


`)

	bootstrap := slog.New(logging.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelWarn,
	}))

	configPath, cliCfg, err := config.ParseArgs(os.Args[1:], os.Stdout, os.Stderr)
	if err != nil {
		if errors.Is(err, config.ErrHelp) {
			return
		}
		bootstrap.Error("failed to parse command-line arguments", slog.Any("error", err))
		os.Exit(2)
	}

	cfg, err := config.Load(configPath, cliCfg, bootstrap)
	if err != nil {
		bootstrap.Error("failed to load config", slog.Any("error", err))
		os.Exit(2)
	}

	logLevel := slog.LevelWarn
	if cfg.Debug {
		logLevel = slog.LevelDebug
	}
	logger := slog.New(logging.NewTextHandler(os.Stdout, &slog.HandlerOptions{
		Level: logLevel,
	}))

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	repository, err := storage.NewRepository(ctx, cfg.DBConnectionString, cfg.DBSchema, cfg.DBConnectionPoolSize, logger)
	if err != nil {
		logger.Error("failed to connect to PostgreSQL", slog.Any("error", err))
		os.Exit(1)
	}
	defer repository.Close()

	if err := repository.InitSchema(ctx); err != nil {
		logger.Error("failed to initialize schema", slog.Any("error", err))
		os.Exit(1)
	}

	// Seed the root member(s) from config into the whitelist. Everyone else is
	// admitted later, at runtime, when an existing member enrolls a contact who
	// scanned their QR. Idempotent, so it is safe on every start.
	if err := repository.SeedWhitelist(ctx, cfg.NormalizedMemberList()); err != nil {
		logger.Error("failed to seed whitelist", slog.Any("error", err))
		os.Exit(1)
	}

	server, err := coreserver.New(cfg, repository, logger)
	if err != nil {
		logger.Error("failed to create server", slog.Any("error", err))
		os.Exit(1)
	}
	if err := server.Run(ctx); err != nil {
		logger.Error("server stopped with error", slog.Any("error", err))
		os.Exit(1)
	}

	logger.Info("server stopped")
}
