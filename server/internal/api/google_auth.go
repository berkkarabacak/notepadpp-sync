package api

import (
	"context"
	"encoding/json"
	"errors"
	"html"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"time"

	"npsync/server/internal/apperr"
	"npsync/server/internal/auth"
	"npsync/server/internal/store"
)

const googleLoginTTL = 10 * time.Minute

type googleStartRequest struct {
	DeviceName string `json:"device_name"`
}

type googlePollRequest struct {
	State      string `json:"state"`
	PollSecret string `json:"poll_secret"`
}

type googleStartResponse struct {
	AuthorizationURL string `json:"authorization_url"`
	State            string `json:"state"`
	PollSecret       string `json:"poll_secret"`
	ExpiresIn        int    `json:"expires_in"`
}

// handleGoogleStart begins Authorization Code + PKCE.
// The plugin opens authorization_url in the system browser and polls with
// poll_secret. The code verifier never leaves the server.
func (s *Server) handleGoogleStart(w http.ResponseWriter, r *http.Request) {
	if !s.cfg.GoogleEnabled() {
		apperr.Write(w, apperr.New(http.StatusServiceUnavailable, "google_sso_disabled",
			"Google sign-in is not configured on this server"))
		return
	}
	var req googleStartRequest
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	name := strings.TrimSpace(req.DeviceName)
	if name == "" {
		name = "Notepad++ device"
	}
	if len(name) > 64 {
		apperr.Write(w, apperr.BadRequest("device_name must be at most 64 characters"))
		return
	}
	if !s.limiter.Allow(limiterKey(r, "google-start")) {
		apperr.Write(w, apperr.RateLimited("too many attempts; try again later"))
		return
	}
	verifier, challenge, err := auth.NewCodeVerifier()
	if err != nil {
		apperr.Write(w, err)
		return
	}
	state, err := auth.RandomB64URL(32)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	nonce, err := auth.RandomB64URL(32)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	pollSecret, err := auth.RandomB64URL(32)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	login := &store.OAuthLogin{
		State:          state,
		CodeVerifier:   verifier,
		Nonce:          nonce,
		DeviceName:     name,
		PollSecretHash: auth.HashTokenSHA256(pollSecret),
		ExpiresAt:      s.now().Add(googleLoginTTL),
		Status:         store.OAuthPending,
	}
	if err := s.st.CreateOAuthLogin(r.Context(), login); err != nil {
		apperr.Write(w, err)
		return
	}
	apperr.JSON(w, http.StatusOK, googleStartResponse{
		AuthorizationURL: s.googleAuthorizationURL(state, nonce, challenge),
		State:            state,
		PollSecret:       pollSecret,
		ExpiresIn:        int(googleLoginTTL.Seconds()),
	})
}

func (s *Server) googleAuthorizationURL(state, nonce, challenge string) string {
	q := url.Values{}
	q.Set("client_id", s.cfg.GoogleClientID)
	q.Set("redirect_uri", s.cfg.GoogleRedirectURIOrDefault())
	q.Set("response_type", "code")
	q.Set("scope", "openid email profile")
	q.Set("state", state)
	q.Set("nonce", nonce)
	q.Set("code_challenge", challenge)
	q.Set("code_challenge_method", "S256")
	q.Set("access_type", "online")
	q.Set("prompt", "select_account")
	return s.cfg.GoogleAuthURL + "?" + q.Encode()
}

