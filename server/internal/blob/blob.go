// Package blob provides the ciphertext blob storage abstraction.
// Implementations: local filesystem (default, self-host friendly) and
// S3-compatible object storage (AWS S3, Cloudflare R2, MinIO).
// Blobs are immutable AEAD ciphertext; the server cannot decrypt them.
package blob

import (
	"context"
	"fmt"
	"io"
)

// Store stores and retrieves immutable ciphertext blobs by key.
type Store interface {
	// Put writes the blob. Keys are content-addressed (sha256 hex) so Put is
	// naturally idempotent: re-uploading the same ciphertext is a no-op.
	Put(ctx context.Context, key string, r io.Reader, size int64) error
	Get(ctx context.Context, key string) (io.ReadCloser, int64, error)
	Delete(ctx context.Context, key string) error
}

// ErrNotFound is returned when a blob key does not exist.
var ErrNotFound = fmt.Errorf("blob not found")
