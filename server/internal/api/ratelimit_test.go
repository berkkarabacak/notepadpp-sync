package api

import (
	"net/http"
	"net/netip"
	"testing"

	"npsync/server/internal/config"
)

func TestClientIPIgnoresForwardedForByDefault(t *testing.T) {
	req, _ := http.NewRequest(http.MethodGet, "/", nil)
	req.RemoteAddr = "203.0.113.8:44321"
	req.Header.Set("X-Forwarded-For", "198.51.100.4, 203.0.113.9")
	if got := clientIP(req, nil); got != "203.0.113.8" {
		t.Fatalf("untrusted peer must ignore X-Forwarded-For, got %q", got)
	}
}

func TestClientIPUsesRightmostUntrustedHop(t *testing.T) {
	trusted, err := config.ParseTrustedProxies("10.0.0.0/8, 192.168.1.5")
	if err != nil {
		t.Fatal(err)
	}
	req, _ := http.NewRequest(http.MethodGet, "/", nil)
	req.RemoteAddr = "10.1.2.3:8080"
	// The client prepended a lie. The trusted proxy appended 203.0.113.50,
	// then the next trusted hop appended itself.
	req.Header.Set("X-Forwarded-For", "1.2.3.4, 203.0.113.50, 10.9.9.9")
	if got := clientIP(req, trusted); got != "203.0.113.50" {
		t.Fatalf("got %q", got)
	}

	req.RemoteAddr = "198.51.100.7:9" // not a trusted proxy
	if got := clientIP(req, trusted); got != "198.51.100.7" {
		t.Fatalf("direct client must not be taken from the header, got %q", got)
	}
}

func TestParseTrustedProxiesRejectsGarbage(t *testing.T) {
	if _, err := config.ParseTrustedProxies("10.0.0.1, not-an-ip"); err == nil {
		t.Fatal("expected an error")
	}
	got, err := config.ParseTrustedProxies(" 127.0.0.1 , ")
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || got[0] != netip.MustParsePrefix("127.0.0.1/32") {
		t.Fatalf("got %v", got)
	}
}
