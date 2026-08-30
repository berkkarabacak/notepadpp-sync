package store

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"
)

var memUUIDCounter = 0

func memUUID() string {
	memUUIDCounter++
	return fmt.Sprintf("00000000-0000-4000-8000-%012d", memUUIDCounter)
}

func newMem() *Mem { return NewMem(memUUID) }

func TestMemAccountLifecycle(t *testing.T) {
	ctx := context.Background()
	m := newMem()
	a, err := m.CreateAccount(ctx, "a@b.c", "hash")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := m.CreateAccount(ctx, "a@b.c", "hash2"); !errors.Is(err, ErrEmailTaken) {
		t.Fatal("duplicate email accepted")
	}
	got, err := m.AccountByEmail(ctx, "a@b.c")
	if err != nil || got.ID != a.ID {
		t.Fatalf("lookup failed: %v", err)
	}
}

func TestMemLockout(t *testing.T) {
	ctx := context.Background()
	m := newMem()
	a, _ := m.CreateAccount(ctx, "a@b.c", "hash")
	for i := 0; i < 3; i++ {
		if _, err := m.RecordFailedLogin(ctx, a.ID, 3, time.Minute); err != nil {
			t.Fatal(err)
		}
	}
	got, _ := m.AccountByID(ctx, a.ID)
	if got.LockedUntil == nil {
		t.Fatal("account not locked after threshold")
	}
	_ = m.ResetFailedLogins(ctx, a.ID)
	got, _ = m.AccountByID(ctx, a.ID)
	if got.LockedUntil != nil || got.FailedLoginAttempts != 0 {
		t.Fatal("reset failed")
	}
}

func TestMemDeviceRevocationKillsTokens(t *testing.T) {
	ctx := context.Background()
	m := newMem()
	a, _ := m.CreateAccount(ctx, "a@b.c", "hash")
	d, _ := m.CreateDevice(ctx, a.ID, "laptop")
	tok, _ := m.CreateRefreshToken(ctx, d.ID, "hash-1", time.Now().Add(time.Hour))

	if err := m.RevokeDevice(ctx, a.ID, d.ID); err != nil {
		t.Fatal(err)
	}
	got, _ := m.RefreshTokenByHash(ctx, "hash-1")
	if got.RevokedAt == nil {
		t.Fatal("refresh token survived device revocation")
	}
	_ = tok
}

func TestMemFileConflictSemantics(t *testing.T) {
	ctx := context.Background()
	m := newMem()
	a, _ := m.CreateAccount(ctx, "a@b.c", "hash")

	rec := &FileRecord{
		AccountID: a.ID, FileID: memUUID(), EncryptedMetadata: []byte("m"),
		ContentHash: "h1", VersionVector: map[string]int{"devA": 1},
		Size: 10, BlobKey: "h1", ModifiedAt: time.Now(), OriginDeviceID: "devA",
	}
	created, err := m.CreateFile(ctx, rec)
	if err != nil || created.Version != 1 {
		t.Fatalf("create: %v %+v", err, created)
	}

	// Two devices both edit v1 -> produce v2A and v2B. Only the first wins.
	up := func(hash string, dev string, base int) error {
		r := &FileRecord{
			AccountID: a.ID, FileID: rec.FileID, EncryptedMetadata: []byte("m"),
			ContentHash: hash, VersionVector: map[string]int{"devA": 1, dev: 2},
			Size: 10, BlobKey: hash, ModifiedAt: time.Now(), OriginDeviceID: dev,
		}
		_, err := m.UpdateFile(ctx, r, base)
		return err
	}
	if err := up("h2a", "devA", 1); err != nil {
		t.Fatalf("first update rejected: %v", err)
	}
	if err := up("h2b", "devB", 1); !errors.Is(err, ErrConflict) {
		t.Fatalf("divergent update not detected as conflict (err=%v)", err)
	}

	// History holds both attempts' winner and the base; nothing lost.
	head, _ := m.GetFile(ctx, a.ID, rec.FileID)
	if head.Version != 2 || head.ContentHash != "h2a" {
		t.Fatalf("unexpected head: %+v", head)
	}
	vs, _ := m.ListVersions(ctx, a.ID, rec.FileID, 10)
	if len(vs) != 2 {
		t.Fatalf("expected 2 versions, got %d", len(vs))
	}

	// Merged resolution: base = current (2) succeeds.
	if err := up("h3merged", "devB", 2); err != nil {
		t.Fatalf("merge-based update rejected: %v", err)
	}
}

