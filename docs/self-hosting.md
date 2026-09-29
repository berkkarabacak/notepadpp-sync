# Self-hosting the NPSync server

The whole backend is two containers: the Go API server and PostgreSQL.
No external object storage is required (blobs live in a Docker volume by
default; S3/R2/MinIO is supported if you want it).

## Investor demo (one laptop)

This is the short path. It does not need a domain or TLS.

```bash
git clone https://github.com/berkkarabacak/notepadpp-sync.git
cd notepadpp-sync
cp .env.example .env
```

Edit `.env`:

| Variable | Demo value |
|----------|------------|
| `POSTGRES_PASSWORD` | a long random string |
| `TOKEN_SIGNING_KEY` | `openssl rand -hex 32` (64 hex characters) |
| `BASE_URL` | `http://localhost:8080` |

`BASE_URL` is the URL the server puts in device-pairing payloads and the URL you type into the plugin (*Settings → Advanced → Backend URL*). For this demo they are the same. A fresh plugin install uses `https://sync.berkkarabacak.com`, so this demo replaces that with `http://localhost:8080`, chooses **Save**, and restarts Notepad++. **Save** does not retarget the process that is already running.

```bash
docker compose up -d --build
curl -fsS http://localhost:8080/health
```

Compose sets `NPSYNC_REQUIRE_HTTPS=true`. That only rejects requests that arrive with `X-Forwarded-Proto: http`. A direct `curl` to localhost, and the plugin talking to `http://localhost:8080` with no proxy, are accepted. Put a TLS proxy in front before exposing the port beyond the laptop, and then set `BASE_URL` to the `https://` URL.

