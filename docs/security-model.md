# Security Model

## Guarantees

1. **The server cannot read your notes.** File contents are encrypted on
   your device with AES-256-GCM (authenticated encryption, Windows CNG) —
   ciphertext, integrity tag, and a fresh random nonce per version.
2. **Filenames and paths are encrypted too.** The server sees an opaque
   `file_id`, ciphertext metadata, sizes, and version vectors.
3. **Your password never encrypts files and never reaches the server.**
   It is verified with Argon2id on the server side only.
4. **There is no password reset for encrypted data.** The master key exists
   only on your devices, wrapped by your recovery key or pairing codes.
   Losing all devices *and* the recovery key means permanent data loss —
   this is a deliberate design property, not a bug.

## Key hierarchy

```
master key (32 bytes, generated locally, CSPRNG)
├── wraps to: recovery key  → PBKDF2-HMAC-SHA256 (200k rounds, random salt)
├── wraps to: pairing code  → PBKDF2-HMAC-SHA256 (random salt, single use)
└── encrypts: file contents (AAD "file"), metadata (AAD "metadata")
```

- Master key storage on device: DPAPI-protected (`%APPDATA%\Notepad++Sync\
  secrets\`), bound to the Windows user account.
- Key wrapping: AES-256-GCM with domain-separating AAD, so a content
  ciphertext can never be replayed as a key blob or vice versa.
- Content hashes on the server are SHA-256 of **ciphertext** — the server
  verifies uploads without ever holding plaintext-derived values.

## Authentication

- Email + password; Argon2id (m=64 MiB, t=3, p=2) server-side.
- Access tokens: 15-minute HS256 tokens bound to (account, device).
- Refresh tokens: 256-bit opaque, stored SHA-256-hashed, rotated on every
  use, per-device, revocable (device revocation cascades).
- Login endpoints: per-IP+email rate limiting plus account lockout after
  repeated failures (default 8 → 15 min). Timing is equalized for unknown
  accounts.

## Threat model (selected)

| Threat | Mitigation |
|--------|-----------|
| Curious/compromised server | E2E encryption; server stores ciphertext only |
| Token theft | Short-lived access tokens; revocable refresh tokens; device revocation |
| Brute-force login | Argon2id cost, rate limiting, lockout |
| Replay of API writes | Idempotency keys; AEAD nonces unique per payload |
| Malicious peer device sending crafted paths | Path normalization + containment checks; sync writes can never escape configured roots |
| Tampered downloads | SHA-256 of ciphertext verified before decrypt; AEAD tag verified on decrypt; atomic replace |
| Hostile filenames (traversal, `CON`, trailing dots) | Rejected by `PathUtil` validation |
| Oversized uploads / resource abuse | Server-side size limits, request body caps, WS connection caps |

## Transport

HTTPS required in production (`NPSYNC_REQUIRE_HTTPS=true` rejects
plain-HTTP behind proxies). TLS terminates at your reverse proxy in
self-hosted setups.

## What the server *does* learn

Account email, device names (user-chosen), opaque file IDs, ciphertext
sizes, version numbers/vectors, and timing of changes. If this metadata is
sensitive for you, self-host.

## Logging

Neither side logs decrypted content, keys, passwords, recovery keys, or
tokens. Plugin logs (`%APPDATA%\Notepad++Sync\logs`) contain operational
events only.

## Reporting

See [SECURITY.md](../SECURITY.md).
