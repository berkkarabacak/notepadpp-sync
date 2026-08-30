package auth

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

func sha256Sum(b []byte) []byte {
	s := sha256.Sum256(b)
	return s[:]
}

// AccessClaims are the claims carried by the signed access token.
type AccessClaims struct {
	AccountID string `json:"sub"`
	DeviceID  string `json:"dev"`
	ExpiresAt int64  `json:"exp"`
	IssuedAt  int64  `json:"iat"`
}

// TokenSigner creates and verifies short-lived access tokens.
// Implementation: compact JWS-style HS256 (header.payload.signature).
type TokenSigner struct {
	key []byte
	ttl time.Duration
	now func() time.Time // overridable in tests
}

func NewTokenSigner(key []byte, ttl time.Duration) *TokenSigner {
	return &TokenSigner{key: key, ttl: ttl, now: time.Now}
}

// Mint creates a signed access token for (accountID, deviceID).
func (s *TokenSigner) Mint(accountID, deviceID string) (token string, expiresAt time.Time, err error) {
	now := s.now()
	exp := now.Add(s.ttl)
	claims := AccessClaims{
		AccountID: accountID,
		DeviceID:  deviceID,
		IssuedAt:  now.Unix(),
		ExpiresAt: exp.Unix(),
	}
	header := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))
	payloadBytes, err := json.Marshal(claims)
	if err != nil {
		return "", time.Time{}, err
	}
	payload := base64.RawURLEncoding.EncodeToString(payloadBytes)
	sig := s.sign(header + "." + payload)
	return header + "." + payload + "." + sig, exp, nil
}

// Verify validates a token and returns its claims.
func (s *TokenSigner) Verify(token string) (*AccessClaims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil, errors.New("malformed token")
	}
	expected := s.sign(parts[0] + "." + parts[1])
	if !hmac.Equal([]byte(expected), []byte(parts[2])) {
		return nil, errors.New("bad signature")
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return nil, errors.New("malformed payload")
	}
	var c AccessClaims
	if err := json.Unmarshal(payload, &c); err != nil {
		return nil, errors.New("malformed claims")
	}
	if c.AccountID == "" || c.DeviceID == "" {
		return nil, errors.New("missing claims")
	}
	if s.now().Unix() >= c.ExpiresAt {
		return nil, errors.New("token expired")
	}
	return &c, nil
}

func (s *TokenSigner) sign(data string) string {
	m := hmac.New(sha256.New, s.key)
	m.Write([]byte(data))
	return base64.RawURLEncoding.EncodeToString(m.Sum(nil))
}

// NewUUID returns a random RFC 4122 v4 UUID string.
func NewUUID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}
