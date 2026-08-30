// Package ws implements the realtime change-notification hub. Clients connect
// to /ws with an access token and receive JSON events for changes made by
// other devices on the same account. Missed events are recovered via the
// /sync/changes delta feed on reconnect.
package ws

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// Event is a protocol-v1 WebSocket message (see protocol/schemas/v1/ws-event.json).
type Event struct {
	Type           string `json:"type"`
	FileID         string `json:"file_id,omitempty"`
	Version        int    `json:"version,omitempty"`
	ChangeSeq      int64  `json:"change_seq,omitempty"`
	OriginDeviceID string `json:"origin_device_id,omitempty"`
	DeviceID       string `json:"device_id,omitempty"`
}

const (
	writeWait  = 10 * time.Second
	pongWait   = 60 * time.Second
	pingPeriod = 45 * time.Second
	maxMsgSize = 1 << 16 // clients never send large frames; bound abuse
)

type conn struct {
	accountID string
	deviceID  string
	socket    *websocket.Conn
	send      chan []byte
}

// Hub fans change events out to all connections of an account except the
// originating device.
type Hub struct {
	mu      sync.RWMutex
	byAcct  map[string]map[*conn]struct{}
	maxPerA int
}

func NewHub(maxConnsPerAccount int) *Hub {
	if maxConnsPerAccount <= 0 {
		maxConnsPerAccount = 16
	}
	return &Hub{byAcct: map[string]map[*conn]struct{}{}, maxPerA: maxConnsPerAccount}
}

// Publish sends an event to every connection of accountID except the origin
// device (which already knows).
func (h *Hub) Publish(accountID, originDeviceID string, ev Event) {
	payload, err := json.Marshal(ev)
	if err != nil {
		return
	}
	h.mu.RLock()
	defer h.mu.RUnlock()
	for c := range h.byAcct[accountID] {
		if originDeviceID != "" && c.deviceID == originDeviceID {
			continue
		}
		select {
		case c.send <- payload:
		default:
			// Slow consumer: drop the event rather than block the pipeline.
			// The client's next /sync/changes catch-up covers the gap.
		}
	}
}

// Serve upgrades the HTTP request and runs the connection until it dies.
// The token must already be validated by the caller.
func (h *Hub) Serve(ctx context.Context, w http.ResponseWriter, r *http.Request, accountID, deviceID string) error {
	up := websocket.Upgrader{
		ReadBufferSize:  4096,
		WriteBufferSize: 4096,
		// Token already authenticated; origin checks are meaningless for a
		// native (non-browser) client but we reject cross-account use above.
		CheckOrigin: func(r *http.Request) bool { return true },
	}
	socket, err := up.Upgrade(w, r, nil)
	if err != nil {
		return err
	}
	c := &conn{accountID: accountID, deviceID: deviceID, socket: socket, send: make(chan []byte, 64)}

	h.mu.Lock()
	if len(h.byAcct[accountID]) >= h.maxPerA {
		h.mu.Unlock()
		socket.WriteMessage(websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.ClosePolicyViolation, "too many connections"))
		socket.Close()
		return nil
	}
	if h.byAcct[accountID] == nil {
		h.byAcct[accountID] = map[*conn]struct{}{}
	}
	h.byAcct[accountID][c] = struct{}{}
	h.mu.Unlock()

	defer func() {
		h.mu.Lock()
		delete(h.byAcct[accountID], c)
		if len(h.byAcct[accountID]) == 0 {
			delete(h.byAcct, accountID)
		}
		h.mu.Unlock()
		socket.Close()
	}()

	go c.writer()

	socket.SetReadLimit(maxMsgSize)
	socket.SetReadDeadline(time.Now().Add(pongWait))
	socket.SetPongHandler(func(string) error {
		socket.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})
	for {
		// We don't expect application messages from clients; reads exist only
		// to observe close frames and pongs.
		if _, _, err := socket.ReadMessage(); err != nil {
			return nil
		}
	}
}

func (c *conn) writer() {
	ticker := time.NewTicker(pingPeriod)
	defer ticker.Stop()
	for {
		select {
		case msg, ok := <-c.send:
			c.socket.SetWriteDeadline(time.Now().Add(writeWait))
			if !ok {
				c.socket.WriteMessage(websocket.CloseMessage, []byte{})
				return
			}
			if err := c.socket.WriteMessage(websocket.TextMessage, msg); err != nil {
				slog.Debug("ws write failed", "err", err)
				return
			}
		case <-ticker.C:
			c.socket.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.socket.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		}
	}
}
