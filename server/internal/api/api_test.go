package api

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"npsync/server/internal/auth"
	"npsync/server/internal/blob"
	"npsync/server/internal/config"
	"npsync/server/internal/store"
)

// ---- test harness ----

type testEnv struct {
	t      *testing.T
	server *httptest.Server
	st     store.Store
	cfg    *config.Config
	uuidN  int
}

func newTestEnv(t *testing.T) *testEnv {
	t.Helper()
	cfg := &config.Config{
		BaseURL:           "http://test",
		TokenSigningKey:   bytes.Repeat([]byte{7}, 32),
		AccessTokenTTL:    time.Minute,
		RefreshTokenTTL:   time.Hour,
		MaxFileBytes:      1 << 20,
		MaxBatchBytes:     4 << 20,
		MaxDevices:        10,
		VersionRetention:  30,
		RegistrationOpen:  true,
		LoginRatePerMin:   1000,
		LoginLockoutAfter: 3,
		LoginLockoutFor:   time.Minute,
	}
	blobs, err := blob.NewFS(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	uuidN := 0
	st := store.NewMem(func() string {
		uuidN++
		return fmt.Sprintf("11111111-2222-4333-8444-%012d", uuidN)
	})
	srv := NewServer(cfg, st, blobs)
	env := &testEnv{t: t, server: httptest.NewServer(srv.Handler()), st: st, cfg: cfg}
	t.Cleanup(env.server.Close)
	return env
}

// testClient simulates one device (one plugin instance).
type testClient struct {
	env          *testEnv
	email        string
	DeviceID     string
	AccountID    string
	AccessToken  string
	RefreshToken string
}

func (e *testEnv) newClient(email, password, deviceName string) *testClient {
	e.t.Helper()
	c := &testClient{env: e, email: email}
	resp := c.do("POST", "/auth/register", map[string]string{
		"email": email, "password": password, "device_name": deviceName,
	}, "")
	if resp.StatusCode != http.StatusOK {
		e.t.Fatalf("register failed: %d %s", resp.StatusCode, resp.body)
	}
	var tr tokenResponse
	mustJSON(e.t, resp.body, &tr)
	c.DeviceID, c.AccountID = tr.DeviceID, tr.AccountID
	c.AccessToken, c.RefreshToken = tr.AccessToken, tr.RefreshToken
	return c
}

type httpResp struct {
	StatusCode int
	body       []byte
	header     http.Header
}

func (c *testClient) do(method, path string, body any, token string) *httpResp {
	var rdr io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rdr = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, c.env.server.URL+path, rdr)
	if err != nil {
		c.env.t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-NPSync-Protocol", "1")
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		c.env.t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return &httpResp{StatusCode: resp.StatusCode, body: b, header: resp.Header}
}

func (c *testClient) authed(method, path string, body any) *httpResp {
	return c.do(method, path, body, c.AccessToken)
}

// fakeCipher builds a deterministic "ciphertext" payload with a correct
// content hash (the server never sees plaintext, so tests just need bytes
// that hash consistently).
func fakeCipher(seed string) (b64, hash string, size int64) {
	raw := bytes.Repeat([]byte(seed), 8)
	return base64.RawURLEncoding.EncodeToString(raw),
		auth.HashTokenSHA256(string(raw)),
		int64(len(raw))
}

func mustJSON(t *testing.T, b []byte, v any) {
	t.Helper()
	if err := json.Unmarshal(b, v); err != nil {
		t.Fatalf("bad json %q: %v", b, err)
	}
}

func (c *testClient) uploadFile(fileID, seed string, baseVersion int) *httpResp {
	b64, hash, size := fakeCipher(seed)
	return c.authed("PUT", "/sync/files/"+fileID, fileJSON{
		FileID:            fileID,
		EncryptedMetadata: base64.RawURLEncoding.EncodeToString([]byte(`{"relative_path":"notes.txt"}`)),
		EncryptedContent:  b64,
		ContentHash:       hash,
		BaseVersion:       baseVersion,
		VersionVector:     map[string]int{c.DeviceID: baseVersion + 1},
		Size:              size,
		ModifiedAt:        time.Now().UTC(),
	})
}

func (c *testClient) createFile(fileID, seed string) *httpResp {
	b64, hash, size := fakeCipher(seed)
	return c.authed("POST", "/sync/files", fileJSON{
		FileID:            fileID,
		EncryptedMetadata: base64.RawURLEncoding.EncodeToString([]byte(`{"relative_path":"notes.txt"}`)),
		EncryptedContent:  b64,
		ContentHash:       hash,
		VersionVector:     map[string]int{c.DeviceID: 1},
		Size:              size,
		ModifiedAt:        time.Now().UTC(),
		IdempotencyKey:    auth.NewUUID(),
	})
}

// ---- auth tests ----

func TestRegisterLoginRefreshFlow(t *testing.T) {
	env := newTestEnv(t)
	c := env.newClient("user@example.com", "passw0rd-123", "Laptop A")
	if c.AccessToken == "" || c.RefreshToken == "" {
		t.Fatal("no tokens issued")
	}

	// Wrong password -> 401.
	resp := c.do("POST", "/auth/login", map[string]string{
		"email": "user@example.com", "password": "wr0ng-pass", "device_name": "x",
	}, "")
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("wrong password: got %d", resp.StatusCode)
	}

	// Correct password -> 200 with fresh device.
	resp = c.do("POST", "/auth/login", map[string]string{
		"email": "user@example.com", "password": "passw0rd-123", "device_name": "Laptop B",
	}, "")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("login failed: %d %s", resp.StatusCode, resp.body)
	}
	var tr tokenResponse
	mustJSON(t, resp.body, &tr)

	// Refresh rotation: old token dies on rotation.
	resp = c.do("POST", "/auth/refresh", map[string]string{"refresh_token": tr.RefreshToken}, "")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("refresh failed: %d", resp.StatusCode)
	}
	var tr2 tokenResponse
	mustJSON(t, resp.body, &tr2)
	resp = c.do("POST", "/auth/refresh", map[string]string{"refresh_token": tr.RefreshToken}, "")
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatal("reused refresh token accepted")
	}
}

