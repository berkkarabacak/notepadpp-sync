package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	_ "github.com/jackc/pgx/v5/stdlib"
)

// Postgres is the production Store backed by PostgreSQL.
// All queries are parameterized; no string-built SQL with user input.
type Postgres struct {
	db     *sql.DB
	uuidFn func() string
}

// OpenPostgres connects and verifies the database.
func OpenPostgres(ctx context.Context, url string, uuidFn func() string) (*Postgres, error) {
	db, err := sql.Open("pgx", url)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(20)
	db.SetMaxIdleConns(5)
	db.SetConnMaxLifetime(30 * time.Minute)
	if err := db.PingContext(ctx); err != nil {
		return nil, fmt.Errorf("ping postgres: %w", err)
	}
	return &Postgres{db: db, uuidFn: uuidFn}, nil
}

func (p *Postgres) Close() error { return p.db.Close() }

// ---- accounts ----

func (p *Postgres) CreateAccount(ctx context.Context, email, passwordHash string) (*Account, error) {
	a := &Account{ID: p.uuidFn(), Email: email, PasswordHash: passwordHash}
	err := p.db.QueryRowContext(ctx,
		`INSERT INTO accounts (id, email, password_hash) VALUES ($1, $2, $3)
		 RETURNING created_at, failed_login_attempts`,
		a.ID, email, passwordHash).Scan(&a.CreatedAt, &a.FailedLoginAttempts)
	if err != nil {
		if isUniqueViolation(err) {
			return nil, ErrEmailTaken
		}
		return nil, err
	}
	return a, nil
}

const accountCols = `id, email, password_hash, created_at, failed_login_attempts, locked_until`

func scanAccount(row interface{ Scan(...any) error }) (*Account, error) {
	var a Account
	err := row.Scan(&a.ID, &a.Email, &a.PasswordHash, &a.CreatedAt, &a.FailedLoginAttempts, &a.LockedUntil)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &a, nil
}

func (p *Postgres) AccountByEmail(ctx context.Context, email string) (*Account, error) {
	return scanAccount(p.db.QueryRowContext(ctx,
		`SELECT `+accountCols+` FROM accounts WHERE email = $1`, email))
}

func (p *Postgres) AccountByID(ctx context.Context, id string) (*Account, error) {
	return scanAccount(p.db.QueryRowContext(ctx,
		`SELECT `+accountCols+` FROM accounts WHERE id = $1`, id))
}

func (p *Postgres) RecordFailedLogin(ctx context.Context, accountID string, lockAfter int, lockFor time.Duration) (*Account, error) {
	return scanAccount(p.db.QueryRowContext(ctx,
		`UPDATE accounts SET
		   failed_login_attempts = CASE
		     WHEN failed_login_attempts + 1 >= $2 THEN 0
		     ELSE failed_login_attempts + 1 END,
		   locked_until = CASE
		     WHEN failed_login_attempts + 1 >= $2 THEN now() + $3::interval
		     ELSE locked_until END
		 WHERE id = $1
		 RETURNING `+accountCols,
		accountID, lockAfter, fmt.Sprintf("%d seconds", int(lockFor.Seconds()))))
}

func (p *Postgres) ResetFailedLogins(ctx context.Context, accountID string) error {
	_, err := p.db.ExecContext(ctx,
		`UPDATE accounts SET failed_login_attempts = 0, locked_until = NULL WHERE id = $1`, accountID)
	return err
}

// ---- devices ----

func (p *Postgres) CreateDevice(ctx context.Context, accountID, name string) (*Device, error) {
	d := &Device{ID: p.uuidFn(), AccountID: accountID, Name: name}
	err := p.db.QueryRowContext(ctx,
		`INSERT INTO devices (id, account_id, name) VALUES ($1, $2, $3)
		 RETURNING created_at, last_seen_at`,
		d.ID, accountID, name).Scan(&d.CreatedAt, &d.LastSeenAt)
	if err != nil {
		return nil, err
	}
	return d, nil
}

