// Package store defines the persistence layer. All data about file contents
// is ciphertext; the store never sees plaintext notes, names, or keys.
package store

import (
	"errors"
	"time"
)

// Domain errors mapped to API errors by the api layer.
var (
	ErrNotFound       = errors.New("not found")
	ErrEmailTaken     = errors.New("email already registered")
	ErrConflict       = errors.New("version conflict")
	ErrDeviceLimit    = errors.New("device limit reached")
	ErrPairingExpired = errors.New("pairing code expired or invalid")
	ErrAccountLocked  = errors.New("account temporarily locked")
	ErrDuplicate      = errors.New("duplicate")
)

type Account struct {
	ID                  string
	Email               string
	PasswordHash        string
	CreatedAt           time.Time
	FailedLoginAttempts int
	LockedUntil         *time.Time
}

type Device struct {
	ID         string
	AccountID  string
	Name       string
	CreatedAt  time.Time
	LastSeenAt time.Time
	RevokedAt  *time.Time
}

type RefreshToken struct {
	ID        string
	DeviceID  string
	TokenHash string
	CreatedAt time.Time
	ExpiresAt time.Time
	RevokedAt *time.Time
}

// FileRecord is the current head of one synchronized file.
type FileRecord struct {
	AccountID         string
	FileID            string
	EncryptedMetadata []byte
	ContentHash       string // SHA-256 of ciphertext (hex)
	Version           int
	VersionVector     map[string]int
	Deleted           bool
	Size              int64
	BlobKey           string
	ModifiedAt        time.Time
	OriginDeviceID    string
	ChangeSeq         int64
}

// VersionRecord is one immutable historical version.
type VersionRecord struct {
	AccountID         string
	FileID            string
	Version           int
	EncryptedMetadata []byte
	ContentHash       string
	VersionVector     map[string]int
	Deleted           bool
	Size              int64
	BlobKey           string
	ModifiedAt        time.Time
	OriginDeviceID    string
}

type Change struct {
	ChangeSeq      int64
	AccountID      string
	FileID         string
	Version        int
	Kind           string // "upsert" | "delete"
	OriginDeviceID string
	CreatedAt      time.Time
}

type PairingCode struct {
	Code             string
	AccountID        string
	RequestingDevice string
	WrappedMasterKey []byte
	ApprovingDevice  *string
	CreatedAt        time.Time
	ExpiresAt        time.Time
	ConsumedAt       *time.Time
}

type Session struct {
	AccountID      string
	EncryptedState []byte
	Version        int
	UpdatedAt      time.Time
	OriginDeviceID string
}

// MergeVersionVector returns the element-wise maximum of two vectors.
func MergeVersionVector(a, b map[string]int) map[string]int {
	out := make(map[string]int, len(a)+len(b))
	for k, v := range a {
		out[k] = v
	}
	for k, v := range b {
		if v > out[k] {
			out[k] = v
		}
	}
	return out
}
