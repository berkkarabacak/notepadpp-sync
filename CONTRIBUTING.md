# Contributing to Notepad++ Sync

Thanks for your interest in contributing! This project is privacy-first
open-source software; contributions of all kinds are welcome.

## Ways to contribute

- **Bug reports** — open an issue with Notepad++ version, plugin version,
  server version, and steps to reproduce. Attach logs from
  `%APPDATA%\Notepad++Sync\logs` (they never contain file contents or keys,
  but skim them anyway before posting).
- **Feature requests** — open an issue describing the workflow you want, not
  just the mechanism.
- **Code** — see below.
- **Documentation** — typos, clarifications, translations of docs.

## Development setup

### Server (Go)

```bash
docker compose -f docker-compose.dev.yml up -d   # PostgreSQL on localhost:5432
cd server
go run ./cmd/server
go test ./...
```

### Plugin (C++, Windows)

```bash
cd plugin
cmake -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug
```

To try the plugin in a live Notepad++, copy `build/Release/NppSync.dll` into
`<Notepad++>\plugins\NppSync\` and restart Notepad++.

## Pull request process

1. Fork and create a branch from `main`.
2. Keep PRs focused: one feature or one fix per PR.
3. Add or update tests for behavior changes. CI must pass
   (plugin build + tests + static analysis, server lint + tests + Docker build).
4. Update documentation (`docs/`, `README.md`, `CHANGELOG.md`) when
   user-visible behavior changes.
5. Follow the existing code style:
   - Go: `gofmt`, `go vet`, `golangci-lint` clean.
   - C++: C++17, clang-format (config in `plugin/.clang-format`), no warnings
     at `/W4`.
6. Sign your commits if you can (not required).

## Security-sensitive changes

Anything touching encryption, key handling, authentication, conflict
resolution, or path validation gets extra review. If your change alters the
wire protocol, bump the protocol version per
[protocol/docs/protocol-v1.md](protocol/docs/protocol-v1.md) and keep
backward compatibility for at least one release.

## Commit messages

Use clear, imperative messages, e.g.:

```
server: reject uploads larger than configured limit
plugin: fix reload loop when remote write races local save
```

## License

By contributing, you agree that your contributions are licensed under the
MIT License.
