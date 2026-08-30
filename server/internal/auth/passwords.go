// Package auth implements password hashing (Argon2id) and token handling
// (short-lived HMAC-signed access tokens, opaque rotatable refresh tokens).
package auth

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
)

// Argon2id parameters (OWASP-recommended interactive profile).
const (
	argonTime    = 3
	argonMemory  = 64 * 1024 // KiB
	argonThreads = 2
	argonKeyLen  = 32
	argonSaltLen = 16
)

// HashPassword returns an encoded Argon2id hash string:
// $argon2id$v=19$m=65536,t=3,p=2$<b64 salt>$<b64 hash>
func HashPassword(password string) (string, error) {
	salt := make([]byte, argonSaltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	h := argon2.IDKey([]byte(password), salt, argonTime, argonMemory, argonThreads, argonKeyLen)
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s",
		argon2.Version, argonMemory, argonTime, argonThreads,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(h)), nil
}

// VerifyPassword reports whether password matches the encoded hash.
// It runs in constant time relative to the hash comparison.
func VerifyPassword(encoded, password string) (bool, error) {
	parts := strings.Split(encoded, "$")
	// ["", "argon2id", "v=19", "m=...,t=...,p=...", salt, hash]
	if len(parts) != 6 || parts[1] != "argon2id" {
		return false, errors.New("malformed password hash")
	}
	var mem, time uint32
	var threads uint8
	if _, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &mem, &time, &threads); err != nil {
		return false, errors.New("malformed argon2 parameters")
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil {
		return false, err
	}
	want, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil {
		return false, err
	}
	got := argon2.IDKey([]byte(password), salt, time, mem, threads, uint32(len(want)))
	return subtle.ConstantTimeCompare(got, want) == 1, nil
}

// NewOpaqueToken returns a new 256-bit random token (base64url) and its
// SHA-256 hash hex string for storage. Only the hash is persisted.
func NewOpaqueToken() (token string, hashHex string, err error) {
	b := make([]byte, 32)
	if _, err = rand.Read(b); err != nil {
		return "", "", err
	}
	token = base64.RawURLEncoding.EncodeToString(b)
	hashHex = HashTokenSHA256(token)
	return token, hashHex, nil
}

// HashTokenSHA256 hex-encodes the SHA-256 of a token string.
func HashTokenSHA256(token string) string {
	sum := sha256Sum([]byte(token))
	return fmt.Sprintf("%x", sum)
}

// ValidatePasswordPolicy enforces the account password policy.
func ValidatePasswordPolicy(pw string) error {
	if len(pw) < 10 {
		return errors.New("password must be at least 10 characters")
	}
	if len(pw) > 128 {
		return errors.New("password must be at most 128 characters")
	}
	// Require some diversity without being annoying.
	var hasLetter, hasDigit bool
	for _, r := range pw {
		switch {
		case r >= '0' && r <= '9':
			hasDigit = true
		case (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z'):
			hasLetter = true
		}
	}
	if !hasLetter || !hasDigit {
		return errors.New("password must contain letters and digits")
	}
	return nil
}
