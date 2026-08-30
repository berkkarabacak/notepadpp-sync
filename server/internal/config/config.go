// Package config loads server configuration from environment variables.
// Every tunable limit lives here rather than being hardcoded.
package config

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

type Config struct {
	ListenAddr       string
	DatabaseURL      string
	BaseURL          string
	RequireHTTPS     bool
	RegistrationOpen bool

	TokenSigningKey []byte
	AccessTokenTTL  time.Duration
	RefreshTokenTTL time.Duration

	BlobBackend string // "fs" | "s3"
	BlobFSDir   string
	S3Endpoint  string
	S3Bucket    string
	S3Region    string
	S3AccessKey string
	S3SecretKey string

	MaxFileBytes     int64
	MaxBatchBytes    int64
	MaxDevices       int
	VersionRetention int

	LoginRatePerMin   int
	LoginLockoutAfter int
	LoginLockoutFor   time.Duration
}

func env(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func envBool(key string, def bool) bool {
	v := os.Getenv(key)
	if v == "" {
		return def
	}
	b, err := strconv.ParseBool(v)
	if err != nil {
		return def
	}
	return b
}

func envInt64(key string, def int64) int64 {
	v := os.Getenv(key)
	if v == "" {
		return def
	}
	n, err := strconv.ParseInt(v, 10, 64)
	if err != nil {
		return def
	}
	return n
}

// Load reads configuration from the environment and validates it.
func Load() (*Config, error) {
	c := &Config{
		ListenAddr:        env("NPSYNC_LISTEN_ADDR", ":8080"),
		DatabaseURL:       env("NPSYNC_DATABASE_URL", "postgres://npsync:npsync_dev_password@localhost:5432/npsync_dev?sslmode=disable"),
		BaseURL:           env("NPSYNC_BASE_URL", "http://localhost:8080"),
		RequireHTTPS:      envBool("NPSYNC_REQUIRE_HTTPS", false),
		RegistrationOpen:  envBool("NPSYNC_REGISTRATION_OPEN", true),
		AccessTokenTTL:    15 * time.Minute,
		RefreshTokenTTL:   90 * 24 * time.Hour,
		BlobBackend:       env("NPSYNC_BLOB_BACKEND", "fs"),
		BlobFSDir:         env("NPSYNC_BLOB_FS_DIR", "data/blobs"),
		S3Endpoint:        env("NPSYNC_S3_ENDPOINT", ""),
		S3Bucket:          env("NPSYNC_S3_BUCKET", ""),
		S3Region:          env("NPSYNC_S3_REGION", "auto"),
		S3AccessKey:       env("NPSYNC_S3_ACCESS_KEY", ""),
		S3SecretKey:       env("NPSYNC_S3_SECRET_KEY", ""),
		MaxFileBytes:      envInt64("NPSYNC_MAX_FILE_BYTES", 100<<20),
		MaxBatchBytes:     envInt64("NPSYNC_MAX_BATCH_BYTES", 250<<20),
		MaxDevices:        int(envInt64("NPSYNC_MAX_DEVICES", 10)),
		VersionRetention:  int(envInt64("NPSYNC_VERSION_RETENTION", 30)),
		LoginRatePerMin:   int(envInt64("NPSYNC_LOGIN_RATE_PER_MIN", 10)),
		LoginLockoutAfter: int(envInt64("NPSYNC_LOGIN_LOCKOUT_AFTER", 8)),
		LoginLockoutFor:   15 * time.Minute,
	}

	keyHex := env("NPSYNC_TOKEN_SIGNING_KEY", "")
	if keyHex == "" {
		// Development default — never acceptable in production.
		if c.RequireHTTPS {
			return nil, fmt.Errorf("NPSYNC_TOKEN_SIGNING_KEY is required in production")
		}
		keyHex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
	}
	key, err := decodeHex(keyHex)
	if err != nil || len(key) != 32 {
		return nil, fmt.Errorf("NPSYNC_TOKEN_SIGNING_KEY must be 64 hex chars (32 bytes)")
	}
	c.TokenSigningKey = key

	switch c.BlobBackend {
	case "fs":
	case "s3":
		if c.S3Endpoint == "" || c.S3Bucket == "" || c.S3AccessKey == "" || c.S3SecretKey == "" {
			return nil, fmt.Errorf("s3 blob backend requires NPSYNC_S3_ENDPOINT/BUCKET/ACCESS_KEY/SECRET_KEY")
		}
	default:
		return nil, fmt.Errorf("unknown NPSYNC_BLOB_BACKEND %q (want fs|s3)", c.BlobBackend)
	}
	return c, nil
}

func decodeHex(s string) ([]byte, error) {
	if len(s)%2 != 0 {
		return nil, fmt.Errorf("odd hex length")
	}
	out := make([]byte, len(s)/2)
	for i := 0; i < len(out); i++ {
		hi, err1 := hexNibble(s[2*i])
		lo, err2 := hexNibble(s[2*i+1])
		if err1 != nil || err2 != nil {
			return nil, fmt.Errorf("invalid hex")
		}
		out[i] = hi<<4 | lo
	}
	return out, nil
}

func hexNibble(c byte) (byte, error) {
	switch {
	case c >= '0' && c <= '9':
		return c - '0', nil
	case c >= 'a' && c <= 'f':
		return c - 'a' + 10, nil
	case c >= 'A' && c <= 'F':
		return c - 'A' + 10, nil
	}
	return 0, fmt.Errorf("not hex")
}
