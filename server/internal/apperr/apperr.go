// Package apperr defines the canonical API error shape and helpers.
package apperr

import (
	"encoding/json"
	"log/slog"
	"net/http"
)

// Error is a machine-readable API error.
type Error struct {
	Code       string `json:"error"`
	Message    string `json:"message"`
	HTTPStatus int    `json:"-"`
	// Conflict, when set, carries the server's current file record for a
	// version conflict (HTTP 409) so clients can three-way merge.
	Conflict any `json:"current,omitempty"`
}

func (e *Error) Error() string { return e.Code + ": " + e.Message }

func New(status int, code, msg string) *Error {
	return &Error{Code: code, Message: msg, HTTPStatus: status}
}

var (
	ErrUnauthorized = New(http.StatusUnauthorized, "unauthorized", "missing or invalid credentials")
	ErrForbidden    = New(http.StatusForbidden, "forbidden", "not allowed")
	ErrNotFound     = New(http.StatusNotFound, "not_found", "resource not found")
)

func BadRequest(msg string) *Error { return New(http.StatusBadRequest, "invalid_request", msg) }
func RateLimited(msg string) *Error {
	return New(http.StatusTooManyRequests, "rate_limited", msg)
}
func PayloadTooLarge(msg string) *Error {
	return New(http.StatusRequestEntityTooLarge, "payload_too_large", msg)
}

// Write serializes err to the client. Non-Error failures become 500s and are
// logged; their details are never leaked to clients.
func Write(w http.ResponseWriter, err error) {
	if err == nil {
		return
	}
	ae, ok := err.(*Error)
	if !ok {
		slog.Error("internal error", "err", err)
		ae = New(http.StatusInternalServerError, "internal", "internal server error")
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(ae.HTTPStatus)
	_ = json.NewEncoder(w).Encode(ae)
}

// JSON writes a success response.
func JSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
