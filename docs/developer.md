# Developer Guide

## Prerequisites

| Component | Requirements |
|-----------|--------------|
| Server | Go 1.22+, Docker (for local PostgreSQL) |
| Plugin | Windows, Visual Studio 2022 (MSVC), CMake ≥ 3.20 |

## Local development

```bash
# 1. Start dependencies (PostgreSQL on :5432, optional MinIO with --profile s3)
docker compose -f docker-compose.dev.yml up -d

# 2. Run the server (applies migrations automatically)
cd server
go run ./cmd/server
# -> listening on :8080, NPSYNC_REQUIRE_HTTPS defaults to false in dev

# 3. Build the plugin (in a VS developer prompt)
cd plugin
cmake -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure

# 4. Try it in Notepad++: copy the DLL
copy build\Debug\NppSync.dll "C:\Program Files\Notepad++\plugins\NppSync\"
```

Point the plugin at `http://localhost:8080` via Settings → Advanced →
Backend URL, register an account through the first-run wizard, and sync.

## Server layout

```
server/
  cmd/server/main.go     entry point, migrations, graceful shutdown
  internal/config/       env configuration & limits
  internal/auth/         argon2id, tokens
  internal/store/        Store interface + Postgres + in-memory impl
  internal/blob/         blob storage: fs + s3 (SigV4, stdlib only)
  internal/api/          REST/WS handlers, middleware, rate limiting
  internal/ws/           websocket hub
  migrations/            versioned SQL (embedded in the binary)
```

Commands: `go build ./...`, `go test ./... -race`, `go vet ./...`.
Postgres-backed tests need `NPSYNC_TEST_DATABASE_URL` (CI sets it).

## Plugin layout

```
plugin/src/
  DllMain.cpp, PluginDefinition.*   Notepad++ glue, menu, notifications
  SyncEngine.*                      orchestration (background threads)
  ApiClient.*                       WinHTTP REST + WebSocket client
  LocalDb.*                         SQLite local state
  FolderWatcher.*                   ReadDirectoryChangesW
  Dialogs.*                         native Win32 dialogs
  Settings.*, Logger.*
  core/                             portable, unit-tested logic
    Crypto.*   AES-256-GCM (CNG), key wrap, recovery keys
    Merge.*    three-way line merge
    IgnoreRules.*, PathUtil.*, VersionVector.*
plugin/tests/core_tests.cpp         plain-assert test suite (ctest)
```

Core modules are deliberately free of Notepad++/Win32 UI dependencies so
they stay testable. Add new logic to `core/` when possible, and add tests.

## Protocol work

The wire protocol lives in `protocol/` (docs + JSON Schemas) and is
versioned via the `X-NPSync-Protocol` header. Rules:

1. Additive changes (new optional fields, new endpoints) keep v1.
2. Breaking changes bump to v2: new schema directory, server accepts both
   for at least one release, plugin shows a clear upgrade message on
   mismatch (HTTP 426).

## Testing philosophy

- Server: unit tests (auth, store semantics) + HTTP integration tests that
  simulate two devices end-to-end against the real router, including the
  conflict scenario from the spec (A and B edit the same base; conflict is
  detected; nothing is lost; merge resolves).
- Plugin: core tests under ctest. Full Notepad++ interaction is verified
  manually per release checklist below.

## Release checklist (manual)

1. Build plugin Release x64; run ctest.
2. Run the server locally (Docker compose).
3. Two Notepad++ instances (e.g. two Windows user accounts or VMs), one
   account, two devices.
4. Edit the same synced file → verify propagation both ways.
5. Disable network on one side → edit → re-enable → verify catch-up.
6. Simultaneous conflicting edits → verify conflict UI → verify neither
   version is lost (including the conflict-copy file).
7. Inspect the server DB: confirm only ciphertext (`\x` binary blobs) and
   hashes — never plaintext notes.
8. Tag `vX.Y.Z` → the release workflow builds, tests, packages, and
   publishes.

## Code style

- Go: `gofmt`, `go vet`, golangci-lint clean.
- C++: C++17, clang-format (`plugin/.clang-format`), `/W4` clean.
