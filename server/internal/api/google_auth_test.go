package api

import (
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"math/big"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"npsync/server/internal/auth"
)

// fakeGoogle is a local stand-in for Google's authorize/token/jwks endpoints.
// Credentials are obvious placeholders, never real Google secrets.
type fakeGoogle struct {
	srv           *httptest.Server
	key           *rsa.PrivateKey
	kid           string
	mu            sync.Mutex
	clientID      string
	clientSecret  string
	redirectURI   string
	challenge     string
	nonce         string
	email         string
	sub           string
	emailVerified bool
	iss           string
	aud           string
	exchanges     int
	lastIDToken   string
}

func newFakeGoogle(t *testing.T) *fakeGoogle {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeGoogle{
		key: key, kid: "test-key-1",
		clientID: "test-google-client-id", clientSecret: "test-google-client-secret",
		email: "user@example.com", sub: "google-sub-1", emailVerified: true,
		iss: "https://accounts.google.com", aud: "test-google-client-id",
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /jwks", f.handleJWKS)
	mux.HandleFunc("POST /token", f.handleToken)
	f.srv = httptest.NewServer(mux)
	t.Cleanup(f.srv.Close)
	return f
}

func (f *fakeGoogle) handleJWKS(w http.ResponseWriter, r *http.Request) {
	n := base64.RawURLEncoding.EncodeToString(f.key.N.Bytes())
	e := base64.RawURLEncoding.EncodeToString(big.NewInt(int64(f.key.E)).Bytes())
	_ = json.NewEncoder(w).Encode(map[string]any{
		"keys": []map[string]string{{
			"kty": "RSA", "kid": f.kid, "use": "sig", "alg": "RS256", "n": n, "e": e,
		}},
	})
}

func (f *fakeGoogle) handleToken(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		http.Error(w, "bad form", http.StatusBadRequest)
		return
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.exchanges++
	if r.Form.Get("client_id") != f.clientID || r.Form.Get("client_secret") != f.clientSecret {
		writeOAuthErr(w, "invalid_client")
		return
	}
	if r.Form.Get("grant_type") != "authorization_code" || r.Form.Get("code") == "" {
		writeOAuthErr(w, "invalid_grant")
		return
	}
	if r.Form.Get("redirect_uri") != f.redirectURI || f.redirectURI == "" {
		writeOAuthErr(w, "invalid_grant")
		return
	}
	if auth.PKCEChallengeS256(r.Form.Get("code_verifier")) != f.challenge || f.challenge == "" {
		writeOAuthErr(w, "invalid_grant")
		return
	}
	now := time.Now()
	claims := map[string]any{
		"iss": f.iss, "sub": f.sub, "aud": f.aud,
		"exp": now.Add(5 * time.Minute).Unix(), "iat": now.Add(-time.Minute).Unix(),
		"nonce": f.nonce, "email": f.email, "email_verified": f.emailVerified,
	}
	f.lastIDToken = signTestIDToken(f.key, f.kid, claims)
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"access_token": "google-access-placeholder",
		"token_type":   "Bearer",
		"expires_in":   3600,
		"id_token":     f.lastIDToken,
	})
}

func writeOAuthErr(w http.ResponseWriter, code string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusBadRequest)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": code})
}

func signTestIDToken(key *rsa.PrivateKey, kid string, claims map[string]any) string {
	hb, _ := json.Marshal(map[string]string{"alg": "RS256", "typ": "JWT", "kid": kid})
	pb, _ := json.Marshal(claims)
	h := base64.RawURLEncoding.EncodeToString(hb)
	p := base64.RawURLEncoding.EncodeToString(pb)
	sum := sha256.Sum256([]byte(h + "." + p))
	sig, err := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, sum[:])
	if err != nil {
		panic(err)
	}
	return h + "." + p + "." + base64.RawURLEncoding.EncodeToString(sig)
}

func enableGoogle(env *testEnv, f *fakeGoogle) {
	env.cfg.GoogleClientID = f.clientID
	env.cfg.GoogleClientSecret = f.clientSecret
	env.cfg.GoogleRedirectURI = env.server.URL + "/auth/google/callback"
	env.cfg.GoogleAuthURL = f.srv.URL + "/auth"
	env.cfg.GoogleTokenURL = f.srv.URL + "/token"
	env.cfg.GoogleJWKSURL = f.srv.URL + "/jwks"
	env.cfg.GoogleIssuer = "https://accounts.google.com"
}

type googleStartBody struct {
	AuthorizationURL string `json:"authorization_url"`
	State            string `json:"state"`
	PollSecret       string `json:"poll_secret"`
	ExpiresIn        int    `json:"expires_in"`
}