// handleGoogleCallback is the browser redirect target registered in the
// Google Cloud console. It exchanges the code and tells the user to return
// to Notepad++. NPSync tokens are issued only to the plugin poll.
func (s *Server) handleGoogleCallback(w http.ResponseWriter, r *http.Request) {
	if !s.cfg.GoogleEnabled() {
		writeGooglePage(w, false, "Google sign-in is not configured on this server.")
		return
	}
	q := r.URL.Query()
	state := q.Get("state")
	if state == "" {
		writeGooglePage(w, false, "This sign-in link is missing its state. Return to Notepad++ and try again.")
		return
	}
	existing, err := s.st.OAuthLoginByState(r.Context(), state)
	if errors.Is(err, store.ErrNotFound) {
		writeGooglePage(w, false, "This sign-in session was not found. Return to Notepad++ and try again.")
		return
	}
	if err != nil {
		writeGooglePage(w, false, "Sign-in could not be completed. Return to Notepad++ and try again.")
		return
	}
	if s.now().After(existing.ExpiresAt) {
		writeGooglePage(w, false, "This sign-in expired. Return to Notepad++ and try again.")
		return
	}
	switch existing.Status {
	case store.OAuthReady, store.OAuthConsumed, store.OAuthExchanging:
		writeGooglePage(w, true, "You can close this window and return to Notepad++.")
		return
	case store.OAuthError:
		writeGooglePage(w, false, fallback(existing.ErrorMessage, "Google sign-in failed. Return to Notepad++ and try again."))
		return
	}

	login, err := s.st.ClaimOAuthLogin(r.Context(), state, s.now())
	if err != nil {
		again, rerr := s.st.OAuthLoginByState(r.Context(), state)
		if rerr == nil && again.Status == store.OAuthError {
			writeGooglePage(w, false, fallback(again.ErrorMessage, "Google sign-in failed. Return to Notepad++ and try again."))
			return
		}
		writeGooglePage(w, true, "You can close this window and return to Notepad++.")
		return
	}

	fail := func(code, message string) {
		_ = s.st.MarkOAuthError(r.Context(), state, code, message)
		writeGooglePage(w, false, message)
	}

	if gerr := q.Get("error"); gerr != "" {
		slog.Info("google sign-in denied", "error", gerr)
		fail("google_denied", "Google did not complete sign-in. Return to Notepad++ and try again.")
		return
	}
	code := q.Get("code")
	if code == "" || login.CodeVerifier == "" {
		fail("invalid_request", "Google did not return a sign-in code. Return to Notepad++ and try again.")
		return
	}
	idToken, err := s.exchangeGoogleCode(r.Context(), code, login.CodeVerifier)
	if err != nil {
		slog.Warn("google token exchange failed", "err", err)
		fail("google_exchange_failed", "Google sign-in could not be completed. Return to Notepad++ and try again.")
		return
	}
	identity, err := s.verifyGoogleIDToken(r.Context(), idToken, login.Nonce)
	if err != nil {
		slog.Warn("google id token rejected", "err", err)
		fail("google_token_rejected", "Google sign-in could not be verified. Return to Notepad++ and try again.")
		return
	}
	acct, err := s.accountForGoogle(r.Context(), identity.Subject, normalizeEmail(identity.Email))
	if err != nil {
		var ae *apperr.Error
		if errors.As(err, &ae) {
			fail(ae.Code, ae.Message)
			return
		}
		slog.Error("google account link failed", "err", err)
		fail("internal", "Sign-in could not be completed. Return to Notepad++ and try again.")
		return
	}
	if err := s.st.MarkOAuthReady(r.Context(), state, acct.ID); err != nil {
		slog.Error("google sign-in could not be saved", "err", err)
		fail("internal", "Sign-in could not be completed. Return to Notepad++ and try again.")
		return
	}
	slog.Info("google sign-in ready", "account_id", acct.ID)
	writeGooglePage(w, true, "You can close this window and return to Notepad++.")
}

