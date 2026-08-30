// Command server runs the NPSync sync backend.
package main

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"sort"
	"strings"
	"syscall"
	"time"

	_ "github.com/jackc/pgx/v5/stdlib"

	"npsync/server/internal/api"
	"npsync/server/internal/auth"
	"npsync/server/internal/blob"
	"npsync/server/internal/config"
	"npsync/server/internal/store"
	"npsync/server/migrations"
)

func main() {
	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo})))

	cfg, err := config.Load()
	if err != nil {
		slog.Error("config error", "err", err)
		os.Exit(1)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	st, err := store.OpenPostgres(ctx, cfg.DatabaseURL, auth.NewUUID)
	if err != nil {
		slog.Error("database connection failed", "err", err)
		os.Exit(1)
	}
	defer st.Close()

	if err := runMigrations(cfg.DatabaseURL); err != nil {
		slog.Error("migrations failed", "err", err)
		os.Exit(1)
	}

	blobs, err := openBlobStore(cfg)
	if err != nil {
		slog.Error("blob store setup failed", "err", err)
		os.Exit(1)
	}

	srv := api.NewServer(cfg, st, blobs)
	httpServer := &http.Server{
		Addr:              cfg.ListenAddr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       5 * time.Minute, // generous for large uploads
		WriteTimeout:      5 * time.Minute,
		IdleTimeout:       120 * time.Second,
	}

	go func() {
		slog.Info("npsync server listening",
			"addr", cfg.ListenAddr, "protocol", api.ProtocolVersion,
			"blob_backend", cfg.BlobBackend, "require_https", cfg.RequireHTTPS)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			slog.Error("server failed", "err", err)
			os.Exit(1)
		}
	}()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	<-stop

	slog.Info("shutting down")
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer shutdownCancel()
	_ = httpServer.Shutdown(shutdownCtx)
}

func openBlobStore(cfg *config.Config) (blob.Store, error) {
	switch cfg.BlobBackend {
	case "fs":
		return blob.NewFS(cfg.BlobFSDir)
	case "s3":
		return blob.NewS3(cfg.S3Endpoint, cfg.S3Bucket, cfg.S3Region, cfg.S3AccessKey, cfg.S3SecretKey)
	}
	return nil, errors.New("unknown blob backend")
}

// runMigrations applies embedded SQL migrations in lexical order.
// Migrations are plain SQL files; no ORM auto-migration is ever used.
func runMigrations(databaseURL string) error {
	db, err := sql.Open("pgx", databaseURL)
	if err != nil {
		return err
	}
	defer db.Close()

	entries, err := migrations.FS.ReadDir(".")
	if err != nil {
		return err
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), ".sql") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)

	for _, name := range names {
		var applied bool
		err := db.QueryRow(
			`SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version = $1)`, name).
			Scan(&applied)
		if err != nil {
			// schema_migrations doesn't exist yet (first run): the first
			// migration creates it, so treat query failure as "not applied".
			applied = false
		}
		if applied {
			continue
		}
		body, err := migrations.FS.ReadFile(name)
		if err != nil {
			return err
		}
		slog.Info("applying migration", "name", name)
		if _, err := db.Exec(string(body)); err != nil {
			return err
		}
		if _, err := db.Exec(
			`INSERT INTO schema_migrations (version) VALUES ($1) ON CONFLICT DO NOTHING`, name); err != nil {
			return err
		}
	}
	return nil
}
