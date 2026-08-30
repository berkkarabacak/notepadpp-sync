// Package api implements the NPSync REST + WebSocket HTTP layer.
package api

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"npsync/server/internal/apperr"
	"npsync/server/internal/auth"
	"npsync/server/internal/blob"
	"npsync/server/internal/config"
	"npsync/server/internal/store"
	"npsync/server/internal/ws"
)

// ProtocolVersion is the wire protocol this server speaks.
const ProtocolVersion = 1

type ctxKey int

const (
	ctxAccountID ctxKey = iota
	ctxDeviceID
)

// Server wires together all handlers.
type Server struct {
	cfg     *config.Config
	st      store.Store
	blobs   blob.Store
	hub     *ws.Hub
	signer  *auth.TokenSigner
	limiter *loginLimiter
	mux     *http.ServeMux
	now     func() time.Time
}

func NewServer(cfg *config.Config, st store.Store, blobs blob.Store) *Server {
	s := &Server{
		cfg:     cfg,
		st:      st,
		blobs:   blobs,
		hub:     ws.NewHub(16),
		signer:  auth.NewTokenSigner(cfg.TokenSigningKey, cfg.AccessTokenTTL),
		limiter: newLoginLimiter(cfg.LoginRatePerMin),
		mux:     http.NewServeMux(),
		now:     time.Now,
	}
	s.routes()
	return s
}

// Hub exposes the event hub for tests.
func (s *Server) Hub() *ws.Hub { return s.hub }

func (s *Server) routes() {
	s.mux.HandleFunc("GET /health", s.handleHealth)
	s.mux.HandleFunc("GET /ready", s.handleReady)

	s.mux.HandleFunc("POST /auth/register", s.handleRegister)
	s.mux.HandleFunc("POST /auth/login", s.handleLogin)
	s.mux.HandleFunc("POST /auth/refresh", s.handleRefresh)
	s.mux.HandleFunc("POST /auth/logout", s.withAuth(s.handleLogout))

	s.mux.HandleFunc("GET /devices", s.withAuth(s.handleListDevices))
	s.mux.HandleFunc("POST /devices/pair", s.withAuth(s.handlePair))
	s.mux.HandleFunc("PATCH /devices/{id}", s.withAuth(s.handleRenameDevice))
	s.mux.HandleFunc("DELETE /devices/{id}", s.withAuth(s.handleRevokeDevice))

	s.mux.HandleFunc("GET /sync/files", s.withAuth(s.handleListFiles))
	s.mux.HandleFunc("POST /sync/files", s.withAuth(s.handleCreateFile))
	s.mux.HandleFunc("GET /sync/files/{fileId}", s.withAuth(s.handleGetFile))
	s.mux.HandleFunc("PUT /sync/files/{fileId}", s.withAuth(s.handleUpdateFile))
	s.mux.HandleFunc("DELETE /sync/files/{fileId}", s.withAuth(s.handleDeleteFile))
	s.mux.HandleFunc("GET /sync/files/{fileId}/versions", s.withAuth(s.handleListVersions))
	s.mux.HandleFunc("POST /sync/files/{fileId}/restore", s.withAuth(s.handleRestoreVersion))
	s.mux.HandleFunc("POST /sync/batch", s.withAuth(s.handleBatch))
	s.mux.HandleFunc("GET /sync/changes", s.withAuth(s.handleChanges))

	s.mux.HandleFunc("GET /session", s.withAuth(s.handleGetSession))
	s.mux.HandleFunc("PUT /session", s.withAuth(s.handlePutSession))

	s.mux.HandleFunc("GET /ws", s.handleWebSocket)
}

// Handler returns the root handler with global middleware applied.
func (s *Server) Handler() http.Handler {
	return s.protocolMiddleware(s.securityMiddleware(s.loggingMiddleware(s.mux)))
}

func (s *Server) loggingMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := s.now()
		next.ServeHTTP(w, r)
		slog.Info("request", "method", r.Method, "path", r.URL.Path, "dur", s.now().Sub(start))
	})
}

func (s *Server) securityMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		if s.cfg.RequireHTTPS && r.Header.Get("X-Forwarded-Proto") == "http" {
			apperr.Write(w, apperr.New(http.StatusForbidden, "https_required", "HTTPS is required"))
			return
		}
		next.ServeHTTP(w, r)
	})
}

// protocolMiddleware enforces the X-NPSync-Protocol version header on API
// routes and stamps it on every response.
func (s *Server) protocolMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-NPSync-Protocol", itoa(ProtocolVersion))
		if strings.HasPrefix(r.URL.Path, "/auth/") || strings.HasPrefix(r.URL.Path, "/sync/") ||
			strings.HasPrefix(r.URL.Path, "/devices") || strings.HasPrefix(r.URL.Path, "/session") ||
			r.URL.Path == "/ws" {
			if v := r.Header.Get("X-NPSync-Protocol"); v != "" && v != itoa(ProtocolVersion) {
				w.WriteHeader(http.StatusUpgradeRequired)
				_ = json.NewEncoder(w).Encode(map[string]any{
					"error":            "unsupported_protocol",
					"message":          "client/server protocol mismatch; update the plugin or the server",
					"server_protocols": []int{ProtocolVersion},
				})
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

// withAuth validates the Bearer access token and injects account/device IDs.
func (s *Server) withAuth(next func(http.ResponseWriter, *http.Request)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		h := r.Header.Get("Authorization")
		if !strings.HasPrefix(h, "Bearer ") {
			apperr.Write(w, apperr.ErrUnauthorized)
			return
		}
		claims, err := s.signer.Verify(strings.TrimPrefix(h, "Bearer "))
		if err != nil {
			apperr.Write(w, apperr.ErrUnauthorized)
			return
		}
		// Reject tokens for revoked devices.
		dev, err := s.st.DeviceByID(r.Context(), claims.AccountID, claims.DeviceID)
		if err != nil || dev.RevokedAt != nil {
			apperr.Write(w, apperr.ErrUnauthorized)
			return
		}
		_ = s.st.TouchDevice(r.Context(), claims.DeviceID)
		ctx := context.WithValue(r.Context(), ctxAccountID, claims.AccountID)
		ctx = context.WithValue(ctx, ctxDeviceID, claims.DeviceID)
		next(w, r.WithContext(ctx))
	}
}

func accountID(r *http.Request) string {
	v, _ := r.Context().Value(ctxAccountID).(string)
	return v
}

func deviceID(r *http.Request) string {
	v, _ := r.Context().Value(ctxDeviceID).(string)
	return v
}

func decodeJSON(w http.ResponseWriter, r *http.Request, v any, maxBytes int64) error {
	r.Body = http.MaxBytesReader(w, r.Body, maxBytes)
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		return apperr.BadRequest("invalid JSON body: " + err.Error())
	}
	return nil
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b [8]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	return string(b[i:])
}

// ---- health ----

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	apperr.JSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) handleReady(w http.ResponseWriter, r *http.Request) {
	// Touch the store; a failing DB makes readiness fail.
	if _, err := s.st.ListChangesSince(r.Context(), "00000000-0000-0000-0000-000000000000", 0, 1); err != nil {
		apperr.Write(w, err)
		return
	}
	apperr.JSON(w, http.StatusOK, map[string]string{"status": "ready"})
}
