package auth

import (
	"context"
	"crypto"
	"crypto/hmac"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"strings"
	"sync"
	"time"
)

const idTokenSkew = time.Minute

// GoogleIdentity is the verified subject and email from a Google ID token.
// It is an authentication result only — never an encryption key.
type GoogleIdentity struct {
	Subject       string
	Email         string
	EmailVerified bool
}

// NewCodeVerifier returns a PKCE verifier and its S256 challenge.
// The verifier stays on the server; only the challenge is put in the
// authorization URL.
func NewCodeVerifier() (verifier, challenge string, err error) {
	raw := make([]byte, 32)
	if _, err = rand.Read(raw); err != nil {
		return "", "", err
	}
	verifier = base64.RawURLEncoding.EncodeToString(raw)
	return verifier, PKCEChallengeS256(verifier), nil
}

// PKCEChallengeS256 is BASE64URL(SHA256(verifier)) without padding.
func PKCEChallengeS256(verifier string) string {
	sum := sha256.Sum256([]byte(verifier))
	return base64.RawURLEncoding.EncodeToString(sum[:])
}

// PollSecretMatches reports whether presented is the secret whose SHA-256
// hex was stored. Comparison is constant time.
func PollSecretMatches(storedHash, presented string) bool {
	if storedHash == "" || presented == "" {
		return false
	}
	got := HashTokenSHA256(presented)
	return hmac.Equal([]byte(storedHash), []byte(got))
}

