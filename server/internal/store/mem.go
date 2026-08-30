package store

import (
	"context"
	"sort"
	"sync"
	"time"
)

// Mem is an in-memory Store used by unit/integration tests and local dev.
// It enforces the same conflict and idempotency semantics as Postgres.
type Mem struct {
	mu sync.Mutex

	accounts   map[string]*Account // by ID
	byEmail    map[string]string   // email -> account ID
	devices    map[string]*Device  // by ID
	tokens     map[string]*RefreshToken
	tokensByH  map[string]string          // hash -> token ID
	files      map[string]*FileRecord     // accountID + "/" + fileID
	versions   map[string][]VersionRecord // accountID/fileID -> history
	changes    []Change
	nextSeq    int64
	idempotent map[string][]byte // accountID + "/" + key
	pairing    map[string]*PairingCode
	sessions   map[string]*Session

	uuidFn func() string
}

func NewMem(uuidFn func() string) *Mem {
	return &Mem{
		accounts:   map[string]*Account{},
		byEmail:    map[string]string{},
		devices:    map[string]*Device{},
		tokens:     map[string]*RefreshToken{},
		tokensByH:  map[string]string{},
		files:      map[string]*FileRecord{},
		versions:   map[string][]VersionRecord{},
		nextSeq:    1,
		idempotent: map[string][]byte{},
		pairing:    map[string]*PairingCode{},
		sessions:   map[string]*Session{},
		uuidFn:     uuidFn,
	}
}

func (m *Mem) Close() error { return nil }

func fileKey(accountID, fileID string) string { return accountID + "/" + fileID }

func (m *Mem) CreateAccount(ctx context.Context, email, passwordHash string) (*Account, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.byEmail[email]; ok {
		return nil, ErrEmailTaken
	}
	a := &Account{ID: m.uuidFn(), Email: email, PasswordHash: passwordHash, CreatedAt: time.Now()}
	m.accounts[a.ID] = a
	m.byEmail[email] = a.ID
	return a, nil
}

func (m *Mem) AccountByEmail(ctx context.Context, email string) (*Account, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	id, ok := m.byEmail[email]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *m.accounts[id]
	return &cp, nil
}

func (m *Mem) AccountByID(ctx context.Context, id string) (*Account, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[id]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *a
	return &cp, nil
}

func (m *Mem) RecordFailedLogin(ctx context.Context, accountID string, lockAfter int, lockFor time.Duration) (*Account, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	a, ok := m.accounts[accountID]
	if !ok {
		return nil, ErrNotFound
	}
	a.FailedLoginAttempts++
	if a.FailedLoginAttempts >= lockAfter {
		until := time.Now().Add(lockFor)
		a.LockedUntil = &until
		a.FailedLoginAttempts = 0
	}
	cp := *a
	return &cp, nil
}

func (m *Mem) ResetFailedLogins(ctx context.Context, accountID string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if a, ok := m.accounts[accountID]; ok {
		a.FailedLoginAttempts = 0
		a.LockedUntil = nil
	}
	return nil
}

func (m *Mem) CreateDevice(ctx context.Context, accountID, name string) (*Device, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	d := &Device{ID: m.uuidFn(), AccountID: accountID, Name: name, CreatedAt: time.Now(), LastSeenAt: time.Now()}
	m.devices[d.ID] = d
	return d, nil
}

func (m *Mem) CountActiveDevices(ctx context.Context, accountID string) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	n := 0
	for _, d := range m.devices {
		if d.AccountID == accountID && d.RevokedAt == nil {
			n++
		}
	}
	return n, nil
}

func (m *Mem) ListDevices(ctx context.Context, accountID string) ([]Device, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []Device
	for _, d := range m.devices {
		if d.AccountID == accountID {
			out = append(out, *d)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].CreatedAt.Before(out[j].CreatedAt) })
	return out, nil
}

func (m *Mem) DeviceByID(ctx context.Context, accountID, deviceID string) (*Device, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	d, ok := m.devices[deviceID]
	if !ok || d.AccountID != accountID {
		return nil, ErrNotFound
	}
	cp := *d
	return &cp, nil
}

func (m *Mem) DeviceByIDGlobal(ctx context.Context, deviceID string) (*Device, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	d, ok := m.devices[deviceID]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *d
	return &cp, nil
}