func (p *Postgres) CountActiveDevices(ctx context.Context, accountID string) (int, error) {
	var n int
	err := p.db.QueryRowContext(ctx,
		`SELECT count(*) FROM devices WHERE account_id = $1 AND revoked_at IS NULL`, accountID).Scan(&n)
	return n, err
}

func (p *Postgres) ListDevices(ctx context.Context, accountID string) ([]Device, error) {
	rows, err := p.db.QueryContext(ctx,
		`SELECT id, account_id, name, created_at, last_seen_at, revoked_at
		 FROM devices WHERE account_id = $1 ORDER BY created_at`, accountID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Device
	for rows.Next() {
		var d Device
		if err := rows.Scan(&d.ID, &d.AccountID, &d.Name, &d.CreatedAt, &d.LastSeenAt, &d.RevokedAt); err != nil {
			return nil, err
		}
		out = append(out, d)
	}
	return out, rows.Err()
}

func (p *Postgres) DeviceByID(ctx context.Context, accountID, deviceID string) (*Device, error) {
	var d Device
	err := p.db.QueryRowContext(ctx,
		`SELECT id, account_id, name, created_at, last_seen_at, revoked_at
		 FROM devices WHERE account_id = $1 AND id = $2`, accountID, deviceID).
		Scan(&d.ID, &d.AccountID, &d.Name, &d.CreatedAt, &d.LastSeenAt, &d.RevokedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &d, nil
}

func (p *Postgres) DeviceByIDGlobal(ctx context.Context, deviceID string) (*Device, error) {
	var d Device
	err := p.db.QueryRowContext(ctx,
		`SELECT id, account_id, name, created_at, last_seen_at, revoked_at
		 FROM devices WHERE id = $1`, deviceID).
		Scan(&d.ID, &d.AccountID, &d.Name, &d.CreatedAt, &d.LastSeenAt, &d.RevokedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &d, nil
}

func (p *Postgres) RenameDevice(ctx context.Context, accountID, deviceID, name string) error {
	res, err := p.db.ExecContext(ctx,
		`UPDATE devices SET name = $3 WHERE account_id = $1 AND id = $2`, accountID, deviceID, name)
	if err != nil {
		return err
	}
	return requireAffected(res)
}

// RevokeDevice revokes the device and all its refresh tokens in one tx.
func (p *Postgres) RevokeDevice(ctx context.Context, accountID, deviceID string) error {
	tx, err := p.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	res, err := tx.ExecContext(ctx,
		`UPDATE devices SET revoked_at = now() WHERE account_id = $1 AND id = $2 AND revoked_at IS NULL`,
		accountID, deviceID)
	if err != nil {
		return err
	}
	if err := requireAffected(res); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx,
		`UPDATE refresh_tokens SET revoked_at = now() WHERE device_id = $1 AND revoked_at IS NULL`,
		deviceID); err != nil {
		return err
	}
	return tx.Commit()
}

func (p *Postgres) TouchDevice(ctx context.Context, deviceID string) error {
	_, err := p.db.ExecContext(ctx,
		`UPDATE devices SET last_seen_at = now() WHERE id = $1`, deviceID)
	return err
}

// ---- refresh tokens ----

func (p *Postgres) CreateRefreshToken(ctx context.Context, deviceID, tokenHash string, expiresAt time.Time) (*RefreshToken, error) {
	t := &RefreshToken{ID: p.uuidFn(), DeviceID: deviceID, TokenHash: tokenHash, ExpiresAt: expiresAt}
	err := p.db.QueryRowContext(ctx,
		`INSERT INTO refresh_tokens (id, device_id, token_hash, expires_at) VALUES ($1, $2, $3, $4)
		 RETURNING created_at`, t.ID, deviceID, tokenHash, expiresAt).Scan(&t.CreatedAt)
	if err != nil {
		return nil, err
	}
	return t, nil
}

func (p *Postgres) RefreshTokenByHash(ctx context.Context, tokenHash string) (*RefreshToken, error) {
	var t RefreshToken
	err := p.db.QueryRowContext(ctx,
		`SELECT id, device_id, token_hash, created_at, expires_at, revoked_at
		 FROM refresh_tokens WHERE token_hash = $1`, tokenHash).
		Scan(&t.ID, &t.DeviceID, &t.TokenHash, &t.CreatedAt, &t.ExpiresAt, &t.RevokedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &t, nil
}

func (p *Postgres) RotateRefreshToken(ctx context.Context, oldID, newTokenHash string, expiresAt time.Time) (*RefreshToken, error) {
	tx, err := p.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	var deviceID string
	err = tx.QueryRowContext(ctx,
		`UPDATE refresh_tokens SET revoked_at = now()
		 WHERE id = $1 AND revoked_at IS NULL AND expires_at > now()
		 RETURNING device_id`, oldID).Scan(&deviceID)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	t := &RefreshToken{ID: p.uuidFn(), DeviceID: deviceID, TokenHash: newTokenHash, ExpiresAt: expiresAt}
	err = tx.QueryRowContext(ctx,
		`INSERT INTO refresh_tokens (id, device_id, token_hash, expires_at) VALUES ($1, $2, $3, $4)
		 RETURNING created_at`, t.ID, deviceID, newTokenHash, expiresAt).Scan(&t.CreatedAt)
	if err != nil {
		return nil, err
	}
	return t, tx.Commit()
}

func (p *Postgres) RevokeRefreshToken(ctx context.Context, id string) error {
	_, err := p.db.ExecContext(ctx,
		`UPDATE refresh_tokens SET revoked_at = now() WHERE id = $1`, id)
	return err
}

// ---- files ----

const fileCols = `account_id, file_id, encrypted_metadata, content_hash, version,
                  version_vector, deleted, size, blob_key, modified_at, origin_device_id, change_seq`

func scanFile(row interface{ Scan(...any) error }) (*FileRecord, error) {
	var f FileRecord
	var vv []byte
	err := row.Scan(&f.AccountID, &f.FileID, &f.EncryptedMetadata, &f.ContentHash, &f.Version,
		&vv, &f.Deleted, &f.Size, &f.BlobKey, &f.ModifiedAt, &f.OriginDeviceID, &f.ChangeSeq)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(vv, &f.VersionVector); err != nil {
		return nil, err
	}
	return &f, nil
}

func (p *Postgres) ListFiles(ctx context.Context, accountID string) ([]FileRecord, error) {
	rows, err := p.db.QueryContext(ctx,
		`SELECT `+fileCols+` FROM files WHERE account_id = $1 ORDER BY file_id`, accountID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []FileRecord
	for rows.Next() {
		var f FileRecord
		var vv []byte
		if err := rows.Scan(&f.AccountID, &f.FileID, &f.EncryptedMetadata, &f.ContentHash, &f.Version,
			&vv, &f.Deleted, &f.Size, &f.BlobKey, &f.ModifiedAt, &f.OriginDeviceID, &f.ChangeSeq); err != nil {
			return nil, err
		}
		if err := json.Unmarshal(vv, &f.VersionVector); err != nil {
			return nil, err
		}
		out = append(out, f)
	}
	return out, rows.Err()
}

func (p *Postgres) GetFile(ctx context.Context, accountID, fileID string) (*FileRecord, error) {
	return scanFile(p.db.QueryRowContext(ctx,
		`SELECT `+fileCols+` FROM files WHERE account_id = $1 AND file_id = $2`, accountID, fileID))
}

// CreateFile inserts the file, its first history row, and its change-feed row
// in a single transaction.
func (p *Postgres) CreateFile(ctx context.Context, rec *FileRecord) (*FileRecord, error) {
	vv, err := json.Marshal(rec.VersionVector)
	if err != nil {
		return nil, err
	}
	tx, err := p.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()

	var seq int64
	err = tx.QueryRowContext(ctx,
		`INSERT INTO files (account_id, file_id, encrypted_metadata, content_hash, version,
		                    version_vector, deleted, size, blob_key, modified_at, origin_device_id, change_seq)
		 VALUES ($1,$2,$3,$4,1,$5,$6,$7,$8,$9,$10, nextval('changes_change_seq_seq'))
		 RETURNING change_seq`,
		rec.AccountID, rec.FileID, rec.EncryptedMetadata, rec.ContentHash,
		vv, rec.Deleted, rec.Size, rec.BlobKey, rec.ModifiedAt, rec.OriginDeviceID).Scan(&seq)
	if err != nil {
		if isUniqueViolation(err) {
			return nil, ErrDuplicate
		}
		return nil, err
	}
	if err := insertVersionRow(ctx, tx, rec, 1); err != nil {
		return nil, err
	}
	if err := insertChangeRow(ctx, tx, rec.AccountID, rec.FileID, 1, kindOfStr(rec.Deleted), rec.OriginDeviceID); err != nil {
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	out := *rec
	out.Version = 1
	out.ChangeSeq = seq
	return &out, nil
}

// UpdateFile atomically checks base_version and writes the new head version.
// The version check happens inside the UPDATE's WHERE clause so concurrent
// writers cannot both succeed.
func (p *Postgres) UpdateFile(ctx context.Context, rec *FileRecord, baseVersion int) (*FileRecord, error) {
	vv, err := json.Marshal(rec.VersionVector)
	if err != nil {
		return nil, err
	}
	tx, err := p.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()

	var newVersion int
	var seq int64
	err = tx.QueryRowContext(ctx,
		`UPDATE files SET
		   encrypted_metadata = $3, content_hash = $4, version = version + 1,
		   version_vector = $5, deleted = $6, size = $7, blob_key = $8,
		   modified_at = $9, origin_device_id = $10,
		   change_seq = nextval('changes_change_seq_seq')
		 WHERE account_id = $1 AND file_id = $2 AND version = $11
		 RETURNING version, change_seq`,
		rec.AccountID, rec.FileID, rec.EncryptedMetadata, rec.ContentHash,
		vv, rec.Deleted, rec.Size, rec.BlobKey, rec.ModifiedAt, rec.OriginDeviceID,
		baseVersion).Scan(&newVersion, &seq)
	if errors.Is(err, sql.ErrNoRows) {
		// Distinguish missing file from version conflict.
		if _, ferr := p.GetFile(ctx, rec.AccountID, rec.FileID); errors.Is(ferr, ErrNotFound) {
			return nil, ErrNotFound
		}
		return nil, ErrConflict
	}
	if err != nil {
		return nil, err
	}
	if err := insertVersionRow(ctx, tx, rec, newVersion); err != nil {
		return nil, err
	}
	if err := insertChangeRow(ctx, tx, rec.AccountID, rec.FileID, newVersion, kindOfStr(rec.Deleted), rec.OriginDeviceID); err != nil {
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	out := *rec
	out.Version = newVersion
	out.ChangeSeq = seq
	return &out, nil
}

func insertVersionRow(ctx context.Context, tx *sql.Tx, rec *FileRecord, version int) error {
	vv, err := json.Marshal(rec.VersionVector)
	if err != nil {
		return err
	}
	_, err = tx.ExecContext(ctx,
		`INSERT INTO file_versions (account_id, file_id, version, encrypted_metadata, content_hash,
		                            version_vector, deleted, size, blob_key, modified_at, origin_device_id)
		 VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)`,
		rec.AccountID, rec.FileID, version, rec.EncryptedMetadata, rec.ContentHash,
		vv, rec.Deleted, rec.Size, rec.BlobKey, rec.ModifiedAt, rec.OriginDeviceID)
	return err
}

func insertChangeRow(ctx context.Context, tx *sql.Tx, accountID, fileID string, version int, kind, originDeviceID string) error {
	_, err := tx.ExecContext(ctx,
		`INSERT INTO changes (account_id, file_id, version, kind, origin_device_id)
		 VALUES ($1,$2,$3,$4,$5)`, accountID, fileID, version, kind, originDeviceID)
	return err
}

func kindOfStr(deleted bool) string {
	if deleted {
		return "delete"
	}
	return "upsert"
}

// ---- versions ----

const versionCols = `account_id, file_id, version, encrypted_metadata, content_hash,
                     version_vector, deleted, size, blob_key, modified_at, origin_device_id`

func (p *Postgres) ListVersions(ctx context.Context, accountID, fileID string, limit int) ([]VersionRecord, error) {
	rows, err := p.db.QueryContext(ctx,
		`SELECT `+versionCols+` FROM file_versions
		 WHERE account_id = $1 AND file_id = $2 ORDER BY version DESC LIMIT $3`,
		accountID, fileID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []VersionRecord
	for rows.Next() {
		var v VersionRecord
		var vv []byte
		if err := rows.Scan(&v.AccountID, &v.FileID, &v.Version, &v.EncryptedMetadata, &v.ContentHash,
			&vv, &v.Deleted, &v.Size, &v.BlobKey, &v.ModifiedAt, &v.OriginDeviceID); err != nil {
			return nil, err
		}
		if err := json.Unmarshal(vv, &v.VersionVector); err != nil {
			return nil, err
		}
		out = append(out, v)
	}
	return out, rows.Err()
}

func (p *Postgres) GetVersion(ctx context.Context, accountID, fileID string, version int) (*VersionRecord, error) {
	var v VersionRecord
	var vv []byte
	err := p.db.QueryRowContext(ctx,
		`SELECT `+versionCols+` FROM file_versions
		 WHERE account_id = $1 AND file_id = $2 AND version = $3`,
		accountID, fileID, version).
		Scan(&v.AccountID, &v.FileID, &v.Version, &v.EncryptedMetadata, &v.ContentHash,
			&vv, &v.Deleted, &v.Size, &v.BlobKey, &v.ModifiedAt, &v.OriginDeviceID)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(vv, &v.VersionVector); err != nil {
		return nil, err
	}
	return &v, nil
}

// PruneVersions deletes all but the newest `keep` versions and returns blob
// keys that are no longer referenced by any remaining row.
func (p *Postgres) PruneVersions(ctx context.Context, accountID, fileID string, keep int) ([]string, error) {
	tx, err := p.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	rows, err := tx.QueryContext(ctx,
		`DELETE FROM file_versions
		 WHERE (account_id, file_id, version) IN (
		   SELECT account_id, file_id, version FROM file_versions
		   WHERE account_id = $1 AND file_id = $2
		   ORDER BY version DESC OFFSET $3
		 ) RETURNING blob_key`, accountID, fileID, keep)
	if err != nil {
		return nil, err
	}
	var candidates []string
	for rows.Next() {
		var k string
		if err := rows.Scan(&k); err != nil {
			rows.Close()
			return nil, err
		}
		candidates = append(candidates, k)
	}
	rows.Close()
	// Filter out keys still referenced by head or remaining versions.
	var pruned []string
	for _, k := range candidates {
		var n int
		if err := tx.QueryRowContext(ctx,
			`SELECT (SELECT count(*) FROM files WHERE account_id=$1 AND file_id=$2 AND blob_key=$3) +
			        (SELECT count(*) FROM file_versions WHERE account_id=$1 AND file_id=$2 AND blob_key=$3)`,
			accountID, fileID, k).Scan(&n); err != nil {
			return nil, err
		}
		if n == 0 {
			pruned = append(pruned, k)
		}
	}
	return pruned, tx.Commit()
}

// ---- change feed ----

func (p *Postgres) ListChangesSince(ctx context.Context, accountID string, since int64, limit int) ([]Change, error) {
	rows, err := p.db.QueryContext(ctx,
		`SELECT change_seq, account_id, file_id, version, kind, origin_device_id, created_at
		 FROM changes WHERE account_id = $1 AND change_seq > $2
		 ORDER BY change_seq LIMIT $3`, accountID, since, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Change
	for rows.Next() {
		var c Change
		if err := rows.Scan(&c.ChangeSeq, &c.AccountID, &c.FileID, &c.Version, &c.Kind, &c.OriginDeviceID, &c.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// ---- idempotency ----

func (p *Postgres) GetIdempotentResponse(ctx context.Context, accountID, key string) ([]byte, bool, error) {
	var resp []byte
	err := p.db.QueryRowContext(ctx,
		`SELECT response FROM idempotency_keys WHERE account_id = $1 AND key = $2`,
		accountID, key).Scan(&resp)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, err
	}
	return resp, true, nil
}

func (p *Postgres) SaveIdempotentResponse(ctx context.Context, accountID, key string, responseJSON []byte) error {
	_, err := p.db.ExecContext(ctx,
		`INSERT INTO idempotency_keys (account_id, key, response) VALUES ($1,$2,$3)
		 ON CONFLICT (account_id, key) DO NOTHING`, accountID, key, responseJSON)
	return err
}

// ---- pairing ----

func (p *Postgres) CreatePairingCode(ctx context.Context, pc *PairingCode) error {
	_, err := p.db.ExecContext(ctx,
		`INSERT INTO pairing_codes (code, account_id, requesting_device, expires_at)
		 VALUES ($1,$2,$3,$4)`, pc.Code, pc.AccountID, pc.RequestingDevice, pc.ExpiresAt)
	return err
}

func (p *Postgres) PairingCodeByCode(ctx context.Context, code string) (*PairingCode, error) {
	var pc PairingCode
	err := p.db.QueryRowContext(ctx,
		`SELECT code, account_id, requesting_device, wrapped_master_key, approving_device,
		        created_at, expires_at, consumed_at
		 FROM pairing_codes WHERE code = $1`, code).
		Scan(&pc.Code, &pc.AccountID, &pc.RequestingDevice, &pc.WrappedMasterKey,
			&pc.ApprovingDevice, &pc.CreatedAt, &pc.ExpiresAt, &pc.ConsumedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &pc, nil
}

func (p *Postgres) ApprovePairingCode(ctx context.Context, code, approvingDevice string, wrappedKey []byte) error {
	res, err := p.db.ExecContext(ctx,
		`UPDATE pairing_codes SET wrapped_master_key = $2, approving_device = $3
		 WHERE code = $1 AND consumed_at IS NULL AND expires_at > now()`,
		code, wrappedKey, approvingDevice)
	if err != nil {
		return err
	}
	n, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if n == 0 {
		return ErrPairingExpired
	}
	return nil
}

func (p *Postgres) ConsumePairingCode(ctx context.Context, code string) error {
	_, err := p.db.ExecContext(ctx,
		`UPDATE pairing_codes SET consumed_at = now() WHERE code = $1`, code)
	return err
}

// ---- session ----

func (p *Postgres) GetSession(ctx context.Context, accountID string) (*Session, error) {
	var s Session
	err := p.db.QueryRowContext(ctx,
		`SELECT account_id, encrypted_state, version, updated_at, origin_device_id
		 FROM sessions WHERE account_id = $1`, accountID).
		Scan(&s.AccountID, &s.EncryptedState, &s.Version, &s.UpdatedAt, &s.OriginDeviceID)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return &s, nil
}

func (p *Postgres) PutSession(ctx context.Context, s *Session) error {
	return p.db.QueryRowContext(ctx,
		`INSERT INTO sessions (account_id, encrypted_state, version, origin_device_id)
		 VALUES ($1,$2,$3,$4)
		 ON CONFLICT (account_id) DO UPDATE SET
		   encrypted_state = EXCLUDED.encrypted_state,
		   version = EXCLUDED.version,
		   updated_at = now(),
		   origin_device_id = EXCLUDED.origin_device_id
		 RETURNING updated_at`, s.AccountID, s.EncryptedState, s.Version, s.OriginDeviceID).
		Scan(&s.UpdatedAt)
}

// ---- helpers ----

func requireAffected(res interface{ RowsAffected() (int64, error) }) error {
	n, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if n == 0 {
		return ErrNotFound
	}
	return nil
}

func isUniqueViolation(err error) bool {
	// pgx stdlib surfaces pgconn.PgError with code 23505.
	type sqlState interface {
		SQLState() string
	}
	var st sqlState
	if errors.As(err, &st) && st.SQLState() == "23505" {
		return true
	}
	// Fallback for wrapped errors that only carry the message.
	return err != nil && (contains(err.Error(), "duplicate key") || contains(err.Error(), "23505"))
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}
