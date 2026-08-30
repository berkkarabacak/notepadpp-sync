package api

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"time"

	"npsync/server/internal/apperr"
	"npsync/server/internal/auth"
	"npsync/server/internal/blob"
	"npsync/server/internal/store"
	"npsync/server/internal/ws"
)

// fileJSON is the wire representation of a file record (protocol v1).
type fileJSON struct {
	FileID            string         `json:"file_id"`
	EncryptedMetadata string         `json:"encrypted_metadata"`
	EncryptedContent  string         `json:"encrypted_content,omitempty"`
	ContentHash       string         `json:"content_hash"`
	Version           int            `json:"version"`
	BaseVersion       int            `json:"base_version"`
	VersionVector     map[string]int `json:"version_vector"`
	Deleted           bool           `json:"deleted"`
	Size              int64          `json:"size"`
	ModifiedAt        time.Time      `json:"modified_at"`
	OriginDeviceID    string         `json:"origin_device_id"`
	ChangeSeq         int64          `json:"change_seq"`
	IdempotencyKey    string         `json:"idempotency_key,omitempty"`
}

func toFileJSON(f *store.FileRecord, content string) fileJSON {
	return fileJSON{
		FileID:            f.FileID,
		EncryptedMetadata: encodeBase64URL(f.EncryptedMetadata),
		EncryptedContent:  content,
		ContentHash:       f.ContentHash,
		Version:           f.Version,
		VersionVector:     f.VersionVector,
		Deleted:           f.Deleted,
		Size:              f.Size,
		ModifiedAt:        f.ModifiedAt,
		OriginDeviceID:    f.OriginDeviceID,
		ChangeSeq:         f.ChangeSeq,
	}
}

// blobKey derives the content-addressed storage key from the ciphertext hash.
func blobKey(contentHash string) string { return contentHash }

// validateIncomingFile checks an uploaded file payload for shape and size.
func (s *Server) validateIncomingFile(req *fileJSON) error {
	if !isUUID(req.FileID) {
		return apperr.BadRequest("file_id must be a UUID")
	}
	if !isHex64(req.ContentHash) {
		return apperr.BadRequest("content_hash must be 64 lowercase hex chars")
	}
	meta, err := decodeBase64URL(req.EncryptedMetadata)
	if err != nil || len(meta) == 0 || len(meta) > 64*1024 {
		return apperr.BadRequest("encrypted_metadata missing or oversized")
	}
	if req.Deleted {
		return nil // tombstones carry no content
	}
	content, err := decodeBase64URL(req.EncryptedContent)
	if err != nil || len(content) == 0 {
		return apperr.BadRequest("encrypted_content required")
	}
	if int64(len(content)) > s.cfg.MaxFileBytes {
		return apperr.PayloadTooLarge(fmt.Sprintf("file exceeds %d byte limit", s.cfg.MaxFileBytes))
	}
	// The declared hash must match the received ciphertext; blobs are
	// content-addressed so this also prevents storage of corrupt uploads.
	if auth.HashTokenSHA256(string(content)) != req.ContentHash {
		return apperr.BadRequest("content_hash does not match encrypted_content")
	}
	if req.Size != int64(len(content)) {
		return apperr.BadRequest("size does not match encrypted_content")
	}
	return nil
}