func (m *Mem) RenameDevice(ctx context.Context, accountID, deviceID, name string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	d, ok := m.devices[deviceID]
	if !ok || d.AccountID != accountID {
		return ErrNotFound
	}
	d.Name = name
	return nil
}

func (m *Mem) RevokeDevice(ctx context.Context, accountID, deviceID string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	d, ok := m.devices[deviceID]
	if !ok || d.AccountID != accountID {
		return ErrNotFound
	}
	now := time.Now()
	d.RevokedAt = &now
	for _, t := range m.tokens {
		if t.DeviceID == deviceID && t.RevokedAt == nil {
			t.RevokedAt = &now
		}
	}
	return nil
}

func (m *Mem) TouchDevice(ctx context.Context, deviceID string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if d, ok := m.devices[deviceID]; ok {
		d.LastSeenAt = time.Now()
	}
	return nil
}

func (m *Mem) CreateRefreshToken(ctx context.Context, deviceID, tokenHash string, expiresAt time.Time) (*RefreshToken, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	t := &RefreshToken{ID: m.uuidFn(), DeviceID: deviceID, TokenHash: tokenHash, CreatedAt: time.Now(), ExpiresAt: expiresAt}
	m.tokens[t.ID] = t
	m.tokensByH[tokenHash] = t.ID
	return t, nil
}

func (m *Mem) RefreshTokenByHash(ctx context.Context, tokenHash string) (*RefreshToken, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	id, ok := m.tokensByH[tokenHash]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *m.tokens[id]
	return &cp, nil
}

func (m *Mem) RotateRefreshToken(ctx context.Context, oldID, newTokenHash string, expiresAt time.Time) (*RefreshToken, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	old, ok := m.tokens[oldID]
	if !ok {
		return nil, ErrNotFound
	}
	now := time.Now()
	old.RevokedAt = &now
	t := &RefreshToken{ID: m.uuidFn(), DeviceID: old.DeviceID, TokenHash: newTokenHash, CreatedAt: now, ExpiresAt: expiresAt}
	m.tokens[t.ID] = t
	m.tokensByH[newTokenHash] = t.ID
	return t, nil
}

func (m *Mem) RevokeRefreshToken(ctx context.Context, id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if t, ok := m.tokens[id]; ok {
		now := time.Now()
		t.RevokedAt = &now
	}
	return nil
}

func (m *Mem) ListFiles(ctx context.Context, accountID string) ([]FileRecord, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []FileRecord
	for _, f := range m.files {
		if f.AccountID == accountID {
			out = append(out, *f)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].FileID < out[j].FileID })
	return out, nil
}

func (m *Mem) GetFile(ctx context.Context, accountID, fileID string) (*FileRecord, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	f, ok := m.files[fileKey(accountID, fileID)]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *f
	return &cp, nil
}

func (m *Mem) CreateFile(ctx context.Context, rec *FileRecord) (*FileRecord, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := fileKey(rec.AccountID, rec.FileID)
	if _, ok := m.files[k]; ok {
		return nil, ErrDuplicate
	}
	cp := *rec
	cp.Version = 1
	cp.ChangeSeq = m.nextSeq
	m.nextSeq++
	m.files[k] = &cp
	m.versions[k] = append(m.versions[k], versionFromRecord(&cp))
	m.changes = append(m.changes, Change{
		ChangeSeq: cp.ChangeSeq, AccountID: cp.AccountID, FileID: cp.FileID,
		Version: cp.Version, Kind: kindOf(cp.Deleted), OriginDeviceID: cp.OriginDeviceID,
		CreatedAt: time.Now(),
	})
	out := cp
	return &out, nil
}

func (m *Mem) UpdateFile(ctx context.Context, rec *FileRecord, baseVersion int) (*FileRecord, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := fileKey(rec.AccountID, rec.FileID)
	cur, ok := m.files[k]
	if !ok {
		return nil, ErrNotFound
	}
	if cur.Version != baseVersion {
		return nil, ErrConflict
	}
	cp := *rec
	cp.Version = cur.Version + 1
	cp.ChangeSeq = m.nextSeq
	m.nextSeq++
	m.files[k] = &cp
	m.versions[k] = append(m.versions[k], versionFromRecord(&cp))
	m.changes = append(m.changes, Change{
		ChangeSeq: cp.ChangeSeq, AccountID: cp.AccountID, FileID: cp.FileID,
		Version: cp.Version, Kind: kindOf(cp.Deleted), OriginDeviceID: cp.OriginDeviceID,
		CreatedAt: time.Now(),
	})
	out := cp
	return &out, nil
}