func (s *Server) handleGooglePoll(w http.ResponseWriter, r *http.Request) {
	if !s.cfg.GoogleEnabled() {
		apperr.Write(w, apperr.New(http.StatusServiceUnavailable, "google_sso_disabled",
			"Google sign-in is not configured on this server"))
		return
	}
	var req googlePollRequest
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	if req.State == "" || req.PollSecret == "" {
		apperr.Write(w, apperr.BadRequest("state and poll_secret are required"))
		return
	}
	login, err := s.st.OAuthLoginByState(r.Context(), req.State)
	if errors.Is(err, store.ErrNotFound) {
		s.rejectGooglePoll(w, r)
		return
	}
	if err != nil {
		apperr.Write(w, err)
		return
	}
	if !auth.PollSecretMatches(login.PollSecretHash, req.PollSecret) {
		s.rejectGooglePoll(w, r)
		return
	}
	if s.now().After(login.ExpiresAt) {
		apperr.Write(w, apperr.New(http.StatusGone, "login_expired", "Google sign-in expired; start again"))
		return
	}
	switch login.Status {
	case store.OAuthPending, store.OAuthExchanging:
		apperr.JSON(w, http.StatusAccepted, map[string]string{"status": "pending"})
	case store.OAuthError:
		apperr.Write(w, oauthStatusError(login.ErrorCode, login.ErrorMessage))
	case store.OAuthConsumed:
		apperr.Write(w, apperr.ErrUnauthorized)
	case store.OAuthReady:
		if _, err := s.st.ConsumeOAuthLogin(r.Context(), login.State); err != nil {
			apperr.Write(w, apperr.ErrUnauthorized)
			return
		}
		acct, err := s.st.AccountByID(r.Context(), login.AccountID)
		if err != nil {
			_ = s.st.ReopenOAuthLogin(r.Context(), login.State)
			apperr.Write(w, err)
			return
		}
		s.finishGoogleLogin(w, r, acct, login.DeviceName, login.State)
	default:
		apperr.Write(w, apperr.ErrUnauthorized)
	}
}

func (s *Server) rejectGooglePoll(w http.ResponseWriter, r *http.Request) {
	if !s.limiter.Allow(limiterKey(r, "google-poll")) {
		apperr.Write(w, apperr.RateLimited("too many attempts; try again later"))
		return
	}
	apperr.Write(w, apperr.ErrUnauthorized)
}

func (s *Server) finishGoogleLogin(w http.ResponseWriter, r *http.Request, acct *store.Account, deviceName, state string) {
	count, err := s.st.CountActiveDevices(r.Context(), acct.ID)
	if err != nil {
		_ = s.st.ReopenOAuthLogin(r.Context(), state)
		apperr.Write(w, err)
		return
	}
	if count >= s.cfg.MaxDevices {
		_ = s.st.ReopenOAuthLogin(r.Context(), state)
		apperr.Write(w, apperr.New(http.StatusForbidden, "device_limit",
			"device limit reached; revoke a device first"))
		return
	}
	if strings.TrimSpace(deviceName) == "" {
		deviceName = "Notepad++ device"
	}
	dev, err := s.st.CreateDevice(r.Context(), acct.ID, deviceName)
	if err != nil {
		_ = s.st.ReopenOAuthLogin(r.Context(), state)
		apperr.Write(w, err)
		return
	}
	// From here the device exists. Token minting matches password login:
	// a failure does not reopen the browser session (that would create a
	// second device on retry).
	s.respondWithTokens(w, r, acct.ID, dev.ID)
}

func (s *Server) exchangeGoogleCode(ctx context.Context, code, verifier string) (string, error) {
	form := url.Values{}
	form.Set("code", code)
	form.Set("client_id", s.cfg.GoogleClientID)
	form.Set("client_secret", s.cfg.GoogleClientSecret)
	form.Set("redirect_uri", s.cfg.GoogleRedirectURIOrDefault())
	form.Set("grant_type", "authorization_code")
	form.Set("code_verifier", verifier)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.cfg.GoogleTokenURL, strings.NewReader(form.Encode()))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Accept", "application/json")
	resp, err := s.googleHTTP.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return "", err
	}
	var tr struct {
		IDToken string `json:"id_token"`
		Error   string `json:"error"`
	}
	_ = json.Unmarshal(body, &tr)
	if resp.StatusCode != http.StatusOK || tr.IDToken == "" {
		if tr.Error == "" {
			tr.Error = "no_id_token"
		}
		return "", errors.New(tr.Error)
	}
	return tr.IDToken, nil
}

