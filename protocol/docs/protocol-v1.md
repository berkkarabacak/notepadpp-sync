# NPSync Wire Protocol — Version 1

Status: stable (v1) · Header: `X-NPSync-Protocol: 1`

This document defines the contract between the Notepad++ Sync plugin
(client) and the sync server. JSON Schemas for the message bodies live in
`protocol/schemas/v1/`.

## Versioning rules

- Every request and response carries the header `X-NPSync-Protocol: <n>`.
- A server MUST reject requests with a major protocol it does not support,
  returning `426 Upgrade Required` with body
  `{"error":"unsupported_protocol","server_protocols":[1]}`.
- A client MUST check the header on every response and, on mismatch, stop
  syncing and show: *"This server speaks an incompatible protocol version.
  Update the plugin or the server."*
- Backward-incompatible changes bump the major version and add a new schema
  directory (`schemas/v2/`, …). Additive changes (new optional fields) do not.

## Transport & security

- HTTPS is required in production (`NPSYNC_REQUIRE_HTTPS=true` makes the
  server reject plain HTTP behind proxies). Local development may use HTTP.
- Authentication uses `Authorization: Bearer <access_token>` except on
  `/auth/register`, `/auth/login`, `/auth/refresh`, `/health`, `/ready`.
- All mutating endpoints accept an optional `Idempotency-Key` header.
  Replaying a request with the same key returns the original result without
  applying the mutation twice (protection against duplicate uploads after
  network retries).
- Request/response bodies are JSON unless noted. Binary blob payloads are
  base64url-encoded inside JSON fields.
- **The server never receives plaintext file contents or plaintext
  filenames/paths.** Content fields contain AEAD ciphertext produced on the
  client.

## Error format

```json
{ "error": "machine_readable_code", "message": "human readable detail" }
```

Standard codes: `invalid_request`, `unauthorized`, `forbidden`,
`not_found`, `conflict` (HTTP 409, version conflict — see below),
`rate_limited` (429), `payload_too_large` (413), `unsupported_protocol`
(426), `internal` (500).

## Endpoints

### Auth

| Method | Path             | Auth | Description |
|--------|------------------|------|-------------|
| POST   | /auth/register   | no   | Create account. Rate-limited. |
| POST   | /auth/login      | no   | Email+password → tokens. Rate-limited + brute-force lockout. |
| POST   | /auth/refresh    | no   | Rotate refresh token → new token pair. |
| POST   | /auth/logout     | yes  | Revoke the current device session/refresh token. |

`POST /auth/register` and `/auth/login` take
`{email, password, device_name}` and return
`{account_id, device_id, access_token, access_expires_at, refresh_token}`.

- Access tokens are short-lived JWTs (default 15 min).
- Refresh tokens are opaque 256-bit values, stored hashed, bound to one
  device, rotated on every use, revocable via `DELETE /devices/{id}`.

### Devices

| Method | Path                | Description |
|--------|---------------------|-------------|
| GET    | /devices            | List devices of the account. |
| POST   | /devices/pair       | Device-pairing flow (see below). |
| DELETE | /devices/{id}       | Revoke a device (kills its refresh tokens). |
| PATCH  | /devices/{id}       | Rename a device. |

### Sync

| Method | Path                                   | Description |
|--------|----------------------------------------|-------------|
| GET    | /sync/files                            | List current file records (metadata only, encrypted). |
| GET    | /sync/files/{fileId}                   | Get one record incl. current encrypted content blob. |
| POST   | /sync/files                            | Create a file (first version). Idempotent. |
| PUT    | /sync/files/{fileId}                   | Upload new version. **409 + both versions on conflict.** |
| DELETE | /sync/files/{fileId}                   | Tombstone a file. |
| POST   | /sync/batch                            | Multi-file upload/download in one call (size-limited). |
| GET    | /sync/changes?since={change_seq}       | Delta feed: all changes with `change_seq > since`. |
| GET    | /sync/files/{fileId}/versions          | Version history (retention-limited). |
| POST   | /sync/files/{fileId}/restore           | Restore an old version as a new head version. |

### Session (optional feature)

| Method | Path              | Description |
|--------|-------------------|-------------|
| GET    | /session          | Get encrypted session state (tabs/cursor). |
| PUT    | /session          | Replace encrypted session state. |

### Misc

| Method | Path     | Description |
|--------|----------|-------------|
| GET    | /health  | Liveness: `{"status":"ok"}`. |
| GET    | /ready   | Readiness (DB reachable, migrations applied). |
| GET    | /ws      | WebSocket upgrade (Bearer token via `?token=`). |

## File record

See `schemas/v1/file-record.json`. Key fields:

- `file_id` — client-generated UUID; stable across renames.
- `encrypted_metadata` — AEAD ciphertext of `{relative_path, size, ...}`.
- `encrypted_content` — AEAD ciphertext of the file bytes.
- `content_hash` — SHA-256 of the **ciphertext** (integrity; not of plaintext,
  so the server can verify uploads without seeing content).
- `version` — monotone integer per file, incremented by the server on each
  accepted write.
- `base_version` — the version the client's edit was based on.
- `version_vector` — `{device_id: counter}` map; the server merges it into
  the record's vector so divergence is detectable without wall clocks.
- `deleted` — tombstone flag; deletions propagate like writes.
- `idempotency_key` — client-supplied UUID for safe retries.

## Conflict semantics

A `PUT` is a conflict when the record's current `version` ≠ the request's
`base_version`, i.e. another device wrote first. The server then:

1. Rejects the write with HTTP 409.
2. Returns `{"error":"conflict", "current": <file record>, "your_base_version": n}`.
3. The client performs a three-way merge locally (base / local / remote) and
   either uploads the merged version with the new `base_version`, or leaves
   resolution to the user. Both divergent payloads remain available via the
   versions endpoint until retention prunes them — nothing is lost.

## WebSocket events (`/ws`)

Frames are JSON text messages, `schemas/v1/ws-event.json`:

```json
{ "type": "file_changed", "file_id": "…", "version": 12, "change_seq": 9182, "origin_device_id": "…" }
{ "type": "file_deleted", "file_id": "…", "change_seq": 9183, "origin_device_id": "…" }
{ "type": "device_revoked", "device_id": "…" }
{ "type": "ping" }
```

A client ignores events whose `origin_device_id` equals its own device.
On reconnect the client catches up with `GET /sync/changes?since=<last_seq>`.

## Device pairing (key transfer)

1. New device B: `POST /devices/pair {"action":"request"}` → `{pairing_code, expires_at}`.
2. Existing device A: user types the code;
   `POST /devices/pair {"action":"approve","pairing_code":…,"wrapped_master_key":…, "for_device_id":…}`.
   `wrapped_master_key` is the account master key re-encrypted (XChaCha20-Poly1305)
   to a key derived from the pairing code — the server relays it opaquely.
3. Device B polls `POST /devices/pair {"action":"poll","pairing_code":…}` →
   receives `wrapped_master_key`, unwraps locally.
4. Codes expire after 5 minutes and are single-use.

## Limits (defaults; all server-configurable)

- Max single file: 100 MB · Max batch: 250 MB · Max devices/account: 10
- Version retention: 30 versions per file