func (f *fakeGoogle) arm(t *testing.T, start googleStartBody, redirect string) {
	t.Helper()
	u, err := url.Parse(start.AuthorizationURL)
	if err != nil {
		t.Fatal(err)
	}
	q := u.Query()
	if q.Get("response_type") != "code" || q.Get("code_challenge_method") != "S256" {
		t.Fatalf("authorization query: %s", start.AuthorizationURL)
	}
	if q.Get("client_id") != f.clientID {
		t.Fatalf("client_id %q", q.Get("client_id"))
	}
	if q.Get("redirect_uri") != redirect {
		t.Fatalf("redirect_uri %q", q.Get("redirect_uri"))
	}
	if !strings.Contains(q.Get("scope"), "openid") || !strings.Contains(q.Get("scope"), "email") {
		t.Fatalf("scope %q", q.Get("scope"))
	}
	if strings.Contains(start.AuthorizationURL, "client_secret") || strings.Contains(start.AuthorizationURL, "code_verifier=") {
		t.Fatal("authorization URL leaked a secret")
	}
	if q.Get("code_challenge") == "" || q.Get("nonce") == "" || q.Get("state") != start.State {
		t.Fatal("missing pkce or state")
	}
	f.mu.Lock()
	f.challenge = q.Get("code_challenge")
	f.nonce = q.Get("nonce")
	f.redirectURI = q.Get("redirect_uri")
	f.mu.Unlock()
}

func (c *testClient) startGoogle(device string) (googleStartBody, *httpResp) {
	resp := c.do("POST", "/auth/google/start", map[string]string{"device_name": device}, "")
	var body googleStartBody
	if resp.StatusCode == http.StatusOK {
		mustJSON(c.env.t, resp.body, &body)
	}
	return body, resp
}

func (c *testClient) googleCallback(state string) *httpResp {
	return c.do("GET", "/auth/google/callback?code=test-auth-code&state="+url.QueryEscape(state), nil, "")
}

func (c *testClient) pollGoogle(state, secret string) *httpResp {
	return c.do("POST", "/auth/google/poll", map[string]string{"state": state, "poll_secret": secret}, "")
}

func TestGoogleSSODisabled(t *testing.T) {
	env := newTestEnv(t)
	c := &testClient{env: env}
	_, resp := c.startGoogle("Laptop")
	if resp.StatusCode != http.StatusServiceUnavailable || !strings.Contains(string(resp.body), "google_sso_disabled") {
		t.Fatalf("start: %d %s", resp.StatusCode, resp.body)
	}
}

func TestGoogleSSOSignIn(t *testing.T) {
	env := newTestEnv(t)
	fake := newFakeGoogle(t)
	enableGoogle(env, fake)
	c := &testClient{env: env}

	start, resp := c.startGoogle("Office PC")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("start: %d %s", resp.StatusCode, resp.body)
	}
	if start.ExpiresIn != 600 || start.PollSecret == "" {
		t.Fatalf("start body: %+v", start)
	}
	fake.arm(t, start, env.cfg.GoogleRedirectURI)

	pending := c.pollGoogle(start.State, start.PollSecret)
	if pending.StatusCode != http.StatusAccepted {
		t.Fatalf("pending: %d %s", pending.StatusCode, pending.body)
	}

	bad := c.pollGoogle(start.State, "not-the-secret")
	if bad.StatusCode != http.StatusUnauthorized {
		t.Fatalf("bad secret: %d %s", bad.StatusCode, bad.body)
	}

	page := c.googleCallback(start.State)
	if page.StatusCode != http.StatusOK || !strings.Contains(string(page.body), "Signed in") {
		t.Fatalf("callback: %d %s", page.StatusCode, page.body)
	}
	// A second browser hit must not redeem the code again.
	again := c.googleCallback(start.State)
	if again.StatusCode != http.StatusOK {
		t.Fatalf("second callback: %d %s", again.StatusCode, again.body)
	}
	fake.mu.Lock()
	exchanges := fake.exchanges
	idToken := fake.lastIDToken
	fake.mu.Unlock()
	if exchanges != 1 {
		t.Fatalf("code exchanged %d times", exchanges)
	}

	resp = c.pollGoogle(start.State, start.PollSecret)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("poll: %d %s", resp.StatusCode, resp.body)
	}
	if strings.Contains(string(resp.body), idToken) || strings.Contains(string(resp.body), "code_verifier") ||
		strings.Contains(string(resp.body), "google-access-placeholder") {
		t.Fatalf("poll leaked google material: %s", resp.body)
	}
	var tr tokenResponse
	mustJSON(t, resp.body, &tr)
	claims, err := auth.NewTokenSigner(env.cfg.TokenSigningKey, time.Minute).Verify(tr.AccessToken)
	if err != nil || claims.AccountID != tr.AccountID {
		t.Fatalf("npsync token: %v %+v", err, claims)
	}
	acct, err := env.st.AccountByID(context.Background(), tr.AccountID)
	if err != nil || acct.GoogleSub != "google-sub-1" || acct.PasswordHash != "" || acct.Email != "user@example.com" {
		t.Fatalf("account: %+v %v", acct, err)
	}

	// The browser session is single use.
	replay := c.pollGoogle(start.State, start.PollSecret)
	if replay.StatusCode != http.StatusUnauthorized {
		t.Fatalf("replay: %d %s", replay.StatusCode, replay.body)
	}

	// Same Google subject, second device, same account.
	c2 := &testClient{env: env}
	start2, resp := c2.startGoogle("Laptop")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("second start: %d %s", resp.StatusCode, resp.body)
	}
	fake.arm(t, start2, env.cfg.GoogleRedirectURI)
	if page := c2.googleCallback(start2.State); page.StatusCode != http.StatusOK {
		t.Fatalf("second callback: %d %s", page.StatusCode, page.body)
	}
	resp = c2.pollGoogle(start2.State, start2.PollSecret)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("second poll: %d %s", resp.StatusCode, resp.body)
	}
	var tr2 tokenResponse
	mustJSON(t, resp.body, &tr2)
	if tr2.AccountID != tr.AccountID || tr2.DeviceID == tr.DeviceID {
		t.Fatalf("second device: %+v vs %+v", tr2, tr)
	}

	// The issued access token works on an authenticated route.
	authed := &testClient{env: env, AccessToken: tr.AccessToken}
	if resp := authed.authed("GET", "/devices", nil); resp.StatusCode != http.StatusOK {
		t.Fatalf("devices: %d %s", resp.StatusCode, resp.body)
	}
}