func isHex64(s string) bool {
	if len(s) != 64 {
		return false
	}
	for _, c := range []byte(s) {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

// storeBlob writes the ciphertext into blob storage under its content hash.
func (s *Server) storeBlob(r *http.Request, req *fileJSON) error {
	if req.Deleted {
		return nil
	}
	content, _ := decodeBase64URL(req.EncryptedContent)
	return s.blobs.Put(r.Context(), blobKey(req.ContentHash), bytes.NewReader(content), int64(len(content)))
}

func (s *Server) recordFromRequest(r *http.Request, req *fileJSON) *store.FileRecord {
	modified := req.ModifiedAt
	if modified.IsZero() {
		modified = s.now()
	}
	return &store.FileRecord{
		AccountID:         accountID(r),
		FileID:            req.FileID,
		EncryptedMetadata: mustDecode(req.EncryptedMetadata),
		ContentHash:       req.ContentHash,
		VersionVector:     req.VersionVector,
		Deleted:           req.Deleted,
		Size:              req.Size,
		BlobKey:           blobKey(req.ContentHash),
		ModifiedAt:        modified,
		OriginDeviceID:    deviceID(r),
	}
}

func mustDecode(s string) []byte {
	b, _ := decodeBase64URL(s)
	return b
}

// publish broadcasts a change event to the account's other devices.
func (s *Server) publish(accountID, originDeviceID string, f *store.FileRecord) {
	typ := "file_changed"
	if f.Deleted {
		typ = "file_deleted"
	}
	s.hub.Publish(accountID, originDeviceID, ws.Event{
		Type: typ, FileID: f.FileID, Version: f.Version,
		ChangeSeq: f.ChangeSeq, OriginDeviceID: originDeviceID,
	})
}

// ---- handlers ----

func (s *Server) handleListFiles(w http.ResponseWriter, r *http.Request) {
	files, err := s.st.ListFiles(r.Context(), accountID(r))
	if err != nil {
		apperr.Write(w, err)
		return
	}
	out := make([]fileJSON, 0, len(files))
	for i := range files {
		out = append(out, toFileJSON(&files[i], ""))
	}
	apperr.JSON(w, http.StatusOK, map[string]any{"files": out})
}

func (s *Server) handleGetFile(w http.ResponseWriter, r *http.Request) {
	f, err := s.st.GetFile(r.Context(), accountID(r), r.PathValue("fileId"))
	if err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	content := ""
	if !f.Deleted {
		c, err := s.readBlob(r, f.BlobKey)
		if err != nil {
			apperr.Write(w, err)
			return
		}
		content = c
	}
	apperr.JSON(w, http.StatusOK, toFileJSON(f, content))
}

func (s *Server) readBlob(r *http.Request, key string) (string, error) {
	rc, _, err := s.blobs.Get(r.Context(), key)
	if errors.Is(err, blob.ErrNotFound) {
		return "", apperr.ErrNotFound
	}
	if err != nil {
		return "", err
	}
	defer rc.Close()
	var buf bytes.Buffer
	if _, err := buf.ReadFrom(rc); err != nil {
		return "", err
	}
	return encodeBase64URL(buf.Bytes()), nil
}

func (s *Server) handleCreateFile(w http.ResponseWriter, r *http.Request) {
	var req fileJSON
	if err := decodeJSON(w, r, &req, s.cfg.MaxFileBytes*2); err != nil {
		apperr.Write(w, err)
		return
	}
	if err := s.validateIncomingFile(&req); err != nil {
		apperr.Write(w, err)
		return
	}
	// Idempotency: a retried create returns the original response.
	if resp, ok := s.checkIdempotency(w, r, req.IdempotencyKey); ok {
		writeRawJSON(w, http.StatusOK, resp)
		return
	}
	if err := s.storeBlob(r, &req); err != nil {
		apperr.Write(w, err)
		return
	}
	rec := s.recordFromRequest(r, &req)
	created, err := s.st.CreateFile(r.Context(), rec)
	if errors.Is(err, store.ErrDuplicate) {
		apperr.Write(w, apperr.New(http.StatusConflict, "duplicate", "file_id already exists; use PUT"))
		return
	}
	if err != nil {
		apperr.Write(w, err)
		return
	}
	s.publish(accountID(r), deviceID(r), created)
	resp, _ := json.Marshal(toFileJSON(created, ""))
	s.saveIdempotency(r, req.IdempotencyKey, resp)
	writeRawJSON(w, http.StatusCreated, resp)
}

func (s *Server) handleUpdateFile(w http.ResponseWriter, r *http.Request) {
	fileID := r.PathValue("fileId")
	var req fileJSON
	if err := decodeJSON(w, r, &req, s.cfg.MaxFileBytes*2); err != nil {
		apperr.Write(w, err)
		return
	}
	req.FileID = fileID
	if !isUUID(fileID) {
		apperr.Write(w, apperr.BadRequest("file id must be a UUID"))
		return
	}
	if err := s.validateIncomingFile(&req); err != nil {
		apperr.Write(w, err)
		return
	}
	if resp, ok := s.checkIdempotency(w, r, req.IdempotencyKey); ok {
		writeRawJSON(w, http.StatusOK, resp)
		return
	}
	rec := s.recordFromRequest(r, &req)
	// Store the blob BEFORE committing metadata so a head record never points
	// at a missing blob (blobs are content-addressed, so this is retry-safe).
	if err := s.storeBlob(r, &req); err != nil {
		apperr.Write(w, err)
		return
	}
	updated, err := s.st.UpdateFile(r.Context(), rec, req.BaseVersion)
	if errors.Is(err, store.ErrConflict) {
		// Return the server's current record so the client can 3-way merge.
		cur, gerr := s.st.GetFile(r.Context(), accountID(r), fileID)
		if gerr != nil {
			apperr.Write(w, mapStoreErr(gerr))
			return
		}
		curJSON := toFileJSON(cur, "")
		apperr.Write(w, &apperr.Error{
			Code: "conflict", HTTPStatus: http.StatusConflict,
			Message:  fmt.Sprintf("base_version %d is stale; current version is %d", req.BaseVersion, cur.Version),
			Conflict: curJSON,
		})
		return
	}
	if err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	s.pruneVersions(r, fileID)
	s.publish(accountID(r), deviceID(r), updated)
	resp, _ := json.Marshal(toFileJSON(updated, ""))
	s.saveIdempotency(r, req.IdempotencyKey, resp)
	writeRawJSON(w, http.StatusOK, resp)
}

func (s *Server) handleDeleteFile(w http.ResponseWriter, r *http.Request) {
	fileID := r.PathValue("fileId")
	cur, err := s.st.GetFile(r.Context(), accountID(r), fileID)
	if err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	tomb := *cur
	tomb.Deleted = true
	tomb.ModifiedAt = s.now()
	tomb.OriginDeviceID = deviceID(r)
	tomb.Size = 0
	updated, err := s.st.UpdateFile(r.Context(), &tomb, cur.Version)
	if err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	s.publish(accountID(r), deviceID(r), updated)
	apperr.JSON(w, http.StatusOK, toFileJSON(updated, ""))
}

// pruneVersions applies retention and garbage-collects unreferenced blobs.
func (s *Server) pruneVersions(r *http.Request, fileID string) {
	if s.cfg.VersionRetention <= 0 {
		return
	}
	keys, err := s.st.PruneVersions(r.Context(), accountID(r), fileID, s.cfg.VersionRetention)
	if err != nil {
		return
	}
	for _, k := range keys {
		_ = s.blobs.Delete(r.Context(), k)
	}
}

func (s *Server) handleListVersions(w http.ResponseWriter, r *http.Request) {
	limit := 100
	if q := r.URL.Query().Get("limit"); q != "" {
		if n, err := strconv.Atoi(q); err == nil && n > 0 && n <= 500 {
			limit = n
		}
	}
	vs, err := s.st.ListVersions(r.Context(), accountID(r), r.PathValue("fileId"), limit)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	type versionJSON struct {
		Version        int            `json:"version"`
		ContentHash    string         `json:"content_hash"`
		Size           int64          `json:"size"`
		Deleted        bool           `json:"deleted"`
		ModifiedAt     time.Time      `json:"modified_at"`
		OriginDeviceID string         `json:"origin_device_id"`
		VersionVector  map[string]int `json:"version_vector"`
	}
	out := make([]versionJSON, 0, len(vs))
	for _, v := range vs {
		out = append(out, versionJSON{
			Version: v.Version, ContentHash: v.ContentHash, Size: v.Size,
			Deleted: v.Deleted, ModifiedAt: v.ModifiedAt,
			OriginDeviceID: v.OriginDeviceID, VersionVector: v.VersionVector,
		})
	}
	apperr.JSON(w, http.StatusOK, map[string]any{"versions": out})
}

type restoreRequest struct {
	Version        int    `json:"version"`
	IdempotencyKey string `json:"idempotency_key,omitempty"`
}

func (s *Server) handleRestoreVersion(w http.ResponseWriter, r *http.Request) {
	fileID := r.PathValue("fileId")
	var req restoreRequest
	if err := decodeJSON(w, r, &req, 1<<20); err != nil {
		apperr.Write(w, err)
		return
	}
	v, err := s.st.GetVersion(r.Context(), accountID(r), fileID, req.Version)
	if err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	cur, err := s.st.GetFile(r.Context(), accountID(r), fileID)
	if err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	// Restore = write the old version's payload as a new head version.
	rec := &store.FileRecord{
		AccountID: accountID(r), FileID: fileID,
		EncryptedMetadata: v.EncryptedMetadata, ContentHash: v.ContentHash,
		VersionVector: store.MergeVersionVector(cur.VersionVector, v.VersionVector),
		Deleted:       v.Deleted, Size: v.Size, BlobKey: v.BlobKey,
		ModifiedAt: s.now(), OriginDeviceID: deviceID(r),
	}
	updated, err := s.st.UpdateFile(r.Context(), rec, cur.Version)
	if err != nil {
		apperr.Write(w, mapStoreErr(err))
		return
	}
	s.publish(accountID(r), deviceID(r), updated)
	apperr.JSON(w, http.StatusOK, toFileJSON(updated, ""))
}

// ---- batch ----

type batchRequest struct {
	Uploads   []fileJSON `json:"uploads,omitempty"`
	Downloads []string   `json:"downloads,omitempty"` // file IDs
}

type batchResultItem struct {
	FileID   string    `json:"file_id"`
	OK       bool      `json:"ok"`
	Error    string    `json:"error,omitempty"`
	Conflict *fileJSON `json:"conflict,omitempty"`
	Record   *fileJSON `json:"record,omitempty"`
	Content  string    `json:"content,omitempty"`
}

func (s *Server) handleBatch(w http.ResponseWriter, r *http.Request) {
	var req batchRequest
	if err := decodeJSON(w, r, &req, s.cfg.MaxBatchBytes*2); err != nil {
		apperr.Write(w, err)
		return
	}
	var total int64
	for i := range req.Uploads {
		total += req.Uploads[i].Size
	}
	if total > s.cfg.MaxBatchBytes {
		apperr.Write(w, apperr.PayloadTooLarge("batch exceeds size limit"))
		return
	}
	results := make([]batchResultItem, 0, len(req.Uploads)+len(req.Downloads))

	for i := range req.Uploads {
		u := &req.Uploads[i]
		item := batchResultItem{FileID: u.FileID}
		if err := s.validateIncomingFile(u); err != nil {
			item.OK, item.Error = false, "invalid_request"
			results = append(results, item)
			continue
		}
		rec := s.recordFromRequest(r, u)
		var out *store.FileRecord
		var err error
		if _, gerr := s.st.GetFile(r.Context(), accountID(r), u.FileID); errors.Is(gerr, store.ErrNotFound) {
			out, err = s.st.CreateFile(r.Context(), rec)
		} else if gerr == nil {
			// Client's declared base decides: stale base -> conflict.
			out, err = s.st.UpdateFile(r.Context(), rec, u.BaseVersion)
			if errors.Is(err, store.ErrConflict) {
				cur, cerr := s.st.GetFile(r.Context(), accountID(r), u.FileID)
				if cerr == nil {
					cj := toFileJSON(cur, "")
					item.OK, item.Error, item.Conflict = false, "conflict", &cj
					results = append(results, item)
					continue
				}
			}
		} else {
			err = gerr
		}
		if err != nil {
			item.OK, item.Error = false, "internal"
		} else {
			if s.storeBlob(r, u) != nil {
				item.OK, item.Error = false, "internal"
			} else {
				item.OK = true
				rj := toFileJSON(out, "")
				item.Record = &rj
				s.publish(accountID(r), deviceID(r), out)
			}
		}
		results = append(results, item)
	}

	for _, id := range req.Downloads {
		item := batchResultItem{FileID: id}
		f, err := s.st.GetFile(r.Context(), accountID(r), id)
		if err != nil {
			item.Error = "not_found"
		} else {
			item.OK = true
			rj := toFileJSON(f, "")
			item.Record = &rj
			if !f.Deleted {
				if c, err := s.readBlob(r, f.BlobKey); err == nil {
					item.Content = c
				}
			}
		}
		results = append(results, item)
	}

	apperr.JSON(w, http.StatusOK, map[string]any{"results": results})
}

// ---- changes feed ----

func (s *Server) handleChanges(w http.ResponseWriter, r *http.Request) {
	var since int64
	if q := r.URL.Query().Get("since"); q != "" {
		n, err := strconv.ParseInt(q, 10, 64)
		if err != nil || n < 0 {
			apperr.Write(w, apperr.BadRequest("invalid since parameter"))
			return
		}
		since = n
	}
	changes, err := s.st.ListChangesSince(r.Context(), accountID(r), since, 1000)
	if err != nil {
		apperr.Write(w, err)
		return
	}
	type changeJSON struct {
		ChangeSeq      int64  `json:"change_seq"`
		FileID         string `json:"file_id"`
		Version        int    `json:"version"`
		Kind           string `json:"kind"`
		OriginDeviceID string `json:"origin_device_id"`
	}
	out := make([]changeJSON, 0, len(changes))
	var maxSeq = since
	for _, c := range changes {
		out = append(out, changeJSON{
			ChangeSeq: c.ChangeSeq, FileID: c.FileID, Version: c.Version,
			Kind: c.Kind, OriginDeviceID: c.OriginDeviceID,
		})
		if c.ChangeSeq > maxSeq {
			maxSeq = c.ChangeSeq
		}
	}
	apperr.JSON(w, http.StatusOK, map[string]any{"changes": out, "latest_seq": maxSeq})
}

// ---- idempotency helpers ----

func (s *Server) checkIdempotency(w http.ResponseWriter, r *http.Request, key string) ([]byte, bool) {
	if key == "" {
		key = r.Header.Get("Idempotency-Key")
	}
	if key == "" {
		return nil, false
	}
	resp, found, err := s.st.GetIdempotentResponse(r.Context(), accountID(r), key)
	if err != nil || !found {
		return nil, false
	}
	return resp, true
}

func (s *Server) saveIdempotency(r *http.Request, key string, resp []byte) {
	if key == "" {
		key = r.Header.Get("Idempotency-Key")
	}
	if key == "" {
		return
	}
	_ = s.st.SaveIdempotentResponse(r.Context(), accountID(r), key, resp)
}

func writeRawJSON(w http.ResponseWriter, status int, raw []byte) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(raw)
}
