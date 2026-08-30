package api

import (
	"crypto/rand"
	"errors"
	"net/http"
	"strings"
	"time"

	"npsync/server/internal/apperr"
	"npsync/server/internal/store"
	"npsync/server/internal/ws"
)

type deviceJSON struct {
	ID         string     `json:"id"`
	Name       string     `json:"name"`
	CreatedAt  time.Time  `json:"created_at"`
	LastSeenAt time.Time  `json:"last_seen_at"`
	RevokedAt  *time.Time `json:"revoked_at,omitempty"`
	Current    bool       `json:"current"`
}

func toDeviceJSON(d store.Device, currentID string) deviceJSON {
	return deviceJSON{
		ID: d.ID, Name: d.Name, CreatedAt: d.CreatedAt,
		LastSeenAt: d.LastSeenAt, RevokedAt: d.RevokedAt, Current: d.ID == currentID,
	}
}

func (s *Server) handleListDevices(w http.ResponseWriter, r *http.Request) {
	devs, err := s.st.ListDevices(r.Context(), accountID(r))
	if err != nil {
		apperr.Write(w, err)
		return
	}
	out := make([]deviceJSON, 0, len(devs))
	for _, d := range devs {
		out = append(out, toDeviceJSON(d, deviceID(r)))
	}
	apperr.JSON(w, http.StatusOK, map[string]any{"devices": out})
}

type renameDeviceRequest struct {
	Name string `json:"name"`
}

func (s *Server) handleRenameDevice(w http.ResponseWriter, r *http.Request) {
	var req renameDeviceRequest
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	req.Name = strings.TrimSpace(req.Name)
	if req.Name == "" || len(req.Name) > 64 {
		apperr.Write(w, apperr.BadRequest("name required (max 64 chars)"))
		return
	}
	if err := s.st.RenameDevice(r.Context(), accountID(r), r.PathValue("id"), req.Name); err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	apperr.JSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) handleRevokeDevice(w http.ResponseWriter, r *http.Request) {
	target := r.PathValue("id")
	if err := s.st.RevokeDevice(r.Context(), accountID(r), target); err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	// Live-kill any of that device's sockets.
	s.hub.Publish(accountID(r), "", ws.Event{Type: "device_revoked", DeviceID: target})
	apperr.JSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// ---- device pairing ----

type pairRequest struct {
	Action           string `json:"action"`
	PairingCode      string `json:"pairing_code,omitempty"`
	ForDeviceID      string `json:"for_device_id,omitempty"`
	WrappedMasterKey string `json:"wrapped_master_key,omitempty"` // base64url, opaque to server
}

const pairingCodeTTL = 5 * time.Minute

// generatePairingCode returns an "XXXX-XXXX" code from an unambiguous
// Crockford-ish alphabet (no 0/O, 1/I/L).
func generatePairingCode() (string, error) {
	const alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
	b := make([]byte, 8)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	out := make([]byte, 0, 9)
	for i, c := range b {
		if i == 4 {
			out = append(out, '-')
		}
		out = append(out, alphabet[int(c)%len(alphabet)])
	}
	return string(out), nil
}

func (s *Server) handlePair(w http.ResponseWriter, r *http.Request) {
	var req pairRequest
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	switch req.Action {
	case "request":
		s.pairRequestCode(w, r)
	case "approve":
		s.pairApprove(w, r, &req)
	case "poll":
		s.pairPoll(w, r, &req)
	default:
		apperr.Write(w, apperr.BadRequest("action must be request|approve|poll"))
	}
}

func (s *Server) pairRequestCode(w http.ResponseWriter, r *http.Request) {
	code, err := generatePairingCode()
	if err != nil {
		apperr.Write(w, err)
		return
	}
	pc := &store.PairingCode{
		Code:             code,
		AccountID:        accountID(r),
		RequestingDevice: deviceID(r),
		CreatedAt:        s.now(),
		ExpiresAt:        s.now().Add(pairingCodeTTL),
	}
	if err := s.st.CreatePairingCode(r.Context(), pc); err != nil {
		apperr.Write(w, err)
		return
	}
	apperr.JSON(w, http.StatusOK, map[string]any{
		"pairing_code": code,
		"expires_at":   pc.ExpiresAt,
	})
}

func (s *Server) pairApprove(w http.ResponseWriter, r *http.Request, req *pairRequest) {
	if req.PairingCode == "" || req.WrappedMasterKey == "" {
		apperr.Write(w, apperr.BadRequest("pairing_code and wrapped_master_key required"))
		return
	}
	wrapped, err := decodeBase64URL(req.WrappedMasterKey)
	if err != nil || len(wrapped) == 0 || len(wrapped) > 4096 {
		apperr.Write(w, apperr.BadRequest("invalid wrapped_master_key"))
		return
	}
	pc, err := s.st.PairingCodeByCode(r.Context(), req.PairingCode)
	if err != nil || pc.AccountID != accountID(r) {
		apperr.Write(w, apperr.ErrNotFound)
		return
	}
	if pc.RequestingDevice == deviceID(r) {
		apperr.Write(w, apperr.BadRequest("cannot approve your own pairing request"))
		return
	}
	if err := s.st.ApprovePairingCode(r.Context(), req.PairingCode, deviceID(r), wrapped); err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	apperr.JSON(w, http.StatusOK, map[string]string{"status": "approved"})
}

func (s *Server) pairPoll(w http.ResponseWriter, r *http.Request, req *pairRequest) {
	if req.PairingCode == "" {
		apperr.Write(w, apperr.BadRequest("pairing_code required"))
		return
	}
	pc, err := s.st.PairingCodeByCode(r.Context(), req.PairingCode)
	if err != nil || pc.AccountID != accountID(r) || pc.RequestingDevice != deviceID(r) {
		apperr.Write(w, apperr.ErrNotFound)
		return
	}
	if s.now().After(pc.ExpiresAt) || pc.ConsumedAt != nil {
		apperr.Write(w, apperr.New(http.StatusGone, "pairing_expired", "pairing code expired"))
		return
	}
	if len(pc.WrappedMasterKey) == 0 {
		apperr.JSON(w, http.StatusOK, map[string]string{"status": "pending"})
		return
	}
	if err := s.st.ConsumePairingCode(r.Context(), req.PairingCode); err != nil {
		apperr.Write(w, err)
		return
	}
	apperr.JSON(w, http.StatusOK, map[string]string{
		"status":             "approved",
		"wrapped_master_key": encodeBase64URL(pc.WrappedMasterKey),
	})
}

func mapStoreErr(err error) error {
	switch {
	case errors.Is(err, store.ErrNotFound):
		return apperr.ErrNotFound
	case errors.Is(err, store.ErrPairingExpired):
		return apperr.New(http.StatusGone, "pairing_expired", "pairing code expired or invalid")
	case errors.Is(err, store.ErrDuplicate):
		return apperr.New(http.StatusConflict, "duplicate", "resource already exists")
	default:
		return err
	}
}

// UUID validation for path parameters.
func isUUID(s string) bool {
	if len(s) != 36 {
		return false
	}
	for i, c := range []byte(s) {
		switch i {
		case 8, 13, 18, 23:
			if c != '-' {
				return false
			}
		default:
			if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
				return false
			}
		}
	}
	return true
}
