package auth

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"math/big"
	"strings"
	"testing"
	"time"
)

func testRSA(t *testing.T) *rsa.PrivateKey {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	return key
}

func signRS256(t *testing.T, key *rsa.PrivateKey, kid string, claims map[string]any) string {
	t.Helper()
	return signRS256Alg(t, key, kid, "RS256", claims)
}

func signRS256Alg(t *testing.T, key *rsa.PrivateKey, kid, alg string, claims map[string]any) string {
	t.Helper()
	hb, err := json.Marshal(map[string]string{"alg": alg, "typ": "JWT", "kid": kid})
	if err != nil {
		t.Fatal(err)
	}
	pb, err := json.Marshal(claims)
	if err != nil {
		t.Fatal(err)
	}
	h := base64.RawURLEncoding.EncodeToString(hb)
	p := base64.RawURLEncoding.EncodeToString(pb)
	signing := h + "." + p
	if alg == "none" {
		return signing + "."
	}
	sum := sha256.Sum256([]byte(signing))
	sig, err := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, sum[:])
	if err != nil {
		t.Fatal(err)
	}
	return signing + "." + base64.RawURLEncoding.EncodeToString(sig)
}

func goodClaims(nonce string) map[string]any {
	return map[string]any{
		"iss":            "https://accounts.google.com",
		"sub":            "google-sub-1",
		"aud":            "test-google-client-id",
		"exp":            time.Now().Add(5 * time.Minute).Unix(),
		"iat":            time.Now().Add(-time.Minute).Unix(),
		"nonce":          nonce,
		"email":          "user@example.com",
		"email_verified": true,
	}
}

func TestPKCEChallengeS256(t *testing.T) {
	verifier, challenge, err := NewCodeVerifier()
	if err != nil {
		t.Fatal(err)
	}
	if len(verifier) < 43 || len(verifier) > 128 {
		t.Fatalf("verifier length %d", len(verifier))
	}
	if challenge != PKCEChallengeS256(verifier) {
		t.Fatal("challenge does not match verifier")
	}
	if strings.Contains(challenge, "=") || strings.Contains(verifier, "=") {
		t.Fatal("pkce values must be unpadded base64url")
	}
	if challenge == verifier {
		t.Fatal("challenge must not equal the verifier")
	}
}

func TestVerifyRS256IDToken(t *testing.T) {
	key := testRSA(t)
	const nonce = "nonce-value-1"
	tok := signRS256(t, key, "kid-1", goodClaims(nonce))
	got, err := VerifyRS256IDToken(tok, &key.PublicKey, time.Now(), "https://accounts.google.com", "test-google-client-id", nonce)
	if err != nil {
		t.Fatal(err)
	}
	if got.Subject != "google-sub-1" || got.Email != "user@example.com" || !got.EmailVerified {
		t.Fatalf("identity: %+v", got)
	}

	// Google documents both issuer spellings.
	alt := goodClaims(nonce)
	alt["iss"] = "accounts.google.com"
	tok = signRS256(t, key, "kid-1", alt)
	if _, err := VerifyRS256IDToken(tok, &key.PublicKey, time.Now(), "https://accounts.google.com", "test-google-client-id", nonce); err != nil {
		t.Fatal(err)
	}
}

func TestVerifyRS256IDTokenRejects(t *testing.T) {
	key := testRSA(t)
	other := testRSA(t)
	const nonce = "nonce-value-1"
	now := time.Now()
	valid := signRS256(t, key, "kid-1", goodClaims(nonce))

	cases := []struct {
		name  string
		token string
		nonce string
		aud   string
		iss   string
		when  time.Time
	}{
		{name: "bad signature", token: signRS256(t, other, "kid-1", goodClaims(nonce)), nonce: nonce, aud: "test-google-client-id", iss: "https://accounts.google.com", when: now},
		{name: "wrong aud", token: valid, nonce: nonce, aud: "other-client", iss: "https://accounts.google.com", when: now},
		{name: "wrong nonce", token: valid, nonce: "other-nonce", aud: "test-google-client-id", iss: "https://accounts.google.com", when: now},
		{name: "wrong iss", token: valid, nonce: nonce, aud: "test-google-client-id", iss: "https://evil.example", when: now},
		{name: "expired", token: func() string {
			c := goodClaims(nonce)
			c["exp"] = now.Add(-2 * time.Hour).Unix()
			c["iat"] = now.Add(-3 * time.Hour).Unix()
			return signRS256(t, key, "kid-1", c)
		}(), nonce: nonce, aud: "test-google-client-id", iss: "https://accounts.google.com", when: now},
		{name: "unverified email", token: func() string {
			c := goodClaims(nonce)
			c["email_verified"] = false
			return signRS256(t, key, "kid-1", c)
		}(), nonce: nonce, aud: "test-google-client-id", iss: "https://accounts.google.com", when: now},
		{name: "alg none", token: signRS256Alg(t, key, "kid-1", "none", goodClaims(nonce)), nonce: nonce, aud: "test-google-client-id", iss: "https://accounts.google.com", when: now},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := VerifyRS256IDToken(tc.token, &key.PublicKey, tc.when, tc.iss, tc.aud, tc.nonce); err == nil {
				t.Fatal("accepted")
			}
		})
	}

	if _, err := RS256Kid(signRS256Alg(t, key, "kid-1", "none", goodClaims(nonce))); err == nil {
		t.Fatal("alg none returned a kid")
	}
}

func TestJWKSRejectsSmallKey(t *testing.T) {
	// A 512-bit modulus must not be accepted even if the JSON parses.
	n := new(big.Int).Lsh(big.NewInt(1), 512)
	body, _ := json.Marshal(map[string]any{
		"keys": []map[string]string{{
			"kty": "RSA",
			"kid": "small",
			"use": "sig",
			"alg": "RS256",
			"n":   base64.RawURLEncoding.EncodeToString(n.Bytes()),
			"e":   base64.RawURLEncoding.EncodeToString(big.NewInt(65537).Bytes()),
		}},
	})
	if _, err := parseJWKS(body); err == nil {
		t.Fatal("small rsa key accepted")
	}
}
