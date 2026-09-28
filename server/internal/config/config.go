// Package config loads server configuration from environment variables.
// Every tunable limit lives here rather than being hardcoded.
package config

import (
	"fmt"
	"net/netip"
	"net/url"
	"os"
	"strconv"
	"strings"
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

	// Google OAuth/OIDC. Empty client ID and secret disables Google sign-in.
	// The server is the confidential client (Authorization Code + PKCE).
	// Endpoint URLs default to Google and exist so tests can point at a fake IdP.
	GoogleClientID     string
	GoogleClientSecret string
	GoogleRedirectURI  string
	GoogleAuthURL      string
	GoogleTokenURL     string
	GoogleJWKSURL      string
	GoogleIssuer       string

	// TrustedProxies are the reverse proxies allowed to supply the client
	// address via X-Forwarded-For. Empty means the header is ignored and
	// login rate limits use the direct connection.
	TrustedProxies []netip.Prefix
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
		ListenAddr:         env("NPSYNC_LISTEN_ADDR", ":8080"),
		DatabaseURL:        env("NPSYNC_DATABASE_URL", "postgres://npsync:npsync_dev_password@localhost:5432/npsync_dev?sslmode=disable"),
		BaseURL:            env("NPSYNC_BASE_URL", "http://localhost:8080"),
		RequireHTTPS:       envBool("NPSYNC_REQUIRE_HTTPS", false),
		RegistrationOpen:   envBool("NPSYNC_REGISTRATION_OPEN", true),
		AccessTokenTTL:     15 * time.Minute,
		RefreshTokenTTL:    90 * 24 * time.Hour,
		BlobBackend:        env("NPSYNC_BLOB_BACKEND", "fs"),
		BlobFSDir:          env("NPSYNC_BLOB_FS_DIR", "data/blobs"),
		S3Endpoint:         env("NPSYNC_S3_ENDPOINT", ""),
		S3Bucket:           env("NPSYNC_S3_BUCKET", ""),
		S3Region:           env("NPSYNC_S3_REGION", "auto"),
		S3AccessKey:        env("NPSYNC_S3_ACCESS_KEY", ""),
		S3SecretKey:        env("NPSYNC_S3_SECRET_KEY", ""),
		MaxFileBytes:       envInt64("NPSYNC_MAX_FILE_BYTES", 100<<20),
		MaxBatchBytes:      envInt64("NPSYNC_MAX_BATCH_BYTES", 250<<20),
		MaxDevices:         int(envInt64("NPSYNC_MAX_DEVICES", 10)),
		VersionRetention:   int(envInt64("NPSYNC_VERSION_RETENTION", 30)),
		LoginRatePerMin:    int(envInt64("NPSYNC_LOGIN_RATE_PER_MIN", 10)),
		LoginLockoutAfter:  int(envInt64("NPSYNC_LOGIN_LOCKOUT_AFTER", 8)),
		LoginLockoutFor:    15 * time.Minute,
		GoogleClientID:     env("NPSYNC_GOOGLE_CLIENT_ID", ""),
		GoogleClientSecret: env("NPSYNC_GOOGLE_CLIENT_SECRET", ""),
		GoogleRedirectURI:  env("NPSYNC_GOOGLE_REDIRECT_URI", ""),
		GoogleAuthURL:      env("NPSYNC_GOOGLE_AUTH_URL", "https://accounts.google.com/o/oauth2/v2/auth"),
		GoogleTokenURL:     env("NPSYNC_GOOGLE_TOKEN_URL", "https://oauth2.googleapis.com/token"),
		GoogleJWKSURL:      env("NPSYNC_GOOGLE_JWKS_URL", "https://www.googleapis.com/oauth2/v3/certs"),
		GoogleIssuer:       env("NPSYNC_GOOGLE_ISSUER", "https://accounts.google.com"),
	}
	proxies, err := ParseTrustedProxies(env("NPSYNC_TRUSTED_PROXIES", ""))
	if err != nil {
		return nil, err
	}
	c.TrustedProxies = proxies

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

	if (c.GoogleClientID == "") != (c.GoogleClientSecret == "") {
		return nil, fmt.Errorf("NPSYNC_GOOGLE_CLIENT_ID and NPSYNC_GOOGLE_CLIENT_SECRET must both be set (or both left empty)")
	}
	if c.GoogleEnabled() {
		redir := c.GoogleRedirectURIOrDefault()
		u, err := url.Parse(redir)
		if err != nil || u.Host == "" || (u.Scheme != "https" && u.Scheme != "http") {
			return nil, fmt.Errorf("Google redirect URI must be an absolute http(s) URL")
		}
		host := u.Hostname()
		local := host == "localhost" || host == "127.0.0.1" || host == "::1"
		if c.RequireHTTPS && u.Scheme != "https" && !local {
			return nil, fmt.Errorf("Google redirect URI must be https when NPSYNC_REQUIRE_HTTPS is set")
		}
	}

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

// GoogleEnabled reports whether Google sign-in is configured.
func (c *Config) GoogleEnabled() bool {
	return c.GoogleClientID != "" && c.GoogleClientSecret != ""
}

// GoogleRedirectURIOrDefault is the browser redirect target registered in
// the Google Cloud console. It defaults to {BaseURL}/auth/google/callback.
func (c *Config) GoogleRedirectURIOrDefault() string {
	if c.GoogleRedirectURI != "" {
		return c.GoogleRedirectURI
	}
	return strings.TrimRight(c.BaseURL, "/") + "/auth/google/callback"
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

// ParseTrustedProxies parses a comma-separated list of IPs or CIDRs.
// A bare IP is treated as a single host (/32 or /128). An empty string
// yields no trusted proxies.
func ParseTrustedProxies(raw string) ([]netip.Prefix, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return nil, nil
	}
	var out []netip.Prefix
	for _, part := range strings.Split(raw, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if strings.Contains(part, "/") {
			p, err := netip.ParsePrefix(part)
			if err != nil {
				return nil, fmt.Errorf("NPSYNC_TRUSTED_PROXIES: %q: %w", part, err)
			}
			out = append(out, p.Masked())
			continue
		}
		addr, err := netip.ParseAddr(part)
		if err != nil {
			return nil, fmt.Errorf("NPSYNC_TRUSTED_PROXIES: %q: %w", part, err)
		}
		bits := addr.BitLen()
		out = append(out, netip.PrefixFrom(addr, bits).Masked())
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
