package api

import (
	"net/http"
	"strings"

	"npsync/server/internal/apperr"
	"npsync/server/internal/ws"
)

func wsEventSessionChanged(originDeviceID string) ws.Event {
	return ws.Event{Type: "session_changed", OriginDeviceID: originDeviceID}
}

// handleWebSocket authenticates via ?token= (browsers/native clients cannot
// set headers during upgrade) and serves the realtime feed.
func (s *Server) handleWebSocket(w http.ResponseWriter, r *http.Request) {
	token := r.URL.Query().Get("token")
	if token == "" {
		// Also accept the standard header form.
		h := r.Header.Get("Authorization")
		if strings.HasPrefix(h, "Bearer ") {
			token = strings.TrimPrefix(h, "Bearer ")
		}
	}
	if token == "" {
		apperr.Write(w, apperr.ErrUnauthorized)
		return
	}
	claims, err := s.signer.Verify(token)
	if err != nil {
		apperr.Write(w, apperr.ErrUnauthorized)
		return
	}
	dev, err := s.st.DeviceByID(r.Context(), claims.AccountID, claims.DeviceID)
	if err != nil || dev.RevokedAt != nil {
		apperr.Write(w, apperr.ErrUnauthorized)
		return
	}
	if err := s.hub.Serve(r.Context(), w, r, claims.AccountID, claims.DeviceID); err != nil {
		apperr.Write(w, err)
	}
}
