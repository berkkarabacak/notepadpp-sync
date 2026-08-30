# Architecture

## Overview

```
┌─────────────┐      HTTPS + WSS (protocol v1)      ┌─────────────┐
│ Notepad++   │  ─────────────────────────────────► │ Sync server │
│ plugin (A)  │  REST /auth /devices /sync /session │  (Go)       │
│             │ ◄─────────────────────────────────  │             │
│ SQLite      │   WebSocket /ws change events       │ PostgreSQL  │
│ (local      │                                     │ + blob      │
│  state)     │                                     │ storage     │
└─────────────┘                                     └─────────────┘
        ▲ ciphertext only, end-to-end encrypted            ▲
        └────────────── plugin (B) identical ──────────────┘
```

## Components

### Plugin (`plugin/`, C++17)

| Module | Role |
|--------|------|
| `PluginDefinition` / `DllMain` | Notepad++ plugin interface, menu, notifications |
| `SyncEngine` | Orchestrates scanning, upload queue, downloads, conflicts |
| `FolderWatcher` | `ReadDirectoryChangesW` notifications (no polling) |
| `ApiClient` | WinHTTP REST client + token refresh; WS client for `/ws` |
| `LocalDb` | SQLite: files, versions, pending ops, conflicts, change seq |
| `Settings` | JSON settings + DPAPI-protected secrets |
| `core/Crypto` | AES-256-GCM (CNG) AEAD, key wrap, recovery keys |
| `core/Merge` | Line-based three-way merge |
| `core/IgnoreRules` | `.gitignore`-style matching |
| `core/PathUtil` | Path normalization + traversal defense |
| `core/VersionVector` | Explicit version tracking (never timestamps) |

Threads: worker (pending ops), poller (`/sync/changes` fallback), scanner
(safety-net rescan), plus watcher threads and the WS thread. All network and
disk work happens off the Notepad++ UI thread.

### Server (`server/`, Go)

| Package | Role |
|---------|------|
| `cmd/server` | Entry point, embedded migrations, graceful shutdown |
| `internal/config` | Env-based config, all limits tunable |
| `internal/api` | REST + WS handlers, auth middleware, rate limiting |
| `internal/auth` | Argon2id, access tokens (HS256), opaque refresh tokens |
| `internal/store` | Store interface; Postgres impl + in-memory impl for tests |
| `internal/blob` | Blob storage interface; filesystem + S3-compatible impls |
| `internal/ws` | WebSocket hub, per-account fan-out |

## Data flow

**Upload (save on A):**
save → watcher/NPPN_FILESAVED → read file → sha256 → AES-GCM encrypt →
`POST/PUT /sync/files` (idempotency key) → server verifies hash, checks
`base_version`, stores blob + version row + change-feed row → WS event.

**Download (to B):**
WS event (or `/sync/changes?since=` catch-up) → `GET /sync/files/{id}` →
verify ciphertext hash → decrypt locally → conflict check against local
state → atomic write (temp → verify → rename) → shadow copy for future
merges → reload buffer if open.

**Conflict:**
`PUT` with stale `base_version` → 409 + server's current record → plugin
fetches both versions → three-way merge (base from local shadow copy) →
clean: upload merged result. Conflict: preserve local copy, record in
conflict table, UI resolution (Keep Local/Remote/Both).

## Versioning

- Each file: monotone `version` (server-assigned), `base_version`
  (client-declared), and a `version_vector` (device→counter) merged by the
  server. Divergence detection never relies on clocks.
- Global `change_seq` per account powers delta sync and reconnect catch-up.

## Storage

- PostgreSQL: accounts, devices, tokens, file heads, version history,
  change feed, idempotency keys, pairing codes, sessions.
- Blobs: content-addressed by ciphertext SHA-256 (`<ab>/<cd>/<hash>`),
  atomic temp+rename writes; pluggable backend (fs now, S3/R2/MinIO via the
  same interface).

## Reliability choices

- Idempotency keys on mutating calls → safe retries, no duplicate uploads.
- Content-addressed blobs → re-upload of identical ciphertext is a no-op.
- Atomic local writes + hash verification before replacement.
- Local backup history of replaced versions (last 5 per file).
- Offline queue survives restarts (SQLite `pending_ops`).
- WS drop → periodic polling keeps syncing regardless.
