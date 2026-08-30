# Security Policy

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Email security reports to: **security@example.com** (replace with the project security contact before publishing).

Include:

- A description of the vulnerability and its impact
- Steps to reproduce or a proof of concept
- Affected versions (plugin and/or server)

You can expect an acknowledgement within 72 hours and a status update at least
every 7 days until resolution. We coordinate disclosure with reporters and
credit them in the release notes unless they prefer anonymity.

## Supported versions

| Version | Supported |
|---------|-----------|
| latest release | ✅ |
| older releases | ❌ (please upgrade) |

Security fixes are applied to the latest release line only.

## Security model summary

- All file contents and sensitive metadata (names, paths) are encrypted on the
  client with XChaCha20-Poly1305 (AES-256-GCM also supported) before leaving
  the device. The server stores ciphertext only.
- Passwords are hashed with Argon2id. The master encryption key, recovery key,
  and plaintext passwords are never transmitted to or stored on the server.
- Access tokens are short-lived; refresh tokens are per-device and revocable.
- Login endpoints are rate-limited with exponential backoff / lockout.
- All mutating API calls accept idempotency keys; downloads are hash-verified
  and applied with atomic renames.
- Paths received from any remote client are normalized and validated so a sync
  payload can never write outside a configured sync root.

See [docs/security-model.md](docs/security-model.md) for the full design,
threat model, and hardening checklist.
