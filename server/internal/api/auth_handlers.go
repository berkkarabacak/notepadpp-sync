package api

import (
	"errors"
	"net/http"
	"strings"
	"time"

	"npsync/server/internal/apperr"
	"npsync/server/internal/auth"
	"npsync/server/internal/store"
)

type authRequest struct {
	Email      string `json:"email"`
	Password   string `json:"password"`
	DeviceName string `json:"device_name"`
}

type tokenResponse struct {
	AccountID       string    `json:"account_id"`
	DeviceID        string    `json:"device_id"`
	AccessToken     string    `json:"access_token"`
	AccessExpiresAt time.Time `json:"access_expires_at"`
	RefreshToken    string    `json:"refresh_token"`
	ServerProtocols []int     `json:"server_protocols"`
}

func normalizeEmail(e string) string { return strings.ToLower(strings.TrimSpace(e)) }

func validEmail(e string) bool {
	// Deliberately minimal: one '@', non-empty local/domain, bounded length.
	if len(e) < 3 || len(e) > 254 {
		return false
	}
	at := strings.IndexByte(e, '@')
	return at > 0 && at < len(e)-1 && strings.IndexByte(e[at+1:], '.') > 0
}

func (s *Server) issueTokens(w http.ResponseWriter, r *http.Request, acct *store.Account, deviceName string) {
	count, err := s.st.CountActiveDevices(r.Context(), acct.ID)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	if count >= s.cfg.MaxDevices {
		apperr.Write(w, apperr.New(http.StatusForbidden, "device_limit",
			"device limit reached; revoke a device first"))
		return
	}
	dev, err := s.st.CreateDevice(r.Context(), acct.ID, deviceName)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	s.respondWithTokens(w, r, acct.ID, dev.ID)
}

func (s *Server) respondWithTokens(w http.ResponseWriter, r *http.Request, accountID, devID string) {
	access, exp, err := s.signer.Mint(accountID, devID)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	refresh, refreshHash, err := auth.NewOpaqueToken()
	if err != nil {
		apperr.Write(w, err)
		return
	}
	if _, err := s.st.CreateRefreshToken(r.Context(), devID, refreshHash, s.now().Add(s.cfg.RefreshTokenTTL)); err != nil {
		apperr.Write(w, err)
		return
	}
	apperr.JSON(w, http.StatusOK, tokenResponse{
		AccountID:       accountID,
		DeviceID:        devID,
		AccessToken:     access,
		AccessExpiresAt: exp,
		RefreshToken:    refresh,
		ServerProtocols: []int{ProtocolVersion},
	})
}