// RandomB64URL returns n random bytes encoded as unpadded base64url.
func RandomB64URL(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

type jwtHeader struct {
	Alg string `json:"alg"`
	Kid string `json:"kid"`
	Typ string `json:"typ"`
}

type audience []string

func (a *audience) UnmarshalJSON(b []byte) error {
	if len(b) > 0 && b[0] == '"' {
		var s string
		if err := json.Unmarshal(b, &s); err != nil {
			return err
		}
		*a = []string{s}
		return nil
	}
	var many []string
	if err := json.Unmarshal(b, &many); err != nil {
		return err
	}
	*a = many
	return nil
}

// flexibleBool accepts JSON true/false and the strings "true"/"false".
type flexibleBool bool

func (b *flexibleBool) UnmarshalJSON(data []byte) error {
	switch string(data) {
	case "true", `"true"`:
		*b = true
	case "false", `"false"`, "null":
		*b = false
	default:
		return errors.New("email_verified is not a boolean")
	}
	return nil
}

type idTokenClaims struct {
	Issuer        string       `json:"iss"`
	Subject       string       `json:"sub"`
	Audience      audience     `json:"aud"`
	ExpiresAt     int64        `json:"exp"`
	IssuedAt      int64        `json:"iat"`
	Nonce         string       `json:"nonce"`
	Email         string       `json:"email"`
	EmailVerified flexibleBool `json:"email_verified"`
}

// RS256Kid returns the key id of an RS256 JWT. Any other alg is rejected
// before a key is fetched, which blocks algorithm-confusion attacks.
func RS256Kid(token string) (string, error) {
	h, _, _, err := splitJWT(token)
	if err != nil {
		return "", err
	}
	if h.Alg != "RS256" {
		return "", errors.New("id token alg must be RS256")
	}
	if h.Kid == "" {
		return "", errors.New("id token missing kid")
	}
	return h.Kid, nil
}

// VerifyRS256IDToken checks the signature and the OIDC claims we rely on.
func VerifyRS256IDToken(token string, pub *rsa.PublicKey, now time.Time, issuer, audience, nonce string) (*GoogleIdentity, error) {
	if pub == nil || pub.N == nil {
		return nil, errors.New("missing signing key")
	}
	h, signingInput, sig, err := splitJWTParts(token)
	if err != nil {
		return nil, err
	}
	if h.Alg != "RS256" {
		return nil, errors.New("id token alg must be RS256")
	}
	sum := sha256.Sum256([]byte(signingInput))
	if err := rsa.VerifyPKCS1v15(pub, crypto.SHA256, sum[:], sig); err != nil {
		return nil, errors.New("id token signature rejected")
	}
	payload, err := decodeB64(strings.Split(signingInput, ".")[1])
	if err != nil {
		return nil, errors.New("malformed id token payload")
	}
	var claims idTokenClaims
	if err := json.Unmarshal(payload, &claims); err != nil {
		return nil, errors.New("malformed id token claims")
	}
	if !issuerAccepted(claims.Issuer, issuer) {
		return nil, errors.New("id token issuer rejected")
	}
	if !audienceContains(claims.Audience, audience) {
		return nil, errors.New("id token audience rejected")
	}
	if claims.IssuedAt == 0 || claims.ExpiresAt == 0 || claims.ExpiresAt < claims.IssuedAt {
		return nil, errors.New("id token has invalid timestamps")
	}
	if now.After(time.Unix(claims.ExpiresAt, 0).Add(idTokenSkew)) {
		return nil, errors.New("id token expired")
	}
	if time.Unix(claims.IssuedAt, 0).After(now.Add(idTokenSkew)) {
		return nil, errors.New("id token issued in the future")
	}
	if nonce == "" || claims.Nonce == "" || claims.Nonce != nonce {
		return nil, errors.New("id token nonce rejected")
	}
	if !claims.EmailVerified {
		return nil, errors.New("google email is not verified")
	}
	if claims.Subject == "" || claims.Email == "" {
		return nil, errors.New("id token missing sub or email")
	}
	return &GoogleIdentity{
		Subject:       claims.Subject,
		Email:         claims.Email,
		EmailVerified: true,
	}, nil
}

func issuerAccepted(got, want string) bool {
	if got == want {
		return true
	}
	// Google documents both forms. Accept either when the configured issuer
	// is one of them, so a self-hoster is not broken by the alternate form.
	google := got == "https://accounts.google.com" || got == "accounts.google.com"
	wantGoogle := want == "https://accounts.google.com" || want == "accounts.google.com"
	return google && wantGoogle
}

func audienceContains(aud audience, want string) bool {
	for _, a := range aud {
		if a == want {
			return true
		}
	}
	return false
}

func splitJWT(token string) (jwtHeader, string, []byte, error) {
	return splitJWTParts(token)
}

func splitJWTParts(token string) (jwtHeader, string, []byte, error) {
	var h jwtHeader
	if len(token) == 0 || len(token) > 8192 {
		return h, "", nil, errors.New("malformed id token")
	}
	parts := strings.Split(token, ".")
	if len(parts) != 3 || parts[0] == "" || parts[1] == "" {
		return h, "", nil, errors.New("malformed id token")
	}
	rawH, err := decodeB64(parts[0])
	if err != nil {
		return h, "", nil, errors.New("malformed id token header")
	}
	if err := json.Unmarshal(rawH, &h); err != nil {
		return h, "", nil, errors.New("malformed id token header")
	}
	sig, err := decodeB64(parts[2])
	if err != nil {
		return h, "", nil, errors.New("malformed id token signature")
	}
	return h, parts[0] + "." + parts[1], sig, nil
}

func decodeB64(s string) ([]byte, error) {
	if b, err := base64.RawURLEncoding.DecodeString(s); err == nil {
		return b, nil
	}
	if b, err := base64.URLEncoding.DecodeString(s); err == nil {
		return b, nil
	}
	return base64.StdEncoding.DecodeString(s)
}

// JWKSCache fetches and caches RSA keys from an OIDC jwks_uri.
type JWKSCache struct {
	urlFn  func() string
	client *http.Client
	ttl    time.Duration
	now    func() time.Time

	mu        sync.Mutex
	keys      map[string]*rsa.PublicKey
	fetchedAt time.Time
	cachedURL string
}

func NewJWKSCache(urlFn func() string, client *http.Client, ttl time.Duration) *JWKSCache {
	if client == nil {
		client = http.DefaultClient
	}
	if ttl <= 0 {
		ttl = time.Hour
	}
	return &JWKSCache{urlFn: urlFn, client: client, ttl: ttl, now: time.Now, keys: map[string]*rsa.PublicKey{}}
}

// Key returns the RSA public key for kid, refreshing when the cache is stale
// or does not contain that kid (key rotation).
func (c *JWKSCache) Key(ctx context.Context, kid string) (*rsa.PublicKey, error) {
	if kid == "" {
		return nil, errors.New("missing kid")
	}
	if key, ok := c.lookup(kid, false); ok {
		return key, nil
	}
	if err := c.refresh(ctx); err != nil {
		return nil, err
	}
	key, ok := c.lookup(kid, true)
	if !ok {
		return nil, fmt.Errorf("jwks has no key %q", kid)
	}
	return key, nil
}

func (c *JWKSCache) lookup(kid string, ignoreTTL bool) (*rsa.PublicKey, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	url := ""
	if c.urlFn != nil {
		url = c.urlFn()
	}
	fresh := ignoreTTL || (url == c.cachedURL && c.now().Sub(c.fetchedAt) < c.ttl && len(c.keys) > 0)
	if !fresh {
		return nil, false
	}
	key, ok := c.keys[kid]
	return key, ok
}

func (c *JWKSCache) refresh(ctx context.Context) error {
	url := c.urlFn()
	if url == "" {
		return errors.New("jwks url is empty")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Accept", "application/json")
	resp, err := c.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return err
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("jwks http %d", resp.StatusCode)
	}
	keys, err := parseJWKS(body)
	if err != nil {
		return err
	}
	c.mu.Lock()
	c.keys = keys
	c.fetchedAt = c.now()
	c.cachedURL = url
	c.mu.Unlock()
	return nil
}

type jwkKey struct {
	Kty string `json:"kty"`
	Kid string `json:"kid"`
	Use string `json:"use"`
	Alg string `json:"alg"`
	N   string `json:"n"`
	E   string `json:"e"`
}

func parseJWKS(body []byte) (map[string]*rsa.PublicKey, error) {
	var doc struct {
		Keys []jwkKey `json:"keys"`
	}
	if err := json.Unmarshal(body, &doc); err != nil {
		return nil, errors.New("malformed jwks")
	}
	out := make(map[string]*rsa.PublicKey, len(doc.Keys))
	for _, k := range doc.Keys {
		if k.Kty != "RSA" || k.Kid == "" || k.N == "" || k.E == "" {
			continue
		}
		if k.Use != "" && k.Use != "sig" {
			continue
		}
		if k.Alg != "" && k.Alg != "RS256" {
			continue
		}
		pub, err := rsaPublicFromModExp(k.N, k.E)
		if err != nil {
			continue
		}
		out[k.Kid] = pub
	}
	if len(out) == 0 {
		return nil, errors.New("jwks contained no usable RS256 keys")
	}
	return out, nil
}

func rsaPublicFromModExp(nB64, eB64 string) (*rsa.PublicKey, error) {
	nb, err := decodeB64(nB64)
	if err != nil {
		return nil, err
	}
	eb, err := decodeB64(eB64)
	if err != nil {
		return nil, err
	}
	n := new(big.Int).SetBytes(nb)
	e := new(big.Int).SetBytes(eb)
	bits := n.BitLen()
	if bits < 2048 || bits > 4096 {
		return nil, errors.New("rsa key size rejected")
	}
	if e.Sign() <= 0 || e.BitLen() > 31 {
		return nil, errors.New("rsa exponent rejected")
	}
	return &rsa.PublicKey{N: n, E: int(e.Int64())}, nil
}
