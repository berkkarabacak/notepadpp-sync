package store

import (
	"context"
	"time"
)

// Store is the complete persistence contract of the sync server.
// Implementations: Postgres (production) and Mem (tests / dev).
type Store interface {
	// ---- accounts ----
	CreateAccount(ctx context.Context, email, passwordHash string) (*Account, error)
	// CreateGoogleAccount inserts an account with no password hash.
	CreateGoogleAccount(ctx context.Context, email, googleSub string) (*Account, error)
	AccountByEmail(ctx context.Context, email string) (*Account, error)
	AccountByGoogleSub(ctx context.Context, googleSub string) (*Account, error)
	AccountByID(ctx context.Context, id string) (*Account, error)
	// LinkGoogleSubject sets google_sub when it is still empty.
	// ErrConflict: this account is already linked to a different subject.
	// ErrDuplicate: this subject is already linked to another account.
	LinkGoogleSubject(ctx context.Context, accountID, googleSub string) error
	RecordFailedLogin(ctx context.Context, accountID string, lockAfter int, lockFor time.Duration) (*Account, error)
	ResetFailedLogins(ctx context.Context, accountID string) error

	// ---- Google browser logins (Authorization Code + PKCE) ----
	CreateOAuthLogin(ctx context.Context, login *OAuthLogin) error
	OAuthLoginByState(ctx context.Context, state string) (*OAuthLogin, error)
	// ClaimOAuthLogin moves pending → exchanging when now is before expiry.
	// A second claim fails so only one callback redeems the authorization code.
	ClaimOAuthLogin(ctx context.Context, state string, now time.Time) (*OAuthLogin, error)
	MarkOAuthReady(ctx context.Context, state, accountID string) error
	MarkOAuthError(ctx context.Context, state, code, message string) error
	// ConsumeOAuthLogin moves ready → consumed. ErrNotFound if it was not ready.
	ConsumeOAuthLogin(ctx context.Context, state string) (*OAuthLogin, error)
	// ReopenOAuthLogin moves consumed → ready so a failed token issue can retry.
	ReopenOAuthLogin(ctx context.Context, state string) error

	// ---- devices ----
	CreateDevice(ctx context.Context, accountID, name string) (*Device, error)
	CountActiveDevices(ctx context.Context, accountID string) (int, error)
	ListDevices(ctx context.Context, accountID string) ([]Device, error)
	DeviceByID(ctx context.Context, accountID, deviceID string) (*Device, error)
	// DeviceByIDGlobal resolves a device without knowing the account
	// (used when authenticating via refresh token).
	DeviceByIDGlobal(ctx context.Context, deviceID string) (*Device, error)
	RenameDevice(ctx context.Context, accountID, deviceID, name string) error
	// RevokeDevice marks the device revoked and revokes all its refresh tokens.
	RevokeDevice(ctx context.Context, accountID, deviceID string) error
	TouchDevice(ctx context.Context, deviceID string) error

	// ---- refresh tokens ----
	CreateRefreshToken(ctx context.Context, deviceID, tokenHash string, expiresAt time.Time) (*RefreshToken, error)
	RefreshTokenByHash(ctx context.Context, tokenHash string) (*RefreshToken, error)
	// RotateRefreshToken revokes oldID and inserts a fresh token for the same device.
	RotateRefreshToken(ctx context.Context, oldID, newTokenHash string, expiresAt time.Time) (*RefreshToken, error)
	RevokeRefreshToken(ctx context.Context, id string) error

	// ---- files ----
	ListFiles(ctx context.Context, accountID string) ([]FileRecord, error)
	GetFile(ctx context.Context, accountID, fileID string) (*FileRecord, error)
	// CreateFile inserts a new file at version 1. Returns ErrDuplicate if the
	// file_id already exists for this account.
	CreateFile(ctx context.Context, rec *FileRecord) (*FileRecord, error)
	// UpdateFile writes a new head version iff rec.Version == current.Version+1
	// and rec.BaseVersion == current.Version; otherwise ErrConflict.
	// On success it appends the version history row and the change-feed row,
	// and returns the updated record with its assigned change_seq.
	UpdateFile(ctx context.Context, rec *FileRecord, baseVersion int) (*FileRecord, error)
	// ListVersions returns history newest-first.
	ListVersions(ctx context.Context, accountID, fileID string, limit int) ([]VersionRecord, error)
	GetVersion(ctx context.Context, accountID, fileID string, version int) (*VersionRecord, error)
	// PruneVersions keeps the newest `keep` versions per file; returns blob
	// keys that became unreferenced (so the caller can delete blobs).
	PruneVersions(ctx context.Context, accountID, fileID string, keep int) ([]string, error)

	// ---- change feed ----
	ListChangesSince(ctx context.Context, accountID string, since int64, limit int) ([]Change, error)

	// ---- idempotency ----
	GetIdempotentResponse(ctx context.Context, accountID, key string) (responseJSON []byte, found bool, err error)
	SaveIdempotentResponse(ctx context.Context, accountID, key string, responseJSON []byte) error

	// ---- pairing ----
	CreatePairingCode(ctx context.Context, pc *PairingCode) error
	PairingCodeByCode(ctx context.Context, code string) (*PairingCode, error)
	ApprovePairingCode(ctx context.Context, code, approvingDevice string, wrappedKey []byte) error
	ConsumePairingCode(ctx context.Context, code string) error

	// ---- session ----
	GetSession(ctx context.Context, accountID string) (*Session, error)
	PutSession(ctx context.Context, s *Session) error

	Close() error
}