func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	if !s.cfg.RegistrationOpen {
		apperr.Write(w, apperr.New(http.StatusForbidden, "registration_closed", "registration is disabled on this server"))
		return
	}
	var req authRequest
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	req.Email = normalizeEmail(req.Email)
	if !validEmail(req.Email) {
		apperr.Write(w, apperr.BadRequest("invalid email"))
		return
	}
	if err := auth.ValidatePasswordPolicy(req.Password); err != nil {
		apperr.Write(w, apperr.BadRequest(err.Error()))
		return
	}
	if strings.TrimSpace(req.DeviceName) == "" || len(req.DeviceName) > 64 {
		apperr.Write(w, apperr.BadRequest("device_name required (max 64 chars)"))
		return
	}
	if !s.limiter.Allow(limiterKey(r, req.Email)) {
		apperr.Write(w, apperr.RateLimited("too many attempts; try again later"))
		return
	}
	hash, err := auth.HashPassword(req.Password)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	acct, err := s.st.CreateAccount(r.Context(), req.Email, hash)
	if errors.Is(err, store.ErrEmailTaken) {
		apperr.Write(w, apperr.New(http.StatusConflict, "email_taken", "email already registered"))
		return
	}
	if err != nil {
		apperr.Write(w, err)
		return
	}
	s.issueTokens(w, r, acct, req.DeviceName)
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	var req authRequest
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	req.Email = normalizeEmail(req.Email)
	if !s.limiter.Allow(limiterKey(r, req.Email)) {
		apperr.Write(w, apperr.RateLimited("too many attempts; try again later"))
		return
	}
	acct, err := s.st.AccountByEmail(r.Context(), req.Email)
	if errors.Is(err, store.ErrNotFound) {
		// Run a dummy verify to keep timing uniform.
		_, _ = auth.VerifyPassword(dummyHash, req.Password)
		apperr.Write(w, apperr.ErrUnauthorized)
		return
	}
	if err != nil {
		apperr.Write(w, err)
		return
	}
	if acct.LockedUntil != nil && s.now().Before(*acct.LockedUntil) {
		apperr.Write(w, apperr.RateLimited("account temporarily locked due to failed logins"))
		return
	}
	ok, err := auth.VerifyPassword(acct.PasswordHash, req.Password)
	if err != nil || !ok {
		if _, uerr := s.st.RecordFailedLogin(r.Context(), acct.ID, s.cfg.LoginLockoutAfter, s.cfg.LoginLockoutFor); uerr != nil {
			apperr.Write(w, uerr)
			return
		}
		apperr.Write(w, apperr.ErrUnauthorized)
		return
	}
	_ = s.st.ResetFailedLogins(r.Context(), acct.ID)
	if strings.TrimSpace(req.DeviceName) == "" {
		req.DeviceName = "Notepad++ device"
	}
	s.issueTokens(w, r, acct, req.DeviceName)
}

// Precomputed argon2id hash for timing-equalization on unknown emails.
const dummyHash = "$argon2id$v=19$m=65536,t=3,p=2$c29tZXNhbHRzb21lc2FsdA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

type refreshRequest struct {
	RefreshToken string `json:"refresh_token"`
}

func (s *Server) handleRefresh(w http.ResponseWriter, r *http.Request) {
	var req refreshRequest
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	hash := auth.HashTokenSHA256(req.RefreshToken)
	tok, err := s.st.RefreshTokenByHash(r.Context(), hash)
	if errors.Is(err, store.ErrNotFound) || (err == nil && (tok.RevokedAt != nil || s.now().After(tok.ExpiresAt))) {
		apperr.Write(w, apperr.ErrUnauthorized)
		return
	}
	if err != nil {
		apperr.Write(w, err)
		return
	}
	dev, err := s.deviceForToken(r, tok.DeviceID)
	if err != nil {
		apperr.Write(w, apperr.ErrUnauthorized)
		return
	}
	newRefresh, newHash, err := auth.NewOpaqueToken()
	if err != nil {
		apperr.Write(w, err)
		return
	}
	if _, err := s.st.RotateRefreshToken(r.Context(), tok.ID, newHash, s.now().Add(s.cfg.RefreshTokenTTL)); err != nil {
		apperr.Write(w, err)
		return
	}
	access, exp, err := s.signer.Mint(dev.AccountID, dev.ID)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	apperr.JSON(w, http.StatusOK, tokenResponse{
		AccountID:       dev.AccountID,
		DeviceID:        dev.ID,
		AccessToken:     access,
		AccessExpiresAt: exp,
		RefreshToken:    newRefresh,
		ServerProtocols: []int{ProtocolVersion},
	})
}

// deviceForToken resolves a device from a refresh token (which itself
// authenticates the request) without an account context.
func (s *Server) deviceForToken(r *http.Request, deviceID string) (*store.Device, error) {
	dev, err := s.st.DeviceByIDGlobal(r.Context(), deviceID)
	if err != nil {
		return nil, err
	}
	if dev.RevokedAt != nil {
		return nil, store.ErrNotFound
	}
	return dev, nil
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	// Logout = revoke this device's refresh tokens (device record remains,
	// marked revoked so existing access tokens die quickly too).
	if err := s.st.RevokeDevice(r.Context(), accountID(r), deviceID(r)); err != nil {
		apperr.Write(w, err)
		return
	}
	apperr.JSON(w, http.StatusOK, map[string]string{"status": "ok"})
}
