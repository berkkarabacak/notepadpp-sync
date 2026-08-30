package blob

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// S3 stores blobs in an S3-compatible object store (AWS S3, Cloudflare R2,
// MinIO). It speaks SigV4 directly using only the standard library, so no AWS
// SDK dependency is required.
//
// Blob keys are the SHA-256 hex of the ciphertext; we reuse that value as the
// SigV4 payload hash on PUT (x-amz-content-sha256), which keeps uploads
// streaming without a separate hashing pass.
type S3 struct {
	endpoint  string // e.g. https://s3.eu-central-1.amazonaws.com or http://localhost:9000
	bucket    string
	region    string
	accessKey string
	secretKey string
	client    *http.Client
}

func NewS3(endpoint, bucket, region, accessKey, secretKey string) (*S3, error) {
	if endpoint == "" || bucket == "" {
		return nil, fmt.Errorf("s3: endpoint and bucket are required")
	}
	if region == "" {
		region = "auto"
	}
	return &S3{
		endpoint:  strings.TrimRight(endpoint, "/"),
		bucket:    bucket,
		region:    region,
		accessKey: accessKey,
		secretKey: secretKey,
		client:    &http.Client{Timeout: 5 * time.Minute},
	}, nil
}

// objectURL uses virtual-hosted-style for AWS endpoints and path-style for
// everything else (MinIO, R2 custom endpoints).
func (s *S3) objectURL(key string) (string, error) {
	u, err := url.Parse(s.endpoint)
	if err != nil {
		return "", err
	}
	esc := url.PathEscape(key)
	if strings.HasSuffix(u.Host, "amazonaws.com") {
		u.Host = s.bucket + "." + u.Host
		u.Path = "/" + esc
	} else {
		u.Path = strings.TrimRight(u.Path, "/") + "/" + url.PathEscape(s.bucket) + "/" + esc
	}
	return u.String(), nil
}

func hmacSHA256(key []byte, data string) []byte {
	m := hmac.New(sha256.New, key)
	m.Write([]byte(data))
	return m.Sum(nil)
}

func (s *S3) signingKey(date string) []byte {
	kDate := hmacSHA256([]byte("AWS4"+s.secretKey), date)
	kRegion := hmacSHA256(kDate, s.region)
	kService := hmacSHA256(kRegion, "s3")
	return hmacSHA256(kService, "aws4_request")
}

// sign adds an Authorization header per AWS Signature Version 4.
func (s *S3) sign(req *http.Request, payloadHashHex string, now time.Time) {
	amzDate := now.UTC().Format("20060102T150405Z")
	date := now.UTC().Format("20060102")

	req.Header.Set("X-Amz-Date", amzDate)
	req.Header.Set("X-Amz-Content-Sha256", payloadHashHex)
	if req.Header.Get("Host") == "" {
		req.Header.Set("Host", req.URL.Host)
	}

	signedHeaders := "host;x-amz-content-sha256;x-amz-date"
	canonicalHeaders := "host:" + req.URL.Host + "\n" +
		"x-amz-content-sha256:" + payloadHashHex + "\n" +
		"x-amz-date:" + amzDate + "\n"

	canonicalURI := req.URL.EscapedPath()
	if canonicalURI == "" {
		canonicalURI = "/"
	}
	canonicalRequest := strings.Join([]string{
		req.Method, canonicalURI, req.URL.RawQuery,
		canonicalHeaders, signedHeaders, payloadHashHex,
	}, "\n")

	scope := date + "/" + s.region + "/s3/aws4_request"
	crHash := sha256.Sum256([]byte(canonicalRequest))
	stringToSign := "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + hex.EncodeToString(crHash[:])

	sig := hex.EncodeToString(hmacSHA256(s.signingKey(date), stringToSign))
	req.Header.Set("Authorization",
		"AWS4-HMAC-SHA256 Credential="+s.accessKey+"/"+scope+
			", SignedHeaders="+signedHeaders+", Signature="+sig)
}

func (s *S3) Put(ctx context.Context, key string, r io.Reader, size int64) error {
	u, err := s.objectURL(key)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPut, u, r)
	if err != nil {
		return err
	}
	req.ContentLength = size
	req.Header.Set("Content-Type", "application/octet-stream")
	s.sign(req, key, time.Now()) // key == sha256 hex of payload
	resp, err := s.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	io.Copy(io.Discard, resp.Body)
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("s3 put: status %d", resp.StatusCode)
	}
	return nil
}

func (s *S3) Get(ctx context.Context, key string) (io.ReadCloser, int64, error) {
	u, err := s.objectURL(key)
	if err != nil {
		return nil, 0, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return nil, 0, err
	}
	emptyHash := hex.EncodeToString(sha256Sum(nil))
	s.sign(req, emptyHash, time.Now())
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, 0, err
	}
	if resp.StatusCode == http.StatusNotFound {
		resp.Body.Close()
		return nil, 0, ErrNotFound
	}
	if resp.StatusCode/100 != 2 {
		resp.Body.Close()
		return nil, 0, fmt.Errorf("s3 get: status %d", resp.StatusCode)
	}
	return resp.Body, resp.ContentLength, nil
}

func (s *S3) Delete(ctx context.Context, key string) error {
	u, err := s.objectURL(key)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodDelete, u, nil)
	if err != nil {
		return err
	}
	emptyHash := hex.EncodeToString(sha256Sum(nil))
	s.sign(req, emptyHash, time.Now())
	resp, err := s.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	io.Copy(io.Discard, resp.Body)
	if resp.StatusCode == http.StatusNotFound || resp.StatusCode/100 == 2 {
		return nil
	}
	return fmt.Errorf("s3 delete: status %d", resp.StatusCode)
}

func sha256Sum(b []byte) []byte {
	s := sha256.Sum256(b)
	return s[:]
}
