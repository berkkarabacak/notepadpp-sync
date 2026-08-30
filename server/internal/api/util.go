package api

import "encoding/base64"

func decodeBase64URL(s string) ([]byte, error) {
	return base64.RawURLEncoding.DecodeString(s)
}

func encodeBase64URL(b []byte) string {
	return base64.RawURLEncoding.EncodeToString(b)
}
