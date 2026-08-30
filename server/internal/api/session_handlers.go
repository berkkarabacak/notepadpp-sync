package api

import (
	"errors"
	"net/http"
	"time"

	"npsync/server/internal/apperr"
	"npsync/server/internal/store"
)

type sessionJSON struct {
	EncryptedState string    `json:"encrypted_state"`
	Version        int       `json:"version"`
	UpdatedAt      time.Time `json:"updated_at,omitempty"`
	OriginDeviceID string    `json:"origin_device_id,omitempty"`
}

// Session state is an opaque encrypted blob produced by the plugin
// (open tabs, selected tab, cursor/scroll). The server cannot read it.

func (s *Server) handleGetSession(w http.ResponseWriter, r *http.Request) {
	sess, err := s.st.GetSession(r.Context(), accountID(r))
	if errors.Is(err, store.ErrNotFound) {
		apperr.Write(w, apperr.ErrNotFound)
		return
	}
	if err != nil {
		apperr.Write(w, err)
		return
	}
	apperr.JSON(w, http.StatusOK, sessionJSON{
		EncryptedState: encodeBase64URL(sess.EncryptedState),
		Version:        sess.Version,
		UpdatedAt:      sess.UpdatedAt,
		OriginDeviceID: sess.OriginDeviceID,
	})
}

func (s *Server) handlePutSession(w http.ResponseWriter, r *http.Request) {
	var req sessionJSON
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	state, err := decodeBase64URL(req.EncryptedState)
	if err != nil || len(state) == 0 || len(state) > 1<<20 {
		apperr.Write(w, apperr.BadRequest("encrypted_state missing or oversized (max 1 MiB)"))
		return
	}
	sess := &store.Session{
		AccountID:      accountID(r),
		EncryptedState: state,
		Version:        req.Version,
		OriginDeviceID: deviceID(r),
	}
	if err := s.st.PutSession(r.Context(), sess); err != nil {
		apperr.Write(w, err)
		return
	}
	s.hub.Publish(accountID(r), deviceID(r), wsEventSessionChanged(deviceID(r)))
	apperr.JSON(w, http.StatusOK, sessionJSON{
		EncryptedState: req.EncryptedState,
		Version:        sess.Version,
		UpdatedAt:      sess.UpdatedAt,
		OriginDeviceID: sess.OriginDeviceID,
	})
}