func versionFromRecord(f *FileRecord) VersionRecord {
	return VersionRecord{
		AccountID: f.AccountID, FileID: f.FileID, Version: f.Version,
		EncryptedMetadata: f.EncryptedMetadata, ContentHash: f.ContentHash,
		VersionVector: f.VersionVector, Deleted: f.Deleted, Size: f.Size,
		BlobKey: f.BlobKey, ModifiedAt: f.ModifiedAt, OriginDeviceID: f.OriginDeviceID,
	}
}

func kindOf(deleted bool) string {
	if deleted {
		return "delete"
	}
	return "upsert"
}

func (m *Mem) ListVersions(ctx context.Context, accountID, fileID string, limit int) ([]VersionRecord, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	vs := m.versions[fileKey(accountID, fileID)]
	out := make([]VersionRecord, 0, len(vs))
	for i := len(vs) - 1; i >= 0 && len(out) < limit; i-- {
		out = append(out, vs[i])
	}
	return out, nil
}

func (m *Mem) GetVersion(ctx context.Context, accountID, fileID string, version int) (*VersionRecord, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, v := range m.versions[fileKey(accountID, fileID)] {
		if v.Version == version {
			cp := v
			return &cp, nil
		}
	}
	return nil, ErrNotFound
}

func (m *Mem) PruneVersions(ctx context.Context, accountID, fileID string, keep int) ([]string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := fileKey(accountID, fileID)
	vs := m.versions[k]
	if len(vs) <= keep {
		return nil, nil
	}
	headBlob := ""
	if f, ok := m.files[k]; ok {
		headBlob = f.BlobKey
	}
	var pruned []string
	drop := vs[:len(vs)-keep]
	kept := vs[len(vs)-keep:]
	keptKeys := map[string]bool{headBlob: true}
	for _, v := range kept {
		keptKeys[v.BlobKey] = true
	}
	for _, v := range drop {
		if !keptKeys[v.BlobKey] {
			pruned = append(pruned, v.BlobKey)
		}
	}
	m.versions[k] = kept
	return pruned, nil
}

func (m *Mem) ListChangesSince(ctx context.Context, accountID string, since int64, limit int) ([]Change, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []Change
	for _, c := range m.changes {
		if c.AccountID == accountID && c.ChangeSeq > since {
			out = append(out, c)
			if limit > 0 && len(out) >= limit {
				break
			}
		}
	}
	return out, nil
}

func (m *Mem) GetIdempotentResponse(ctx context.Context, accountID, key string) ([]byte, bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	v, ok := m.idempotent[accountID+"/"+key]
	return v, ok, nil
}

func (m *Mem) SaveIdempotentResponse(ctx context.Context, accountID, key string, responseJSON []byte) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.idempotent[accountID+"/"+key] = responseJSON
	return nil
}

func (m *Mem) CreatePairingCode(ctx context.Context, pc *PairingCode) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.pairing[pc.Code] = pc
	return nil
}

func (m *Mem) PairingCodeByCode(ctx context.Context, code string) (*PairingCode, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	pc, ok := m.pairing[code]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *pc
	return &cp, nil
}

func (m *Mem) ApprovePairingCode(ctx context.Context, code, approvingDevice string, wrappedKey []byte) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	pc, ok := m.pairing[code]
	if !ok || pc.ConsumedAt != nil || time.Now().After(pc.ExpiresAt) {
		return ErrPairingExpired
	}
	pc.WrappedMasterKey = wrappedKey
	pc.ApprovingDevice = &approvingDevice
	return nil
}

func (m *Mem) ConsumePairingCode(ctx context.Context, code string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	pc, ok := m.pairing[code]
	if !ok {
		return ErrNotFound
	}
	now := time.Now()
	pc.ConsumedAt = &now
	return nil
}

func (m *Mem) GetSession(ctx context.Context, accountID string) (*Session, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	s, ok := m.sessions[accountID]
	if !ok {
		return nil, ErrNotFound
	}
	cp := *s
	return &cp, nil
}

func (m *Mem) PutSession(ctx context.Context, s *Session) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	cp := *s
	cp.UpdatedAt = time.Now()
	m.sessions[s.AccountID] = &cp
	return nil
}