func TestRegisterRejectsWeakPassword(t *testing.T) {
	env := newTestEnv(t)
	c := &testClient{env: env}
	resp := c.do("POST", "/auth/register", map[string]string{
		"email": "weak@example.com", "password": "short", "device_name": "d",
	}, "")
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("weak password accepted: %d", resp.StatusCode)
	}
}

func TestAccountLockout(t *testing.T) {
	env := newTestEnv(t)
	env.newClient("lock@example.com", "passw0rd-123", "L")
	c := &testClient{env: env}
	for i := 0; i < 3; i++ {
		c.do("POST", "/auth/login", map[string]string{
			"email": "lock@example.com", "password": "wr0ng-pass1", "device_name": "d",
		}, "")
	}
	// Next attempt — even with the right password — is locked out.
	resp := c.do("POST", "/auth/login", map[string]string{
		"email": "lock@example.com", "password": "passw0rd-123", "device_name": "d",
	}, "")
	if resp.StatusCode != http.StatusTooManyRequests {
		t.Fatalf("expected lockout 429, got %d", resp.StatusCode)
	}
}

func TestDeviceRevocation(t *testing.T) {
	env := newTestEnv(t)
	a := env.newClient("rev@example.com", "passw0rd-123", "A")
	resp := a.do("POST", "/auth/login", map[string]string{
		"email": "rev@example.com", "password": "passw0rd-123", "device_name": "B",
	}, "")
	var tr tokenResponse
	mustJSON(t, resp.body, &tr)
	b := &testClient{env: env, AccessToken: tr.AccessToken, RefreshToken: tr.RefreshToken, DeviceID: tr.DeviceID}

	// A lists devices, revokes B.
	resp = a.authed("GET", "/devices", nil)
	if resp.StatusCode != http.StatusOK || !strings.Contains(string(resp.body), tr.DeviceID) {
		t.Fatalf("device list: %d %s", resp.StatusCode, resp.body)
	}
	resp = a.authed("DELETE", "/devices/"+b.DeviceID, nil)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("revoke failed: %d", resp.StatusCode)
	}
	// B's access token now fails, and its refresh token is dead too.
	resp = b.authed("GET", "/devices", nil)
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("revoked device token still works: %d", resp.StatusCode)
	}
	resp = b.do("POST", "/auth/refresh", map[string]string{"refresh_token": b.RefreshToken}, "")
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("revoked device refresh still works: %d", resp.StatusCode)
	}
}

