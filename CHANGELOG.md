# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Google SSO (Authorization Code + PKCE) as the primary sign-in path.
  The server holds the OAuth client secret; the plugin opens the system
  browser and polls. Google identity only authenticates the account.
  Email/password remains for existing accounts and for servers that have
  not configured Google. A verified Google email links to an existing
  password account without removing the password.

### Fixed
- A second computer signed in with the same Google account can receive the
  existing master key. **Get my notes** shows a code and polls until the
  computer that already has the notes chooses **Allow another computer**.
  The first-run wizard does not mint a new key when that wrapped key is
  already installed, or while it is still waiting. The recovery key remains
  a DPAPI secret on the computer that created it and does not unlock the
  second computer. Fresh installs use `https://sync.berkkarabacak.com`.
- Batch upload stores the ciphertext blob before committing file metadata,
  matching single-file upload. A failed blob write no longer leaves a head
  record other clients can see.
- Empty ciphertext with a bad authentication tag no longer decrypts as
  success. Empty plaintext still round-trips.
- Login rate limits ignore `X-Forwarded-For` unless the direct peer is in
  `NPSYNC_TRUSTED_PROXIES`.

### Changed
- The plugin's default Backend URL is `https://sync.berkkarabacak.com`.
  A fresh install does not use `https://sync.example.com`. A saved
  placeholder (or a blank URL) is treated as unset on the next start.
  Any other saved URL, including `http://localhost:8080`, is kept.
  Changing Backend URL still requires a Notepad++ restart: **Save**
  writes the file, and the running process keeps the URL it loaded at
  startup.
- Docs describe the cipher that ships: AES-256-GCM. The recovery key is
  documented as about 100 bits (20 symbols from a 32-character alphabet).
- Security reports go through GitHub private vulnerability reporting.
  `security@example.com` is removed; no replacement address is invented.
- The release workflow builds the server with Go 1.25, matching `go.mod`.

## [1.1.0] - 2026-09-02

### Added
- Full native settings dialog with tabs: General / Files / Session /
  Security / Advanced.
- Device management UI: list, rename, revoke (with confirmation), pairing
  code request/approval.
- Synced Files/Folders manager: add folders/files, remove roots, edit
  ignore patterns.
- Conflicts window with per-item Keep Local / Keep Remote / Keep Both /
  Open Comparison actions.
- Live Sync Status dialog with refresh.
- Printable acceptance test script (`docs/acceptance-test-script.md`).

### Fixed
- Include-order safety on Windows (locked via `SortIncludes: Never`).
- PowerShell packaging script encoding; release ref-name expansion.

## [1.0.0] - 2026-08-30

### Added
- Initial release: native Notepad++ plugin (C++), Go sync backend,
  versioned protocol (v1), Docker self-hosting, CI/release workflows.
- End-to-end encryption with AES-256-GCM, local key generation,
  device pairing codes, and offline recovery keys.
- Explicit version-vector conflict detection with automatic three-way text
  merge and a conflict resolution UI (Keep Local / Keep Remote / Keep Both).
- Offline change queue persisted in SQLite, resumable across restarts.
- Realtime sync over WebSocket with periodic-sync fallback.
- `.gitignore`-style ignore rules plus per-root `.npsyncignore` files.
- Version history with configurable retention (default: 30 versions/file).
- Optional session sync (open tabs, selected tab, cursor/scroll position).
- Self-hosted mode via `docker compose up -d` (PostgreSQL + local blob
  storage; S3-compatible storage supported by configuration).

[1.0.0]: https://github.com/berkkarabacak/notepadpp-sync/releases/tag/v1.0.0