func TestGoogleSSOLinksVerifiedEmailAndKeepsPassword(t *testing.T) {
	env := newTestEnv(t)
	fake := newFakeGoogle(t)
	fake.email = "User@Example.com"
	enableGoogle(env, fake)
	owner := env.newClient("user@example.com", "passw0rd-123", "Existing")

	c := &testClient{env: env}
	start, resp := c.startGoogle("Browser")
	if resp.StatusCode != http.StatusOK {
		t.Fatal(resp.StatusCode)
	}
	fake.arm(t, start, env.cfg.GoogleRedirectURI)
	if page := c.googleCallback(start.State); !strings.Contains(string(page.body), "Signed in") {
		t.Fatalf("callback: %s", page.body)
	}
	resp = c.pollGoogle(start.State, start.PollSecret)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("poll: %d %s", resp.StatusCode, resp.body)
	}
	var tr tokenResponse
	mustJSON(t, resp.body, &tr)
	if tr.AccountID != owner.AccountID {
		t.Fatalf("linked account %s, want %s", tr.AccountID, owner.AccountID)
	}
	acct, err := env.st.AccountByEmail(context.Background(), "user@example.com")
	if err != nil || acct.GoogleSub != fake.sub || acct.PasswordHash == "" {
		t.Fatalf("password removed or not linked: %+v %v", acct, err)
	}
	// Password login still works after Google is linked.
	resp = c.do("POST", "/auth/login", map[string]string{
		"email": "user@example.com", "password": "passw0rd-123", "device_name": "Still password",
	}, "")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("password after link: %d %s", resp.StatusCode, resp.body)
	}
}

func TestGoogleSSORejectsUnverifiedEmailAndOtherSubject(t *testing.T) {
	env := newTestEnv(t)
	fake := newFakeGoogle(t)
	fake.emailVerified = false
	enableGoogle(env, fake)
	c := &testClient{env: env}
	start, _ := c.startGoogle("PC")
	fake.arm(t, start, env.cfg.GoogleRedirectURI)
	page := c.googleCallback(start.State)
	if page.StatusCode == http.StatusOK && strings.Contains(string(page.body), "<h1>Signed in</h1>") {
		t.Fatalf("unverified email signed in: %s", page.body)
	}
	resp := c.pollGoogle(start.State, start.PollSecret)
	if resp.StatusCode != http.StatusBadRequest || !strings.Contains(string(resp.body), "google_token_rejected") {
		t.Fatalf("poll: %d %s", resp.StatusCode, resp.body)
	}

	// A verified subject that does not own the email cannot take the account.
	fake.emailVerified = true
	env.newClient("user@example.com", "passw0rd-123", "Owner")
	// Link the real subject first.
	fake.sub = "owner-sub"
	start, resp = c.startGoogle("Owner browser")
	if resp.StatusCode != http.StatusOK {
		t.Fatal(string(resp.body))
	}
	fake.arm(t, start, env.cfg.GoogleRedirectURI)
	_ = c.googleCallback(start.State)
	if resp = c.pollGoogle(start.State, start.PollSecret); resp.StatusCode != http.StatusOK {
		t.Fatalf("owner link: %d %s", resp.StatusCode, resp.body)
	}
	fake.sub = "attacker-sub"
	start, _ = c.startGoogle("Attacker")
	fake.arm(t, start, env.cfg.GoogleRedirectURI)
	_ = c.googleCallback(start.State)
	resp = c.pollGoogle(start.State, start.PollSecret)
	if resp.StatusCode != http.StatusConflict || !strings.Contains(string(resp.body), "google_email_taken") {
		t.Fatalf("mismatch: %d %s", resp.StatusCode, resp.body)
	}
}

