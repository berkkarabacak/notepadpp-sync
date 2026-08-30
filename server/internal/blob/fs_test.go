package blob

import (
	"bytes"
	"context"
	"io"
	"testing"
)

func TestFSRoundTrip(t *testing.T) {
	dir := t.TempDir()
	fs, err := NewFS(dir)
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	payload := []byte("ciphertext-bytes")

	if err := fs.Put(ctx, "abc123", bytes.NewReader(payload), int64(len(payload))); err != nil {
		t.Fatal(err)
	}
	rc, n, err := fs.Get(ctx, "abc123")
	if err != nil {
		t.Fatal(err)
	}
	defer rc.Close()
	got, _ := io.ReadAll(rc)
	if n != int64(len(payload)) || !bytes.Equal(got, payload) {
		t.Fatalf("round trip mismatch: %d %q", n, got)
	}

	// Idempotent: same key, same content.
	if err := fs.Put(ctx, "abc123", bytes.NewReader(payload), int64(len(payload))); err != nil {
		t.Fatal(err)
	}

	if err := fs.Delete(ctx, "abc123"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := fs.Get(ctx, "abc123"); err != ErrNotFound {
		t.Fatalf("want ErrNotFound, got %v", err)
	}
}

func TestFSRejectsTraversal(t *testing.T) {
	fs, err := NewFS(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	for _, key := range []string{"../evil", "..", "/abs", "a/../../b"} {
		if err := fs.Put(ctx, key, bytes.NewReader([]byte("x")), 1); err == nil {
			t.Fatalf("traversal key accepted: %q", key)
		}
	}
}
