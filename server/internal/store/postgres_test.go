package store

import (
	"context"
	"errors"
	"fmt"
	"os"
	"sort"
	"strings"
	"testing"
	"time"

	"npsync/server/migrations"
)

// Postgres integration tests run only when NPSYNC_TEST_DATABASE_URL is set
// (CI provides a postgres service; unit tests use the Mem store).
func openTestPostgres(t *testing.T) *Postgres {
	t.Helper()
	url := os.Getenv("NPSYNC_TEST_DATABASE_URL")
	if url == "" {
		t.Skip("NPSYNC_TEST_DATABASE_URL not set; skipping postgres integration test")
	}
	n := 0
	p, err := OpenPostgres(context.Background(), url, func() string {
		n++
		return fmt.Sprintf("22222222-3333-4444-8555-%012d", n)
	})
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	t.Cleanup(func() { p.Close() })

	// Apply embedded migrations (the test DB is created empty by the service).
	entries, err := migrations.FS.ReadDir(".")
	if err != nil {
		t.Fatalf("read migrations: %v", err)
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), ".sql") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	ctx := context.Background()
	for _, name := range names {
		body, err := migrations.FS.ReadFile(name)
		if err != nil {
			t.Fatalf("read %s: %v", name, err)
		}
		if _, err := p.db.ExecContext(ctx, string(body)); err != nil {
			t.Fatalf("migrate %s: %v", name, err)
		}
	}

	// Clean slate per test: wipe tables used by the suite.
	for _, table := range []string{"changes", "file_versions", "files",
		"idempotency_keys", "pairing_codes", "sessions", "refresh_tokens",
		"devices", "accounts"} {
		if _, err := p.db.ExecContext(ctx, "DELETE FROM "+table); err != nil {
			t.Fatalf("clean %s: %v", table, err)
		}
	}
	return p
}

func TestPostgresAccountAndConflict(t *testing.T) {
	p := openTestPostgres(t)
	ctx := context.Background()

	a, err := p.CreateAccount(ctx, "pg@example.com", "hash")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := p.CreateAccount(ctx, "pg@example.com", "hash2"); !errors.Is(err, ErrEmailTaken) {
		t.Fatalf("want ErrEmailTaken, got %v", err)
	}

	rec := &FileRecord{
		AccountID: a.ID, FileID: "33333333-4444-4555-8666-777777777777",
		EncryptedMetadata: []byte("m"), ContentHash: "h1",
		VersionVector: map[string]int{"d": 1}, Size: 1, BlobKey: "h1",
		ModifiedAt: time.Now(), OriginDeviceID: "dddddddd-0000-4000-8000-000000000000",
	}
	created, err := p.CreateFile(ctx, rec)
	if err != nil || created.Version != 1 {
		t.Fatalf("create: %v %+v", err, created)
	}

	up := &FileRecord{
		AccountID: a.ID, FileID: rec.FileID, EncryptedMetadata: []byte("m"),
		ContentHash: "h2", VersionVector: map[string]int{"d": 2},
		Size: 1, BlobKey: "h2", ModifiedAt: time.Now(), OriginDeviceID: rec.OriginDeviceID,
	}
	if _, err := p.UpdateFile(ctx, up, 0); !errors.Is(err, ErrConflict) {
		t.Fatalf("stale base_version accepted: %v", err)
	}
	if _, err := p.UpdateFile(ctx, up, 1); err != nil {
		t.Fatalf("valid update rejected: %v", err)
	}

	head, err := p.GetFile(ctx, a.ID, rec.FileID)
	if err != nil || head.Version != 2 || head.ContentHash != "h2" {
		t.Fatalf("head: %v %+v", err, head)
	}

	ch, err := p.ListChangesSince(ctx, a.ID, 0, 10)
	if err != nil || len(ch) != 2 {
		t.Fatalf("changes: %v %d", err, len(ch))
	}

	vs, err := p.ListVersions(ctx, a.ID, rec.FileID, 10)
	if err != nil || len(vs) != 2 {
		t.Fatalf("versions: %v %d", err, len(vs))
	}
}

func TestPostgresDeviceRevocationCascade(t *testing.T) {
	p := openTestPostgres(t)
	ctx := context.Background()
	a, _ := p.CreateAccount(ctx, "cascade@example.com", "hash")
	d, err := p.CreateDevice(ctx, a.ID, "laptop")
	if err != nil {
		t.Fatal(err)
	}
	tok, err := p.CreateRefreshToken(ctx, d.ID, "hash-x", time.Now().Add(time.Hour))
	if err != nil {
		t.Fatal(err)
	}
	if err := p.RevokeDevice(ctx, a.ID, d.ID); err != nil {
		t.Fatal(err)
	}
	got, err := p.RefreshTokenByHash(ctx, tok.TokenHash)
	if err != nil || got.RevokedAt == nil {
		t.Fatalf("token not revoked with device: %v", err)
	}
}
