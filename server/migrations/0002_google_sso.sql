-- Google SSO (protocol v1, additive).
-- Google identity authenticates the account only. It is not an encryption
-- key and is never used to wrap or recover the client-side master key.
-- password_hash is NULL for accounts that have no password (Google-only).
-- Re-running this file is safe: the test suite applies every migration on
-- each run, and production applies it once via schema_migrations.

ALTER TABLE accounts ALTER COLUMN password_hash DROP NOT NULL;

ALTER TABLE accounts ADD COLUMN IF NOT EXISTS google_sub TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS accounts_google_sub_idx
    ON accounts (google_sub) WHERE google_sub IS NOT NULL;

-- Browser login sessions for Authorization Code + PKCE.
-- NPSync access/refresh tokens are minted on poll and are not stored here.
CREATE TABLE IF NOT EXISTS oauth_logins (
    state             TEXT PRIMARY KEY,
    code_verifier     TEXT NOT NULL,
    nonce             TEXT NOT NULL,
    device_name       TEXT NOT NULL,
    poll_secret_hash  TEXT NOT NULL,             -- SHA-256 hex of the poll secret
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at        TIMESTAMPTZ NOT NULL,
    status            TEXT NOT NULL DEFAULT 'pending', -- pending|exchanging|ready|error|consumed
    account_id        UUID REFERENCES accounts(id) ON DELETE CASCADE,
    error_code        TEXT,
    error_message     TEXT
);
CREATE INDEX IF NOT EXISTS oauth_logins_expires_idx ON oauth_logins (expires_at);
