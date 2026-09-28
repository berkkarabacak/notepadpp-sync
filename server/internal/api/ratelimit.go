package api

import (
	"net"
	"net/http"
	"net/netip"
	"strings"
	"sync"
	"time"
)

// loginLimiter is a simple per-key fixed-window rate limiter for
// authentication endpoints (brute-force protection at the IP+email level).
type loginLimiter struct {
	mu      sync.Mutex
	limit   int
	window  time.Duration
	buckets map[string]*bucket
	now     func() time.Time
}

type bucket struct {
	count   int
	resetAt time.Time
}

func newLoginLimiter(perMinute int) *loginLimiter {
	if perMinute <= 0 {
		perMinute = 10
	}
	l := &loginLimiter{
		limit:   perMinute,
		window:  time.Minute,
		buckets: map[string]*bucket{},
		now:     time.Now,
	}
	go l.gc()
	return l
}

func (l *loginLimiter) gc() {
	t := time.NewTicker(5 * time.Minute)
	defer t.Stop()
	for range t.C {
		l.mu.Lock()
		for k, b := range l.buckets {
			if l.now().After(b.resetAt) {
				delete(l.buckets, k)
			}
		}
		l.mu.Unlock()
	}
}

// Allow reports whether one more attempt is permitted for key.
func (l *loginLimiter) Allow(key string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := l.now()
	b, ok := l.buckets[key]
	if !ok || now.After(b.resetAt) {
		l.buckets[key] = &bucket{count: 1, resetAt: now.Add(l.window)}
		return true
	}
	if b.count >= l.limit {
		return false
	}
	b.count++
	return true
}

func limiterKey(r *http.Request, email string, trusted []netip.Prefix) string {
	return clientIP(r, trusted) + "|" + strings.ToLower(strings.TrimSpace(email))
}

// clientIP is the address used for login rate limiting.
//
// X-Forwarded-For is ignored unless the direct peer (RemoteAddr) is in
// trusted. A client can otherwise pick a fresh header value and skip the
// limit. When the peer is trusted, the client address is the rightmost
// forwarded hop that is not itself a trusted proxy — the address the proxy
// appended — so a spoofed prefix does not become the bucket key.
func clientIP(r *http.Request, trusted []netip.Prefix) string {
	remote := remoteHost(r.RemoteAddr)
	if len(trusted) == 0 || !ipTrusted(remote, trusted) {
		return remote
	}
	xff := r.Header.Get("X-Forwarded-For")
	if xff == "" {
		return remote
	}
	parts := strings.Split(xff, ",")
	for i := len(parts) - 1; i >= 0; i-- {
		cand := strings.TrimSpace(parts[i])
		if cand == "" {
			continue
		}
		if !ipTrusted(cand, trusted) {
			return cand
		}
	}
	return remote
}

func remoteHost(remoteAddr string) string {
	host, _, err := net.SplitHostPort(strings.TrimSpace(remoteAddr))
	if err != nil {
		return strings.Trim(strings.TrimSpace(remoteAddr), "[]")
	}
	return strings.Trim(host, "[]")
}

func ipTrusted(ip string, trusted []netip.Prefix) bool {
	addr, ok := parseIP(ip)
	if !ok {
		return false
	}
	for _, p := range trusted {
		if p.Contains(addr) {
			return true
		}
	}
	return false
}

func parseIP(ip string) (netip.Addr, bool) {
	ip = strings.TrimSpace(ip)
	if host, _, err := net.SplitHostPort(ip); err == nil {
		ip = host
	}
	ip = strings.Trim(ip, "[]")
	addr, err := netip.ParseAddr(ip)
	if err != nil {
		return netip.Addr{}, false
	}
	return addr, true
}
