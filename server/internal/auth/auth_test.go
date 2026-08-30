package auth

import (
	"strings"
	"testing"
	"time"
)

func TestHashAndVerifyPassword(t *testing.T) {
	h, err := HashPassword("correct horse battery 9")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(h, "$argon2id$") {
		t.Fatalf("unexpected hash format: %s", h)
	}
	ok, err := VerifyPassword(h, "correct horse battery 9")
	if err != nil || !ok {
		t.Fatalf("valid password rejected: %v", err)
	}
	ok, err = VerifyPassword(h, "wrong password 123")
	if err != nil || ok {
		t.Fatalf("invalid password accepted")
	}
}

func TestPasswordPolicy(t *testing.T) {
	cases := map[string]bool{
		"short1":          false, // too short
		"onlylettershere": false, // no digit
		"12345678901":     false, // no letter
		"goodPassw0rd":    true,
		"another-ok-123":  true,
	}
	for pw, want := range cases {
		if err := ValidatePasswordPolicy(pw); (err == nil) != want {
			t.Errorf("password %q: want valid=%v", pw, want)
		}
	}
}

func TestTokenRoundTrip(t *testing.T) {
	key := make([]byte, 32)
	for i := range key {
		key[i] = byte(i)
	}
	s := NewTokenSigner(key, time.Minute)
	tok, _, err := s.Mint("acct-1", "dev-1")
	if err != nil {
		t.Fatal(err)
	}
	c, err := s.Verify(tok)
	if err != nil {
		t.Fatal(err)
	}
	if c.AccountID != "acct-1" || c.DeviceID != "dev-1" {
		t.Fatalf("claims mismatch: %+v", c)
	}
}

func TestTokenExpired(t *testing.T) {
	key := make([]byte, 32)
	s := NewTokenSigner(key, time.Minute)
	// Force "now" far into the future after minting.
	tok, _, err := s.Mint("a", "d")
	if err != nil {
		t.Fatal(err)
	}
	s.now = func() time.Time { return time.Now().Add(2 * time.Minute) }
	if _, err := s.Verify(tok); err == nil {
		t.Fatal("expired token accepted")
	}
}

func TestTokenTampered(t *testing.T) {
	key := make([]byte, 32)
	s := NewTokenSigner(key, time.Minute)
	tok, _, _ := s.Mint("a", "d")
	// Flip a character in the payload.
	parts := strings.Split(tok, ".")
	payload := []byte(parts[1])
	if payload[0] == 'A' {
		payload[0] = 'B'
	} else {
		payload[0] = 'A'
	}
	parts[1] = string(payload)
	if _, err := s.Verify(strings.Join(parts, ".")); err == nil {
		t.Fatal("tampered token accepted")
	}
}

func TestOpaqueTokenHashing(t *testing.T) {
	tok, hash, err := NewOpaqueToken()
	if err != nil {
		t.Fatal(err)
	}
	if tok == "" || len(hash) != 64 {
		t.Fatalf("bad token/hash: %d/%d", len(tok), len(hash))
	}
	if HashTokenSHA256(tok) != hash {
		t.Fatal("hash mismatch")
	}
	_, hash2, _ := NewOpaqueToken()
	if hash == hash2 {
		t.Fatal("tokens not unique")
	}
}

func TestUUIDShape(t *testing.T) {
	u := NewUUID()
	if len(u) != 36 || u[8] != '-' || u[14] != '4' {
		t.Fatalf("not a v4 uuid: %s", u)
	}
}