// ---- sync tests ----

func TestUploadDownloadRoundTrip(t *testing.T) {
	env := newTestEnv(t)
	c := env.newClient("sync@example.com", "passw0rd-123", "A")
	fileID := auth.NewUUID()

	resp := c.createFile(fileID, "hello-world-v1")
	if resp.StatusCode != http.StatusCreated {
		t.Fatalf("create: %d %s", resp.StatusCode, resp.body)
	}
	var created fileJSON
	mustJSON(t, resp.body, &created)
	if created.Version != 1 || created.ChangeSeq == 0 {
		t.Fatalf("bad create response: %+v", created)
	}

	resp = c.authed("GET", "/sync/files/"+fileID, nil)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("get: %d", resp.StatusCode)
	}
	var got fileJSON
	mustJSON(t, resp.body, &got)
	b64, _, _ := fakeCipher("hello-world-v1")
	if got.EncryptedContent != b64 {
		t.Fatal("ciphertext mismatch on download")
	}
}

func TestIdempotentCreate(t *testing.T) {
	env := newTestEnv(t)
	c := env.newClient("idem@example.com", "passw0rd-123", "A")
	fileID := auth.NewUUID()
	key := auth.NewUUID()

	b64, hash, size := fakeCipher("v1")
	body := fileJSON{
		FileID: fileID, EncryptedContent: b64, ContentHash: hash,
		EncryptedMetadata: base64.RawURLEncoding.EncodeToString([]byte("m")),
		VersionVector: map[string]int{c.DeviceID: 1}, Size: size,
		ModifiedAt: time.Now().UTC(), IdempotencyKey: key,
	}
	r1 := c.authed("POST", "/sync/files", body)
	r2 := c.authed("POST", "/sync/files", body)
	if r1.StatusCode != http.StatusCreated || r2.StatusCode != http.StatusOK {
		t.Fatalf("idempotent replay: %d then %d", r1.StatusCode, r2.StatusCode)
	}
	// Only one file record exists.
	resp := c.authed("GET", "/sync/files", nil)
	var list struct {
		Files []fileJSON `json:"files"`
	}
	mustJSON(t, resp.body, &list)
	if len(list.Files) != 1 {
		t.Fatalf("duplicate file created despite idempotency: %d", len(list.Files))
	}
}