func TestMemChangeFeedOrdering(t *testing.T) {
	ctx := context.Background()
	m := newMem()
	a, _ := m.CreateAccount(ctx, "a@b.c", "hash")
	mk := func(i int) {
		rec := &FileRecord{
			AccountID: a.ID, FileID: memUUID(), EncryptedMetadata: []byte("m"),
			ContentHash: fmt.Sprintf("h%d", i), VersionVector: map[string]int{"d": i},
			Size: 1, BlobKey: fmt.Sprintf("h%d", i), ModifiedAt: time.Now(), OriginDeviceID: "d",
		}
		if _, err := m.CreateFile(ctx, rec); err != nil {
			t.Fatal(err)
		}
	}
	for i := 1; i <= 5; i++ {
		mk(i)
	}
	ch, err := m.ListChangesSince(ctx, a.ID, 0, 100)
	if err != nil || len(ch) != 5 {
		t.Fatalf("changes: %v %d", err, len(ch))
	}
	for i := 1; i < len(ch); i++ {
		if ch[i].ChangeSeq <= ch[i-1].ChangeSeq {
			t.Fatal("change feed not monotonic")
		}
	}
	ch2, _ := m.ListChangesSince(ctx, a.ID, ch[2].ChangeSeq, 100)
	if len(ch2) != 2 {
		t.Fatalf("delta feed returned %d, want 2", len(ch2))
	}
}

func TestMemIdempotency(t *testing.T) {
	ctx := context.Background()
	m := newMem()
	a, _ := m.CreateAccount(ctx, "a@b.c", "hash")
	_, found, _ := m.GetIdempotentResponse(ctx, a.ID, "k1")
	if found {
		t.Fatal("unexpected idempotency hit")
	}
	_ = m.SaveIdempotentResponse(ctx, a.ID, "k1", []byte(`{"ok":1}`))
	resp, found, _ := m.GetIdempotentResponse(ctx, a.ID, "k1")
	if !found || string(resp) != `{"ok":1}` {
		t.Fatal("idempotency round-trip failed")
	}
}

func TestMemPairingFlow(t *testing.T) {
	ctx := context.Background()
	m := newMem()
	a, _ := m.CreateAccount(ctx, "a@b.c", "hash")
	pc := &PairingCode{
		Code: "ABCD-EFGH", AccountID: a.ID, RequestingDevice: "devB",
		CreatedAt: time.Now(), ExpiresAt: time.Now().Add(5 * time.Minute),
	}
	if err := m.CreatePairingCode(ctx, pc); err != nil {
		t.Fatal(err)
	}
	if err := m.ApprovePairingCode(ctx, "ABCD-EFGH", "devA", []byte("wrapped")); err != nil {
		t.Fatal(err)
	}
	got, _ := m.PairingCodeByCode(ctx, "ABCD-EFGH")
	if string(got.WrappedMasterKey) != "wrapped" {
		t.Fatal("wrapped key not relayed")
	}
	_ = m.ConsumePairingCode(ctx, "ABCD-EFGH")
	if err := m.ApprovePairingCode(ctx, "ABCD-EFGH", "devA", []byte("x")); !errors.Is(err, ErrPairingExpired) {
		t.Fatal("consumed code still approvable")
	}
}

func TestMemPruneVersions(t *testing.T) {
	ctx := context.Background()
	m := newMem()
	a, _ := m.CreateAccount(ctx, "a@b.c", "hash")
	rec := &FileRecord{
		AccountID: a.ID, FileID: memUUID(), EncryptedMetadata: []byte("m"),
		ContentHash: "h1", VersionVector: map[string]int{"d": 1},
		Size: 1, BlobKey: "h1", ModifiedAt: time.Now(), OriginDeviceID: "d",
	}
	m.CreateFile(ctx, rec)
	for i := 2; i <= 5; i++ {
		up := &FileRecord{
			AccountID: a.ID, FileID: rec.FileID, EncryptedMetadata: []byte("m"),
			ContentHash: fmt.Sprintf("h%d", i), VersionVector: map[string]int{"d": i},
			Size: 1, BlobKey: fmt.Sprintf("h%d", i), ModifiedAt: time.Now(), OriginDeviceID: "d",
		}
		if _, err := m.UpdateFile(ctx, up, i-1); err != nil {
			t.Fatal(err)
		}
	}
	pruned, err := m.PruneVersions(ctx, a.ID, rec.FileID, 2)
	if err != nil {
		t.Fatal(err)
	}
	vs, _ := m.ListVersions(ctx, a.ID, rec.FileID, 10)
	if len(vs) != 2 {
		t.Fatalf("retention kept %d versions, want 2", len(vs))
	}
	if len(pruned) == 0 {
		t.Fatal("expected pruned blob keys to be returned")
	}
}

func TestMergeVersionVector(t *testing.T) {
	out := MergeVersionVector(map[string]int{"a": 3, "b": 1}, map[string]int{"b": 5, "c": 2})
	if out["a"] != 3 || out["b"] != 5 || out["c"] != 2 {
		t.Fatalf("bad merge: %v", out)
	}
}
