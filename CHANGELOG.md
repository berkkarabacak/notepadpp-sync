# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial project structure: native Notepad++ plugin (C++), Go sync backend,
  versioned protocol (v1), Docker self-hosting, CI/release workflows.
- End-to-end encryption with XChaCha20-Poly1305, local key generation,
  device pairing codes, and offline recovery keys.
- Explicit version-vector conflict detection with automatic three-way text
  merge and a conflict resolution UI (Keep Local / Keep Remote / Keep Both /
  Compare / Manual Merge).
- Offline change queue persisted in SQLite, resumable across restarts.
- Realtime sync over WebSocket with periodic-sync fallback.
- `.gitignore`-style ignore rules plus per-root `.npsyncignore` files.
- Version history with configurable retention (default: 30 versions/file).
- Optional session sync (open tabs, selected tab, cursor/scroll position).
- Self-hosted mode via `docker compose up -d` (PostgreSQL + local blob
  storage; S3-compatible storage supported by configuration).

[Unreleased]: https://github.com/your-org/notepadpp-sync/compare/v0.0.0...HEAD
