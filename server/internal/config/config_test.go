package config

import "testing"

func TestGoogleConfigRequiresBothCredentials(t *testing.T) {
	t.Setenv("NPSYNC_REQUIRE_HTTPS", "false")
	t.Setenv("NPSYNC_TOKEN_SIGNING_KEY", "")
	t.Setenv("NPSYNC_GOOGLE_CLIENT_ID", "test-google-client-id")
	t.Setenv("NPSYNC_GOOGLE_CLIENT_SECRET", "")
	if _, err := Load(); err == nil {
		t.Fatal("client id without secret was accepted")
	}
}

func TestGoogleRedirectMustBeHTTPSInProduction(t *testing.T) {
	t.Setenv("NPSYNC_REQUIRE_HTTPS", "true")
	t.Setenv("NPSYNC_TOKEN_SIGNING_KEY", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
	t.Setenv("NPSYNC_BASE_URL", "http://sync.example.com")
	t.Setenv("NPSYNC_GOOGLE_CLIENT_ID", "test-google-client-id")
	t.Setenv("NPSYNC_GOOGLE_CLIENT_SECRET", "test-google-client-secret")
	t.Setenv("NPSYNC_GOOGLE_REDIRECT_URI", "")
	if _, err := Load(); err == nil {
		t.Fatal("http public redirect was accepted in production")
	}
}

func TestGoogleDisabledByDefault(t *testing.T) {
	t.Setenv("NPSYNC_REQUIRE_HTTPS", "false")
	t.Setenv("NPSYNC_TOKEN_SIGNING_KEY", "")
	t.Setenv("NPSYNC_BASE_URL", "http://localhost:8080")
	t.Setenv("NPSYNC_GOOGLE_CLIENT_ID", "")
	t.Setenv("NPSYNC_GOOGLE_CLIENT_SECRET", "")
	t.Setenv("NPSYNC_GOOGLE_REDIRECT_URI", "")
	t.Setenv("NPSYNC_GOOGLE_AUTH_URL", "")
	cfg, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.GoogleEnabled() {
		t.Fatal("google enabled with empty credentials")
	}
	if cfg.GoogleAuthURL != "https://accounts.google.com/o/oauth2/v2/auth" {
		t.Fatalf("auth url: %s", cfg.GoogleAuthURL)
	}
	if got := cfg.GoogleRedirectURIOrDefault(); got != "http://localhost:8080/auth/google/callback" {
		t.Fatalf("redirect: %s", got)
	}
}

func TestParseTrustedProxies(t *testing.T) {
	empty, err := ParseTrustedProxies("  ")
	if err != nil || len(empty) != 0 {
		t.Fatalf("empty: %v %v", empty, err)
	}
	got, err := ParseTrustedProxies("10.0.0.0/8, 2001:db8::1")
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 || got[0].String() != "10.0.0.0/8" || got[1].String() != "2001:db8::1/128" {
		t.Fatalf("got %v", got)
	}
}
