package api

import (
	"net/http"
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

func limiterKey(r *http.Request, email string) string {
	ip := r.Header.Get("X-Forwarded-For")
	if ip != "" {
		ip = strings.Split(ip, ",")[0]
	} else {
		ip = r.RemoteAddr
	}
	return strings.TrimSpace(ip) + "|" + strings.ToLower(email)
}
