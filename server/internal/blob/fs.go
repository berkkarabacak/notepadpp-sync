package blob

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// FS stores blobs on the local filesystem, sharded by key prefix:
// <dir>/ab/cd/<full-key>. Writes are atomic (temp file + rename).
type FS struct {
	dir string
}

func NewFS(dir string) (*FS, error) {
	if err := os.MkdirAll(dir, 0o750); err != nil {
		return nil, fmt.Errorf("create blob dir: %w", err)
	}
	return &FS{dir: dir}, nil
}

// safePath maps a blob key to a path inside the store directory and refuses
// anything that would escape it (defense in depth against crafted keys).
func (f *FS) safePath(key string) (string, error) {
	clean := filepath.Clean(key)
	if strings.Contains(clean, "..") || filepath.IsAbs(clean) || clean == "." {
		return "", fmt.Errorf("invalid blob key")
	}
	full := filepath.Join(f.dir, clean)
	if !strings.HasPrefix(full, filepath.Clean(f.dir)+string(os.PathSeparator)) {
		return "", fmt.Errorf("invalid blob key")
	}
	return full, nil
}

func shard(key string) string {
	k := strings.TrimSpace(key)
	if len(k) >= 4 {
		return filepath.Join(k[0:2], k[2:4], k)
	}
	return k
}

func (f *FS) Put(ctx context.Context, key string, r io.Reader, size int64) error {
	p, err := f.safePath(shard(key))
	if err != nil {
		return err
	}
	if _, err := os.Stat(p); err == nil {
		// Content-addressed: identical blob already stored.
		io.Copy(io.Discard, r)
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o750); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(p), ".tmp-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if _, err := io.Copy(tmp, r); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpName, p); err != nil {
		return err
	}
	return nil
}

func (f *FS) Get(ctx context.Context, key string) (io.ReadCloser, int64, error) {
	p, err := f.safePath(shard(key))
	if err != nil {
		return nil, 0, err
	}
	file, err := os.Open(p)
	if errors.Is(err, os.ErrNotExist) {
		return nil, 0, ErrNotFound
	}
	if err != nil {
		return nil, 0, err
	}
	st, err := file.Stat()
	if err != nil {
		file.Close()
		return nil, 0, err
	}
	return file, st.Size(), nil
}

func (f *FS) Delete(ctx context.Context, key string) error {
	p, err := f.safePath(shard(key))
	if err != nil {
		return err
	}
	err = os.Remove(p)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	return err
}
