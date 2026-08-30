-- NPSync server — initial schema (protocol v1).
-- Migrations are applied in lexical order by cmd/server at startup
-- (tracked in schema_migrations). Never edit applied migrations; add new ones.

CREATE TABLE IF NOT EXISTS schema_migrations (
    version     TEXT PRIMARY KEY,
    applied_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS accounts (
    id                     UUID PRIMARY KEY,
    email                  TEXT NOT NULL UNIQUE,
    password_hash          TEXT NOT NULL,          -- argon2id encoded string
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    failed_login_attempts  INT NOT NULL DEFAULT 0,
    locked_until           TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS devices (
    id           UUID PRIMARY KEY,
    account_id   UUID NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name         TEXT NOT NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    revoked_at   TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS devices_account_idx ON devices(account_id);

CREATE TABLE IF NOT EXISTS refresh_tokens (
    id          UUID PRIMARY KEY,
    device_id   UUID NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    token_hash  TEXT NOT NULL UNIQUE,               -- SHA-256 of the opaque token
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at  TIMESTAMPTZ NOT NULL,
    revoked_at  TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS refresh_tokens_device_idx ON refresh_tokens(device_id);

-- Current head state per file. Ciphertext only; the server cannot decrypt.
CREATE TABLE IF NOT EXISTS files (
    account_id         UUID NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    file_id            UUID NOT NULL,
    encrypted_metadata BYTEA NOT NULL,
    content_hash       TEXT NOT NULL,               -- SHA-256 of ciphertext, hex
    version            INT  NOT NULL,
    version_vector     JSONB NOT NULL,
    deleted            BOOLEAN NOT NULL DEFAULT FALSE,
    size               BIGINT NOT NULL,
    blob_key           TEXT NOT NULL,               -- key in blob storage
    modified_at        TIMESTAMPTZ NOT NULL,
    origin_device_id   UUID NOT NULL,
    change_seq         BIGINT NOT NULL,
    PRIMARY KEY (account_id, file_id)
);
CREATE INDEX IF NOT EXISTS files_change_seq_idx ON files(account_id, change_seq);

-- Version history (retention-pruned by application logic).
CREATE TABLE IF NOT EXISTS file_versions (
    account_id         UUID NOT NULL,
    file_id            UUID NOT NULL,
    version            INT  NOT NULL,
    encrypted_metadata BYTEA NOT NULL,
    content_hash       TEXT NOT NULL,
    version_vector     JSONB NOT NULL,
    deleted            BOOLEAN NOT NULL DEFAULT FALSE,
    size               BIGINT NOT NULL,
    blob_key           TEXT NOT NULL,
    modified_at        TIMESTAMPTZ NOT NULL,
    origin_device_id   UUID NOT NULL,
    PRIMARY KEY (account_id, file_id, version),
    FOREIGN KEY (account_id, file_id) REFERENCES files(account_id, file_id) ON DELETE CASCADE
);

-- Global, monotonic change feed for delta sync and WS fan-out.
CREATE TABLE IF NOT EXISTS changes (
    change_seq       BIGSERIAL PRIMARY KEY,
    account_id       UUID NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    file_id          UUID NOT NULL,
    version          INT NOT NULL,
    kind             TEXT NOT NULL,                 -- 'upsert' | 'delete'
    origin_device_id UUID NOT NULL,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS changes_account_idx ON changes(account_id, change_seq);

-- Idempotency for mutating calls: same key -> stored response, no re-apply.
CREATE TABLE IF NOT EXISTS idempotency_keys (
    account_id  UUID NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    key         TEXT NOT NULL,
    response    JSONB NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, key)
);

-- Device pairing codes (single-use, short-lived).
CREATE TABLE IF NOT EXISTS pairing_codes (
    code               TEXT PRIMARY KEY,
    account_id         UUID NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    requesting_device  UUID NOT NULL,
    wrapped_master_key BYTEA,                       -- set when approved; opaque
    approving_device   UUID,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at         TIMESTAMPTZ NOT NULL,
    consumed_at        TIMESTAMPTZ
);

-- Optional encrypted session state (open tabs, cursor, scroll).
CREATE TABLE IF NOT EXISTS sessions (
    account_id       UUID PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    encrypted_state  BYTEA NOT NULL,
    version          INT NOT NULL,
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    origin_device_id UUID NOT NULL
);