// TestTwoClientConflictSimulation is the acceptance scenario from the spec:
// Laptop A and Laptop B both edit the same base version; the second upload
// conflicts, nothing is lost, and a merge-based write resolves it.
func TestTwoClientConflictSimulation(t *testing.T) {
	env := newTestEnv(t)
	a := env.newClient("ab@example.com", "passw0rd-123", "Laptop-A")
	fileID := auth.NewUUID()

	// A creates v1; B "logs in" on the same account and downloads v1.
	if r := a.createFile(fileID, "base-v1"); r.StatusCode != http.StatusCreated {
		t.Fatalf("create: %d %s", r.StatusCode, r.body)
	}
	resp := a.do("POST", "/auth/login", map[string]string{
		"email": "ab@example.com", "password": "passw0rd-123", "device_name": "Laptop-B",
	}, "")
	var bTokens tokenResponse
	mustJSON(t, resp.body, &bTokens)
	b := &testClient{env: env, AccessToken: bTokens.AccessToken, DeviceID: bTokens.DeviceID}

	resp = b.authed("GET", "/sync/files/"+fileID, nil)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("B download v1: %d", resp.StatusCode)
	}

	// A and B both edit v1 offline-style and upload.
	rA := a.uploadFile(fileID, "edit-from-A-v2", 1)
	if rA.StatusCode != http.StatusOK {
		t.Fatalf("A v2A rejected: %d %s", rA.StatusCode, rA.body)
	}
	rB := b.uploadFile(fileID, "edit-from-B-v2", 1)
	if rB.StatusCode != http.StatusConflict {
		t.Fatalf("B v2B must conflict, got %d %s", rB.StatusCode, rB.body)
	}
	// 409 carries the server's current record for three-way merge.
	var conflictErr struct {
		Error   string   `json:"error"`
		Current fileJSON `json:"current"`
	}
	mustJSON(t, rB.body, &conflictErr)
	if conflictErr.Error != "conflict" || conflictErr.Current.Version != 2 {
		t.Fatalf("bad conflict payload: %s", rB.body)
	}

	// B merges locally and uploads the merge based on the new head.
	rMerge := b.uploadFile(fileID, "merged-v3", 2)
	if rMerge.StatusCode != http.StatusOK {
		t.Fatalf("merge upload rejected: %d %s", rMerge.StatusCode, rMerge.body)
	}

	// History preserves every accepted version; nothing is lost.
	resp = a.authed("GET", "/sync/files/"+fileID+"/versions", nil)
	var vs struct {
		Versions []struct {
			Version int `json:"version"`
		} `json:"versions"`
	}
	mustJSON(t, resp.body, &vs)
	if len(vs.Versions) != 3 {
		t.Fatalf("want 3 versions preserved, got %d", len(vs.Versions))
	}

	// A's change feed shows B's winning write so A catches up.
	resp = a.authed("GET", "/sync/changes?since=1", nil)
	var ch struct {
		Changes []struct {
			FileID  string `json:"file_id"`
			Version int    `json:"version"`
		} `json:"changes"`
	}
	mustJSON(t, resp.body, &ch)
	if len(ch.Changes) == 0 || ch.Changes[len(ch.Changes)-1].Version != 3 {
		t.Fatalf("change feed missing final state: %s", resp.body)
	}
}

func TestDeletePropagatesAsTombstone(t *testing.T) {
	env := newTestEnv(t)
	c := env.newClient("del@example.com", "passw0rd-123", "A")
	fileID := auth.NewUUID()
	if r := c.createFile(fileID, "x"); r.StatusCode != http.StatusCreated {
		t.Fatalf("create: %d", r.StatusCode)
	}
	r := c.authed("DELETE", "/sync/files/"+fileID, nil)
	if r.StatusCode != http.StatusOK {
		t.Fatalf("delete: %d %s", r.StatusCode, r.body)
	}
	var rec fileJSON
	mustJSON(t, r.body, &rec)
	if !rec.Deleted || rec.Version != 2 {
		t.Fatalf("tombstone wrong: %+v", rec)
	}
	// File still listed (as tombstone) so other devices learn of deletion.
	resp := c.authed("GET", "/sync/files", nil)
	if !strings.Contains(string(resp.body), `"deleted":true`) {
		t.Fatalf("tombstone not visible in list: %s", resp.body)
	}
}

func TestOversizedUploadRejected(t *testing.T) {
	env := newTestEnv(t)
	c := env.newClient("big@example.com", "passw0rd-123", "A")
	fileID := auth.NewUUID()
	big := bytes.Repeat([]byte("z"), int(env.cfg.MaxFileBytes)+1)
	body := fileJSON{
		FileID: fileID,
		EncryptedMetadata: base64.RawURLEncoding.EncodeToString([]byte("m")),
		EncryptedContent:  base64.RawURLEncoding.EncodeToString(big),
		ContentHash:       auth.HashTokenSHA256(string(big)),
		VersionVector:     map[string]int{c.DeviceID: 1},
		Size:              int64(len(big)),
		ModifiedAt:        time.Now().UTC(),
	}
	r := c.authed("POST", "/sync/files", body)
	if r.StatusCode != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversized upload: got %d", r.StatusCode)
	}
}

