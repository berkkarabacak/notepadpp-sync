# Self-hosting the NPSync server

The whole backend is two containers: the Go API server and PostgreSQL.
No external object storage is required (blobs live in a Docker volume by
default; S3/R2/MinIO is supported if you want it).

## Quick start

```bash
git clone https://github.com/berkkarabacak/notepadpp-sync.git
cd notepadpp-sync
cp .env.example .env
# edit .env: set POSTGRES_PASSWORD and TOKEN_SIGNING_KEY (openssl rand -hex 32)
docker compose up -d
```

The API listens on `:8080`. Verify with `curl http://localhost:8080/health`.

> **Note (fixed in v1.1.0):** the server container runs as a non-root user;
> the Dockerfile creates and owns `/data/blobs` before switching users. If
> you deployed an earlier build and see `mkdir /data/blobs/...: permission
> denied`, pull the latest image and recreate the volumes.

Put it behind a TLS-terminating reverse proxy
(Caddy, nginx, Traefik) and set `BASE_URL=https://sync.myserver.com` and
`NPSYNC_REQUIRE_HTTPS=true` for production. For a local test on your own
machine, plain `http://localhost:8080` is fine.

Then in the plugin: **Settings → Advanced → Backend URL** →
`https://sync.myserver.com` (or `http://localhost:8080` for local testing).

## Configuration

All limits and behaviors are environment variables (see `.env.example`):

| Variable | Default | Meaning |
|----------|---------|---------|
| `NPSYNC_LISTEN_ADDR` | `:8080` | bind address |
| `NPSYNC_DATABASE_URL` | — | PostgreSQL DSN |
| `NPSYNC_TOKEN_SIGNING_KEY` | — (required) | 64 hex chars; signs access tokens |
| `NPSYNC_REQUIRE_HTTPS` | `false` | reject non-HTTPS in production |
| `NPSYNC_BLOB_BACKEND` | `fs` | `fs` or `s3` |
| `NPSYNC_BLOB_FS_DIR` | `/data/blobs` | blob directory (fs backend) |
| `NPSYNC_S3_ENDPOINT/BUCKET/REGION/ACCESS_KEY/SECRET_KEY` | — | s3 backend |
| `NPSYNC_MAX_FILE_BYTES` | `104857600` | 100 MB per file |
| `NPSYNC_MAX_BATCH_BYTES` | `262144000` | 250 MB per batch |
| `NPSYNC_MAX_DEVICES` | `10` | devices per account |
| `NPSYNC_VERSION_RETENTION` | `30` | versions kept per file |
| `NPSYNC_REGISTRATION_OPEN` | `true` | close to make the server invite-only |

## Migrations

The server applies SQL migrations from `server/migrations/` automatically at
startup and records them in `schema_migrations`. Migrations are plain,
versioned SQL files — no ORM auto-migration is ever used. To add a schema
change, create the next file (`0002_*.sql`) and redeploy.

## Reverse proxy example (Caddy)

```
sync.myserver.com {
    reverse_proxy 127.0.0.1:8080
}
```

WebSockets (`/ws`) work through standard reverse proxies without extra
configuration (they are ordinary HTTP Upgrade requests).

## Backups

Back up two things:

1. The PostgreSQL volume (`pgdata`) — metadata, versions, accounts.
2. The blob volume (`blobdata`) — the encrypted file contents.

Both are ciphertext-only; a stolen backup is not readable without the
users' keys, which the server never has.

## Health & readiness

- `GET /health` — liveness.
- `GET /ready` — DB reachable, migrations applied.

## Observability

Structured JSON logs go to stdout (`docker compose logs -f server`). The
code keeps metrics hooks intentionally simple so Prometheus instrumentation
can be added later without refactoring.

## Upgrading

```bash
git pull
docker compose build server
docker compose up -d
```

Migrations run automatically. The plugin and server negotiate protocol
versions; an incompatible pair shows a clear "update the plugin or the
server" message instead of failing unpredictably.
