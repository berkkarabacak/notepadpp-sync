# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