func TestProtocolVersionMismatch(t *testing.T) {
	env := newTestEnv(t)
	req, _ := http.NewRequest("GET", env.server.URL+"/sync/files", nil)
	req.Header.Set("X-NPSync-Protocol", "99")
	req.Header.Set("Authorization", "Bearer whatever")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusUpgradeRequired {
		t.Fatalf("want 426, got %d", resp.StatusCode)
	}
}

func TestRestoreVersion(t *testing.T) {
	env := newTestEnv(t)
	c := env.newClient("rst@example.com", "passw0rd-123", "A")
	fileID := auth.NewUUID()
	c.createFile(fileID, "version-one")
	c.uploadFile(fileID, "version-two", 1)

	r := c.authed("POST", "/sync/files/"+fileID+"/restore", map[string]any{"version": 1})
	if r.StatusCode != http.StatusOK {
		t.Fatalf("restore: %d %s", r.StatusCode, r.body)
	}
	var rec fileJSON
	mustJSON(t, r.body, &rec)
	if rec.Version != 3 {
		t.Fatalf("restore should create new head v3, got %d", rec.Version)
	}
	// Content equals v1's content.
	resp := c.authed("GET", "/sync/files/"+fileID, nil)
	var got fileJSON
	mustJSON(t, resp.body, &got)
	b64, _, _ := fakeCipher("version-one")
	if got.EncryptedContent != b64 {
		t.Fatal("restored content mismatch")
	}
}

func TestPairingFlow(t *testing.T) {
	env := newTestEnv(t)
	newDev := env.newClient("pair@example.com", "passw0rd-123", "New-Laptop")

	// New device requests a code.
	r := newDev.authed("POST", "/devices/pair", map[string]string{"action": "request"})
	if r.StatusCode != http.StatusOK {
		t.Fatalf("pair request: %d", r.StatusCode)
	}
	var req struct {
		PairingCode string `json:"pairing_code"`
	}
	mustJSON(t, r.body, &req)

	// Existing device approves with a wrapped key.
	resp := newDev.do("POST", "/auth/login", map[string]string{
		"email": "pair@example.com", "password": "passw0rd-123", "device_name": "Old-Laptop",
	}, "")
	var oldTokens tokenResponse
	mustJSON(t, resp.body, &oldTokens)
	old := &testClient{env: env, AccessToken: oldTokens.AccessToken, DeviceID: oldTokens.DeviceID}

	wrapped := base64.RawURLEncoding.EncodeToString([]byte("wrapped-master-key-material"))
	r = old.authed("POST", "/devices/pair", map[string]string{
		"action": "approve", "pairing_code": req.PairingCode, "wrapped_master_key": wrapped,
	})
	if r.StatusCode != http.StatusOK {
		t.Fatalf("approve: %d %s", r.StatusCode, r.body)
	}

	// New device polls and receives the wrapped key; code is single-use.
	r = newDev.authed("POST", "/devices/pair", map[string]string{
		"action": "poll", "pairing_code": req.PairingCode,
	})
	if r.StatusCode != http.StatusOK || !strings.Contains(string(r.body), wrapped) {
		t.Fatalf("poll: %d %s", r.StatusCode, r.body)
	}
	r = newDev.authed("POST", "/devices/pair", map[string]string{
		"action": "poll", "pairing_code": req.PairingCode,
	})
	if r.StatusCode != http.StatusGone {
		t.Fatalf("consumed code still pollar: %d", r.StatusCode)
	}
}

func TestSessionRoundTrip(t *testing.T) {
	env := newTestEnv(t)
	c := env.newClient("sess@example.com", "passw0rd-123", "A")
	state := base64.RawURLEncoding.EncodeToString([]byte("encrypted-tabs-cursor"))
	r := c.authed("PUT", "/session", sessionJSON{EncryptedState: state, Version: 1})
	if r.StatusCode != http.StatusOK {
		t.Fatalf("put session: %d", r.StatusCode)
	}
	r = c.authed("GET", "/session", nil)
	var got sessionJSON
	mustJSON(t, r.body, &got)
	if got.EncryptedState != state {
		t.Fatal("session mismatch")
	}
}