Optional Google sign-in: set `GOOGLE_CLIENT_ID` and `GOOGLE_CLIENT_SECRET`. `GOOGLE_REDIRECT_URI` defaults to `${BASE_URL}/auth/google/callback` (`http://localhost:8080/auth/google/callback` for this demo, which Google allows on localhost). Leave both credentials empty and the server keeps email and password only. The console steps are under [Google sign-in](#google-sign-in).

The click-by-click path for the room is [investor-demo-script.md](investor-demo-script.md) (about 8–10 minutes). The 45-minute checklist is [acceptance-test-script.md](acceptance-test-script.md).

A hostname other people can reach is [Public demo URL](#public-demo-url). That is a name you keep, not an ephemeral `*.trycloudflare.com` Quick Tunnel.

## Quick start

```bash
git clone https://github.com/berkkarabacak/notepadpp-sync.git
cd notepadpp-sync
cp .env.example .env
# edit .env: set POSTGRES_PASSWORD, TOKEN_SIGNING_KEY (openssl rand -hex 32), and BASE_URL
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
A fresh install uses `https://sync.berkkarabacak.com`. Replace that when
this server is yours, choose **Save**, and restart Notepad++. **Save**
does not retarget a Notepad++ process that is already running.

## Configuration

All limits and behaviors are environment variables (see `.env.example`):

| Variable | Default | Meaning |
|----------|---------|---------|
| `NPSYNC_LISTEN_ADDR` | `:8080` | bind address |
| `NPSYNC_BASE_URL` | `http://localhost:8080` | public URL (`BASE_URL` in compose) |
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
| `NPSYNC_TRUSTED_PROXIES` | empty | comma-separated IPs or CIDRs allowed to set the client address via `X-Forwarded-For`. Empty ignores the header. |
| `NPSYNC_GOOGLE_CLIENT_ID` | empty | Google OAuth client ID. Empty disables Google sign-in. |
| `NPSYNC_GOOGLE_CLIENT_SECRET` | empty | Google OAuth client secret. Set together with the client ID. |
| `NPSYNC_GOOGLE_REDIRECT_URI` | `{NPSYNC_BASE_URL}/auth/google/callback` | Must match the URI registered in Google Cloud. |

Leave the Google variables empty to keep email/password only. In `.env` / Compose they are `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET`, and `GOOGLE_REDIRECT_URI` (mapped onto the `NPSYNC_` names above). Do not commit real values.

These exist so tests can point at a fake identity provider. Leave them unset in production:

| Variable | Default |
|----------|---------|
| `NPSYNC_GOOGLE_AUTH_URL` | `https://accounts.google.com/o/oauth2/v2/auth` |
| `NPSYNC_GOOGLE_TOKEN_URL` | `https://oauth2.googleapis.com/token` |
| `NPSYNC_GOOGLE_JWKS_URL` | `https://www.googleapis.com/oauth2/v3/certs` |
| `NPSYNC_GOOGLE_ISSUER` | `https://accounts.google.com` |

## Google sign-in

The plugin is a native Windows client, so it does not embed a client secret. The server is the OAuth client: Authorization Code with PKCE (S256). The plugin opens the system browser and polls the server until you finish in the browser. Google identity only signs the account in. Encryption keys stay on the device; a second computer still needs pairing or the recovery key.

Use a **Web application** client, not a Desktop client. Google redirects to your server, not to a random localhost port on each PC.

1. Open [Google Cloud Console → Credentials](https://console.cloud.google.com/apis/credentials) and pick or create a project.
2. Configure the OAuth consent screen (APIs & Services → OAuth consent screen).
   - User type **External**, unless this is a Google Workspace org and only people in that org will sign in (**Internal**).
   - App name: `Notepad++ Sync` (or your own name). Set a support email and a developer contact email.
   - Scopes: `openid`, `email`, and `profile` (the non-sensitive userinfo scopes). You do not need Drive or any other API.
   - While the app is in **Testing**, every Google account that will sign in must be listed under **Test users**. Otherwise Google returns `access_denied`. Publishing the app removes that list. NPSync does not keep Google refresh tokens, so Google's 7-day testing-token limit does not apply to sync sessions.
3. Create credentials → **OAuth client ID** → application type **Web application**.
4. Authorized redirect URI: exactly `https://sync.myserver.com/auth/google/callback`, where `https://sync.myserver.com` is `NPSYNC_BASE_URL` with no trailing slash. If you set `GOOGLE_REDIRECT_URI`, register that exact string instead.
   - Google allows `http://localhost:8080/auth/google/callback` and `http://127.0.0.1:8080/auth/google/callback` for a machine-local server. A public hostname must be `https`. With `NPSYNC_REQUIRE_HTTPS=true` the process refuses to start if the redirect URI is not https (localhost excepted).
5. Copy the client ID and client secret into `.env`:

   ```bash
   GOOGLE_CLIENT_ID=
   GOOGLE_CLIENT_SECRET=
   # optional; default is ${BASE_URL}/auth/google/callback
   GOOGLE_REDIRECT_URI=
   ```

6. `docker compose up -d` (or restart the server). In the plugin, **Sign in with Google** is the primary button. Email and password stay available so existing accounts can sign in and link Google the first time the same verified address is used.

The callback URL carries a one-time authorization code. PKCE binds that code to the verifier stored on the server, so a code copied from a proxy log cannot be redeemed by itself. Prefer not to log query strings on `/auth/google/callback` if your proxy allows it.

If only one of the client ID and secret is set, the server exits at startup rather than offering a half-configured login.

## Migrations

The server applies SQL migrations from `server/migrations/` automatically at
startup and records them in `schema_migrations`. Migrations are plain,
versioned SQL files — no ORM auto-migration is ever used. To add a schema
change, create the next file (`0003_*.sql`) and redeploy.

## Reverse proxy example (Caddy)

```
sync.myserver.com {
    reverse_proxy 127.0.0.1:8080
}
```

WebSockets (`/ws`) work through standard reverse proxies without extra
configuration (they are ordinary HTTP Upgrade requests).

### Client IP for login rate limits

Login rate limits use the TCP peer the server actually accepted. A client
can set `X-Forwarded-For` to anything, so the header is ignored unless that
peer is listed in `NPSYNC_TRUSTED_PROXIES` (an IP or CIDR, comma-separated).
Map it in compose as `TRUSTED_PROXIES`.

When the peer is trusted, the client address is the rightmost forwarded hop
that is not itself a trusted proxy — the address your proxy appended. A
spoofed value the browser stuck on the front of the header does not become
the rate-limit key.

The proxy must **set or append** `X-Forwarded-For` from the connection it
accepted, not blindly forward the client's copy. Caddy's `reverse_proxy`
and nginx `proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;`
do that. For a single proxy, `TRUSTED_PROXIES` is the proxy's address as
the server sees it (often a Docker bridge CIDR such as `172.16.0.0/12`,
not the public IP of the proxy). Leave it empty for the laptop demo above:
there is no proxy, and localhost is the client.

## Public demo URL

The laptop demo uses `BASE_URL=http://localhost:8080`. That URL is only
reachable on that machine. A stable public demo needs a hostname that stays
put, because the plugin Backend URL, device-pairing payloads, and (if you
use Google) the OAuth redirect URI are all that one URL.

You supply the hostname and DNS. You also supply Google OAuth client
credentials if you want Google sign-in. This repo does not register a
domain and does not contain a real client id or secret.
`sync.myserver.com` in the Caddy snippet and in `.env.example` is a
placeholder, not a host this project controls. Below, `<your-hostname>`
means the name you chose. No trailing slash.

Do not use a Cloudflare Quick Tunnel as that hostname. `cloudflared tunnel --url http://localhost:8080` assigns a random `*.trycloudflare.com` name that changes when the process starts again. `BASE_URL`, the plugin, and a redirect URI registered at Google would all be wrong after the next launch.

Two setups that keep a fixed name:

### (a) Your domain and a TLS reverse proxy

Point DNS for `<your-hostname>` at the machine running the proxy, and
terminate TLS there. The [Caddy example](#reverse-proxy-example-caddy)
shows `reverse_proxy` to `127.0.0.1:8080`. nginx or Traefik work the same
way if they forward HTTP Upgrade for `/ws` and set `X-Forwarded-Proto`
from the connection they accepted. Caddy in front of the published port
is not enough.

`docker-compose.yml` publishes `8080:8080` with no host IP, so Docker
listens on every interface. Compose sets `NPSYNC_REQUIRE_HTTPS=true`,
which rejects a request only when `X-Forwarded-Proto` is the explicit
value `http`. A client that opens port 8080 directly does not send that
header, and the API accepts the call. On a public host, do not leave the
API reachable on plaintext `:8080`. Use one of these:

- Bind the publish to loopback only: change `8080:8080` to
  `127.0.0.1:8080:8080`. A Caddy process on the same host can still use
  `127.0.0.1:8080`.
- Put Caddy on the Compose network `npsync` and do not publish the API
  port. Proxy to `http://server:8080` on that network.
- Firewall port 8080 so it is not reachable from the public network.

In `.env`:

```bash
BASE_URL=https://<your-hostname>
# Leave empty to use ${BASE_URL}/auth/google/callback.
GOOGLE_REDIRECT_URI=
# Proxy address as the server sees it. See "Client IP" above.
TRUSTED_PROXIES=
```

`docker compose up -d` after editing `.env`. Caddy's `reverse_proxy` sends
the visitor scheme, including `X-Forwarded-Proto: https` for an HTTPS
visit. Set `TRUSTED_PROXIES` whenever a proxy sits in front; leave it
empty only for the laptop demo.

In the plugin, **Settings → Advanced → Backend URL** is that same
`https://<your-hostname>`. Restart Notepad++ after **Save** — the running
process keeps the URL it loaded at startup.

### (b) A named Cloudflare Tunnel with a fixed hostname

Create a named tunnel in the Cloudflare account that holds the DNS zone
for `<your-hostname>` (`cloudflared tunnel create <name>`, or
**Networking → Tunnels** in the dashboard). Route that hostname to the
tunnel (`cloudflared tunnel route dns <name> <your-hostname>`, or a
published application route). The hostname has to be one the zone already
lets you pick. Quick Tunnel does not give you that.

Locally managed ingress (the last rule is the required catch-all):

```yaml
tunnel: <tunnel-name-or-uuid>
credentials-file: <path-to-the-credentials-json>

ingress:
  - hostname: <your-hostname>
    service: http://localhost:8080
  - service: http_status:404
```

If `cloudflared` runs in another container, `localhost` inside that
container is not the API. Use the compose service name and port
(`http://server:8080`) on a shared network instead. A remotely managed
tunnel sets the same hostname and service URL in the dashboard; do not
also expect a local config file to win. Do not commit the credentials
file or a tunnel token.

Start the connector after the tunnel exists, the DNS route is published,
and the ingress above is saved. On the host:

```bash
cloudflared tunnel run <name>
```

A dashboard tunnel installs the connector with the tunnel token instead.
`docker compose up -d` does not start `cloudflared`. Compose has no tunnel
service (only `db` and `server`), so the connector stays down until you
start it. The tunnel also does not close the port Compose publishes. The
plaintext `:8080` rule in (a) still applies.

Then the same `.env` as (a): `BASE_URL=https://<your-hostname>`, and
`TRUSTED_PROXIES` set to the address of `cloudflared` as the API container
sees it (the Docker bridge peer when `cloudflared` on the host connects to
published port `8080`, not the visitor’s public IP). On a public `https`
hostname, `cloudflared` forwards `X-Forwarded-Proto: https` to that local
HTTP service and appends the visitor to `X-Forwarded-For`. The rate
limiter uses that forwarded address only for a peer listed in
`TRUSTED_PROXIES`.

`/ws` is a normal HTTP Upgrade. Caddy’s `reverse_proxy` passes it, and an
HTTP service on a named tunnel does too.

### Google on that hostname

Only if you want **Sign in with Google**. Create a **Web application**
OAuth client (steps under [Google sign-in](#google-sign-in)) and put the
id and secret in `.env`:

```bash
GOOGLE_CLIENT_ID=
GOOGLE_CLIENT_SECRET=
```

Register this exact authorized redirect URI, which is what the server
uses when `GOOGLE_REDIRECT_URI` is empty:

```text
https://<your-hostname>/auth/google/callback
```

If you set `GOOGLE_REDIRECT_URI`, use that same string so the redirect
stays on the host in `BASE_URL`. With `NPSYNC_REQUIRE_HTTPS=true` the
process refuses to start when the redirect URI is not `https`, except for
`localhost`, `127.0.0.1`, and `::1`. A public Quick Tunnel name would also
have to be re-registered at Google every time it changed — another reason
not to use one as `BASE_URL`.

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