func (s *Server) verifyGoogleIDToken(ctx context.Context, idToken, nonce string) (*auth.GoogleIdentity, error) {
	kid, err := auth.RS256Kid(idToken)
	if err != nil {
		return nil, err
	}
	pub, err := s.jwks.Key(ctx, kid)
	if err != nil {
		return nil, err
	}
	ident, err := auth.VerifyRS256IDToken(idToken, pub, s.now(), s.cfg.GoogleIssuer, s.cfg.GoogleClientID, nonce)
	if err != nil {
		return nil, err
	}
	if !validGoogleSub(ident.Subject) {
		return nil, errors.New("google subject rejected")
	}
	if !validEmail(normalizeEmail(ident.Email)) {
		return nil, errors.New("google email rejected")
	}
	return ident, nil
}

// accountForGoogle finds or creates the NPSync account for a verified Google
// identity. Encryption keys are not involved: this only links the subject.
func (s *Server) accountForGoogle(ctx context.Context, sub, email string) (*store.Account, error) {
	return s.accountForGoogleRetry(ctx, sub, email, false)
}

func (s *Server) accountForGoogleRetry(ctx context.Context, sub, email string, retried bool) (*store.Account, error) {
	acct, err := s.st.AccountByGoogleSub(ctx, sub)
	if err == nil {
		return acct, nil
	}
	if !errors.Is(err, store.ErrNotFound) {
		return nil, err
	}
	acct, err = s.st.AccountByEmail(ctx, email)
	if err == nil {
		if acct.GoogleSub != "" && acct.GoogleSub != sub {
			return nil, apperr.New(http.StatusConflict, "google_email_taken",
				"this email is already linked to a different Google account")
		}
		if acct.GoogleSub == sub {
			return acct, nil
		}
		if err := s.st.LinkGoogleSubject(ctx, acct.ID, sub); err != nil {
			if errors.Is(err, store.ErrDuplicate) && !retried {
				return s.accountForGoogleRetry(ctx, sub, email, true)
			}
			if errors.Is(err, store.ErrConflict) {
				return nil, apperr.New(http.StatusConflict, "google_email_taken",
					"this email is already linked to a different Google account")
			}
			return nil, err
		}
		acct.GoogleSub = sub
		return acct, nil
	}
	if !errors.Is(err, store.ErrNotFound) {
		return nil, err
	}
	if !s.cfg.RegistrationOpen {
		return nil, apperr.New(http.StatusForbidden, "registration_closed",
			"registration is disabled on this server")
	}
	acct, err = s.st.CreateGoogleAccount(ctx, email, sub)
	if err != nil && (errors.Is(err, store.ErrEmailTaken) || errors.Is(err, store.ErrDuplicate)) && !retried {
		return s.accountForGoogleRetry(ctx, sub, email, true)
	}
	return acct, err
}

func oauthStatusError(code, message string) *apperr.Error {
	status := http.StatusBadRequest
	switch code {
	case "registration_closed":
		status = http.StatusForbidden
	case "google_email_taken":
		status = http.StatusConflict
	case "login_expired":
		status = http.StatusGone
	case "google_sso_disabled":
		status = http.StatusServiceUnavailable
	case "internal":
		status = http.StatusInternalServerError
	}
	return apperr.New(status, fallback(code, "google_signin_failed"), fallback(message, "Google sign-in failed"))
}

func fallback(s, def string) string {
	if strings.TrimSpace(s) == "" {
		return def
	}
	return s
}

func validGoogleSub(s string) bool {
	if len(s) < 1 || len(s) > 255 {
		return false
	}
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
		case r == '.', r == '_', r == '-':
		default:
			return false
		}
	}
	return true
}

func writeGooglePage(w http.ResponseWriter, ok bool, message string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Content-Security-Policy", "default-src 'none'")
	w.Header().Set("Cache-Control", "no-store")
	title := "Sign-in failed"
	status := http.StatusBadRequest
	if ok {
		title = "Signed in"
		status = http.StatusOK
	}
	w.WriteHeader(status)
	_, _ = w.Write([]byte("<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><title>Notepad++ Sync</title></head><body><h1>" +
		html.EscapeString(title) + "</h1><p>" + html.EscapeString(message) + "</p></body></html>"))
}