func TestGoogleOnlyAccountHasNoPassword(t *testing.T) {
	env := newTestEnv(t)
	fake := newFakeGoogle(t)
	enableGoogle(env, fake)
	c := &testClient{env: env}
	start, _ := c.startGoogle("PC")
	fake.arm(t, start, env.cfg.GoogleRedirectURI)
	if page := c.googleCallback(start.State); page.StatusCode != http.StatusOK {
		t.Fatal(string(page.body))
	}
	if resp := c.pollGoogle(start.State, start.PollSecret); resp.StatusCode != http.StatusOK {
		t.Fatal(string(resp.body))
	}
	for i := 0; i < 4; i++ {
		resp := c.do("POST", "/auth/login", map[string]string{
			"email": "user@example.com", "password": "passw0rd-123", "device_name": "nope",
		}, "")
		if resp.StatusCode != http.StatusUnauthorized {
			t.Fatalf("attempt %d: %d %s", i, resp.StatusCode, resp.body)
		}
	}
}

func TestGoogleRegistrationClosed(t *testing.T) {
	env := newTestEnv(t)
	fake := newFakeGoogle(t)
	enableGoogle(env, fake)
	env.cfg.RegistrationOpen = false
	c := &testClient{env: env}
	start, _ := c.startGoogle("PC")
	fake.arm(t, start, env.cfg.GoogleRedirectURI)
	_ = c.googleCallback(start.State)
	resp := c.pollGoogle(start.State, start.PollSecret)
	if resp.StatusCode != http.StatusForbidden || !strings.Contains(string(resp.body), "registration_closed") {
		t.Fatalf("new account: %d %s", resp.StatusCode, resp.body)
	}

	if _, err := env.st.CreateGoogleAccount(context.Background(), "known@example.com", "known-sub"); err != nil {
		t.Fatal(err)
	}
	fake.email = "known@example.com"
	fake.sub = "known-sub"
	start, _ = c.startGoogle("Known")
	fake.arm(t, start, env.cfg.GoogleRedirectURI)
	if page := c.googleCallback(start.State); page.StatusCode != http.StatusOK {
		t.Fatal(string(page.body))
	}
	resp = c.pollGoogle(start.State, start.PollSecret)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("existing google account: %d %s", resp.StatusCode, resp.body)
	}
}

func TestGoogleLoginExpires(t *testing.T) {
	env := newTestEnv(t)
	fake := newFakeGoogle(t)
	enableGoogle(env, fake)
	c := &testClient{env: env}
	start, resp := c.startGoogle("PC")
	if resp.StatusCode != http.StatusOK {
		t.Fatal(string(resp.body))
	}
	env.api.now = func() time.Time { return time.Now().Add(15 * time.Minute) }
	resp = c.pollGoogle(start.State, start.PollSecret)
	if resp.StatusCode != http.StatusGone || !strings.Contains(string(resp.body), "login_expired") {
		t.Fatalf("expired poll: %d %s", resp.StatusCode, resp.body)
	}
}

func TestGooglePKCEFailure(t *testing.T) {
	env := newTestEnv(t)
	fake := newFakeGoogle(t)
	enableGoogle(env, fake)
	c := &testClient{env: env}
	start, _ := c.startGoogle("PC")
	fake.arm(t, start, env.cfg.GoogleRedirectURI)
	fake.mu.Lock()
	fake.challenge = "not-the-challenge"
	fake.mu.Unlock()
	page := c.googleCallback(start.State)
	if strings.Contains(string(page.body), "<h1>Signed in</h1>") {
		t.Fatalf("pkce failure signed in: %s", page.body)
	}
	resp := c.pollGoogle(start.State, start.PollSecret)
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("poll: %d %s", resp.StatusCode, resp.body)
	}
}
