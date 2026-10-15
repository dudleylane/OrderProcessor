# Docker Deployment Guide

This directory contains everything needed to run the full OrderProcessor OMS stack: PostgreSQL database, C++ WebSocket server, and React frontend.

## Prerequisites

- Docker or Podman with `docker compose` (or `podman-compose`)
- About 3 GB of disk to build the stack: the base images take about 780 MB, the server's build stage about 1.5 GB (the build cache keeps it), and the server's image about 210 MB
- Port 3000 available: the stack publishes only the frontend's (#146)
- openssl, for the Quick Start's access token and TLS certificate

## Quick Start

```bash
cd docker
cp .env.example .env          # Set PostgreSQL credentials
sed -i "s/^OMS_WS_TOKEN=.*/OMS_WS_TOKEN=$(openssl rand -hex 32)/" .env   # The access token
mkdir -p certs && openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" -keyout certs/tls.key -out certs/tls.crt \
    && chmod 0640 certs/tls.key   # A self-signed TLS certificate (see TLS)
docker compose up --build -d   # Build and start all services
```

Under Docker running as root, also run `sudo chgrp 0 certs/tls.key` before `up`: see [TLS](#tls).

Open **https://localhost:3000** to access the OMS frontend. The browser warns about the certificate, since the Quick Start's is self-signed: accepting it for the page covers the app's WebSocket too. The app asks for the access token first, the `OMS_WS_TOKEN` in `.env` (`grep OMS_WS_TOKEN .env` shows it), and keeps it until the tab closes (see [Access](#access)). The first `up` seeds the new data volume with test instruments, accounts and the clearing firm, so the stack takes orders at once (see [Seed Test Data](#seed-test-data)).

The first build takes several minutes (C++ compilation inside the container). Subsequent starts use the Docker layer cache and are fast.

## Services

| Service | Host port | Image | Description |
|---------|-----------|-------|-------------|
| `oms-db` | none (5432 in the stack) | `postgres:16-alpine` | PostgreSQL database with OMS schema |
| `oms-server` | none (8080 in the stack) | `Dockerfile.oms-server` | C++ OrderProcessor WebSocket server |
| `oms-frontend` | 3000 | `Dockerfile.oms-frontend` | React UI served by nginx, over TLS (#226) |

### Startup Order

1. **oms-db** starts first, initializes schema from `init-db/001_schema.sql`
2. **oms-server** waits for `oms-db` to be healthy (`pg_isready` check), seeds the data volume if it has no instrument yet, then starts the C++ engine on port 8080
3. **oms-frontend** waits for `oms-server` to be healthy (it answers HTTP on 8080, which it does only after restoring its data), then serves the React app over TLS on port 3000

All three restart unless stopped (`restart: unless-stopped`), and each has a health check, a memory limit and a process limit in `docker-compose.yml`. The server's and the frontend's health checks are also in their images; compose repeats them because podman builds OCI images by default, and those don't keep a `HEALTHCHECK`. The server's 2 GB is a starting point: raise it for a large book or a large LMDB map. The server answers only after restoring its data directory, and its health check allows two minutes for that: raise `start_period` for a larger directory, or Docker Compose will find the server unhealthy and won't start the frontend.

On a host with SELinux enforcing, as el10 is by default, compose relabels `init-db/` so the database container can read it (the `z` on its mount, #201). Without that, Postgres can't read its init scripts and never starts.

CI brings this stack up with `scripts/check-compose-stack.sh` (#140), when `docker/` changes and weekly. The script checks that compose refuses to start without an access token and publishes only the frontend's port, that every service becomes healthy, that Postgres loaded the schema, that the server seeded its volume, that `/ws` refuses an upgrade without the token or from another Origin, that a new order sent to the frontend's `/ws` with the token gets its execution report, and that the order and the seeded reference data reach Postgres (#151). It reaches nginx over `https` and `wss` with a certificate made for the run, verifying it (#226). Run it before changing `docker-compose.yml`, `nginx.conf`, `.env.example` or `init-db/`. It takes about three minutes, and it publishes no host port, so it can run beside a stack that is already up.

The server runs as the user `oms`, which owns `/data`, and nginx as the user `nginx`; neither container runs as root. nginx looks the server up through the container's own DNS resolver when a request needs it, so it starts while the server is still down, and it finds a re-created server container at its new address.

### Networking

```
Browser :3000 (https, wss) ──→ nginx (oms-frontend), TLS
                                 ├── /        → serves React SPA static files
                                 └── /ws      → proxy_pass to oms-server:8080 (WebSocket upgrade, plain)
```

The nginx config (`nginx.conf`) terminates TLS and handles WebSocket upgrade headers, so the frontend connects to `wss://localhost:3000/ws`, which is proxied internally to the C++ server.

Only the frontend's port is published (#146). The server and Postgres listen only on the stack's network, so a browser reaches the server through nginx's `/ws`, and `docker compose exec` reaches Postgres. To reach either from the host, publish its port on the loopback address in an override file, such as `127.0.0.1:8080:8080`.

## Configuration

### Environment Variables

Copy `.env.example` to `.env` and set values:

```bash
POSTGRES_USER=oms              # PostgreSQL username
POSTGRES_PASSWORD=changeme     # PostgreSQL password (change this! The server's connection string quotes it, so no ', \ or $)
POSTGRES_DB=oms                # Database name
OMS_WS_TOKEN=                  # WebSocket access token, required: openssl rand -hex 32 makes one (#146)
OMS_ALLOWED_ORIGINS=           # Pages a browser may connect from, comma-separated; empty allows any (#146)
OMS_HSTS=                      # Strict-Transport-Security, such as max-age=31536000, once certs/ holds a certificate browsers trust (#226)
```

### Access

The server refuses a WebSocket client that doesn't offer the access token, `OMS_WS_TOKEN` in `.env`, and compose won't start without one (#146). A browser can't set a WebSocket's headers, so a client offers the token as a subprotocol, `oms-token.<token>`, beside the protocol's own, `oms-v1`; the server names only `oms-v1` in its answer. The app asks for the token when a tab opens it, and **Change token** in its header asks again. A token has at least <!-- DOC_CHECK:ws_min_token_length -->16<!-- /DOC_CHECK --> letters, digits, `.`, `_` or `-`.

`OMS_ALLOWED_ORIGINS`, comma-separated, such as `https://oms.example.com:3000`, limits the pages a browser may connect from: a page from another Origin is refused, even with the token. Empty, as in `.env.example`, allows a page from anywhere that has the token. A client that sends no Origin isn't a browser, and the token alone decides.

The server answers an upgrade without the right token with 401, one from an Origin off the list with 403, and a request that isn't a WebSocket upgrade with 426, then closes the connection.

Everyone with the shared token sees every account and order, and can trade on any account. For less, give each client an identity instead.

#### Identities

With `--ws-credentials` (#225), each client has a token of its own, and its session sees and trades only its identity's accounts: their orders, in the snapshot and in every report after, and its new orders on them alone. Book depth, which shows no account, still goes to every subscriber. README's [Configuration](../README.md#configuration) gives the file's format. To use it with this stack:

1. Make a token per identity with `openssl rand -hex 32`, and its hash with the server's `--hash-ws-token`: `printf '%s' "$TOKEN" | orderProcessorServer --hash-ws-token`, from any build of the server.
2. Write `docker/ws-credentials`, a line per identity: its name, its token's hash, and its accounts. The file holds only hashes, so `chmod 0644` is fine, and the server's user in the image (uid 10001) can read it.
3. In `docker-compose.yml`, give `oms-server` the file and the flag, and take `OMS_WS_TOKEN` out of its environment: the server refuses the two together.
   ```yaml
   command: ["--port", "8080", "--data-dir", "/data", "--ws-credentials", "/etc/orderprocessor/ws-credentials"]
   volumes:
     - ./ws-credentials:/etc/orderprocessor/ws-credentials:ro,z
   ```

Each client then gives the app its own token where it gave the shared one.

### TLS

nginx serves the app and `/ws` over TLS only, on port 3000 (#226). It redirects a plain HTTP request to the same URL over `https`, and the app opens `wss:` because its page came over `https:`. So the access token, the orders and the accounts cross the network encrypted. nginx passes `/ws` on to the server unencrypted, over the stack's own network.

nginx reads its certificate from `certs/tls.crt` and its key from `certs/tls.key`, which compose mounts read-only; the Quick Start makes a self-signed pair. Without them, or without a key it can read, the frontend's container stops at startup, and `docker compose logs oms-frontend` says why. nginx runs as the user `nginx`, which is in group 0, so the key must be readable by group 0 in the container:
- **Rootless podman or rootless Docker:** your own files belong to user and group 0 in the container, so `chmod 0640 certs/tls.key` is enough.
- **Docker running as root:** also run `sudo chgrp 0 certs/tls.key`.

openssl writes a key that only its owner may read (0600), and nginx can't read that in either case.

A certificate must name the host the browser uses. The Quick Start's names `localhost` and `127.0.0.1`. To reach the stack from another machine, add that machine's name to `subjectAltName`, as `DNS:oms.example.com`.

A browser doesn't trust a self-signed certificate. It warns about the page, and accepting the certificate there covers the app's WebSocket too, which uses the same host and port. A browser that hasn't accepted it refuses the WebSocket without asking, and the app says "Not connected".

To use a real certificate:
1. Put it in `certs/tls.crt`, followed by its intermediate certificates, and its key in `certs/tls.key`, readable by group 0 as above.
2. Run `docker compose restart oms-frontend`.
3. Set `OMS_HSTS=max-age=31536000` in `.env` and run `docker compose up -d`, so that browsers keep to `https` for a year.

Don't set `OMS_HSTS` before that: a browser that has seen it refuses the site over a certificate it doesn't trust, such as the self-signed one, with no way to accept it.

### Server Options

The C++ server accepts command-line arguments via the `CMD` directive in `Dockerfile.oms-server`:

```dockerfile
ENTRYPOINT ["orderProcessorServer"]
CMD ["--port", "8080", "--data-dir", "/data"]
```

Override in `docker-compose.yml`:

```yaml
oms-server:
  command: ["--port", "8080", "--data-dir", "/data", "--workers", "4"]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 8080 | WebSocket listen port |
| `--bind` | `0.0.0.0` | Address to listen on: an IP address, such as `127.0.0.1` for this host only (#146) |
| `--ws-token-file` | none | A file whose first line is the access token. `OMS_WS_TOKEN` gives it too, but not both; with neither, any client may connect, and the server warns (#146) |
| `--ws-credentials` | none | Identities instead of the shared token, each with its token's hash and its accounts (#225); see [Identities](#identities) |
| `--allowed-origin` | any | An Origin a browser page may connect from; repeat it for more. `OMS_ALLOWED_ORIGINS` adds a comma-separated list (#146) |
| `--data-dir` | `./data` | LMDB persistence directory. The image's command gives `/data`, its volume |
| `--workers` | 0 | TBB worker threads, and as many event and transaction processor pairs. 0 leaves TBB at its default, a thread per core, and runs 2 pairs |
| `--cpu-affinity` | -1 | Pin main thread starting from this core (-1 = disabled) |
| `--huge-pages` | off | Reports whether huge pages are available; nothing allocates from them yet |
| `--clearing-firm` | `HOUSE-CLEARING` | Clearing firm put on every order; must exist in the data directory |
| `--default-account` | `TRADING-1` | Account for an order that names none; must exist in the data directory |
| `--debug` | off | Debug logging: about six lines per transaction, written inside the transaction manager's lock |

## Common Operations

### Start / Stop

```bash
docker compose up -d             # Start (detached)
docker compose up --build -d     # Rebuild and start
docker compose down              # Stop all services
docker compose down -v           # Stop and delete all data volumes
docker compose restart oms-server  # Restart just the server
```

### Logs

The server logs to standard output in its image (`EXCHANGE_LOG=-`), so its log is what `docker compose logs` shows. Compose keeps at most three 10 MB files of it. Run outside a container, the server writes `exchange.log` in its working directory instead.

```bash
docker compose logs -f               # Follow all service logs
docker compose logs -f oms-server    # Server logs only
docker compose logs -f oms-db        # Database logs only
docker compose logs --tail=100       # Last 100 lines from all services
```

### Status

```bash
docker compose ps                    # Service status
docker compose top                   # Running processes per service
```

### Database Access

Postgres publishes no host port (#146); `docker compose exec` reaches it from the host.

```bash
# Connect to PostgreSQL via psql
docker compose exec oms-db psql -U oms -d oms

# Common queries
SELECT count(*) FROM orders;
SELECT * FROM orders WHERE status = 'NEW' ORDER BY created_at DESC LIMIT 10;
SELECT * FROM executions ORDER BY exec_time DESC LIMIT 10;
\dt                                  -- List all tables
\dT+                                 -- List all enum types
```

### Seed Test Data

A new `oms-data` volume is seeded on the first `up`: the server's container runs `seedData --if-empty` before the server, every time it starts (#161). That seeds a volume with no instrument yet, including one a server ran on before it was seeded, and leaves a volume with instruments alone, since what `seedData` adds is test data.

The `seedData` utility is included in the server image. To seed a volume that has instruments, run it by hand. The server reads reference data only when it starts, so seed with it stopped:

```bash
docker compose stop oms-server
docker compose run --rm --no-deps --entrypoint seedData oms-server --data-dir /data
docker compose start oms-server
```

`seedData` seeds instruments, accounts and the server's clearing firm, and skips any that already exist, so running it again is safe. On a fresh volume, you can also run the `run` line before the first `docker compose up`.

### Shell Access

```bash
docker compose exec oms-server /bin/bash       # Server container shell
docker compose exec oms-db /bin/sh             # Database container shell
docker compose exec oms-frontend /bin/sh       # Frontend container shell
```

## Volumes

| Volume | Mounted at | Purpose |
|--------|-----------|---------|
| `oms-pgdata` | `/var/lib/postgresql/data` (oms-db) | PostgreSQL data files |
| `oms-data` | `/data` (oms-server) | LMDB persistence files |

Data persists across container restarts. To start fresh:

```bash
docker compose down -v       # Removes all volumes
docker compose up --build -d  # Rebuild and reinitialize
```

### A volume written by an older image

Server images before #158 ran as root, so the files they wrote in `oms-data` belong to root. The server now runs as `oms` and can't open them: it exits with `LMDBStorage::load mdb_env_open: Permission denied`, and restarts again and again. Give the volume to `oms` once, and the server comes up at its next restart:

```bash
docker compose run --rm --no-deps --user 0 --entrypoint chown oms-server -R oms:oms /data
```

## Build Details

### oms-server (Multi-stage)

| Stage | Base Image | Purpose |
|-------|-----------|---------|
| Builder | `centos:stream10` | Installs GCC 15 (gcc-toolset-15), CMake, TBB, spdlog, Boost, LMDB, numactl; compiles Release build |
| Runtime | `centos:stream10` | Minimal image with only runtime libraries; copies `orderProcessorServer` and `seedData` |

The build compiles with `-DCMAKE_BUILD_TYPE=Release` (LTO, -O3) and disables tests and benchmarks to minimize build time. It targets x86-64-v3 (`-DMARCH=x86-64-v3`), not the build machine's CPU, so the image runs on any CPU at that level, which has AVX2, BMI1/2, FMA and MOVBE: most Intel Core CPUs since Haswell (2013) and AMD CPUs since Excavator (2015), though not every Pentium or Celeron. CentOS Stream 10, its base, is built for the same level, so it can't run on less anyway. Builds outside the image default to `-march=native` (#170).

CI builds and runs this image for every pull request (`.github/workflows/docker.yml`), with `scripts/check-server-image.sh`, and on each push to `master` or `feature/fx-swap-support` whose tree the merged pull request's run hasn't already passed (#314). The script seeds a volume and starts the server. It then checks that the server listens on every port the image exposes, that it answers a WebSocket upgrade on `/ws`, and that SIGTERM stops it cleanly. Run the script before changing this Dockerfile, the build or a dependency; it takes a few minutes.

### oms-frontend (Multi-stage)

| Stage | Base Image | Purpose |
|-------|-----------|---------|
| Builder | `node:24-slim` | Runs `npm ci` and `npm run build` (TypeScript + Vite) |
| Runtime | `nginx:alpine` | Serves the built SPA over TLS; uses custom `nginx.conf` for TLS and WebSocket proxying |

The frontend connects to `/ws` on whatever host and port served the page, and nginx passes that on to the server. So it works from another machine and behind a remapped port, such as `"3001:3000"`.

CI builds and runs this image as well, with `scripts/check-frontend-image.sh` (#139). The build runs `npm ci` and `npm run build`, so a TypeScript error fails it: nothing else in CI compiles the frontend. The script then checks that the image refuses to start without a certificate, saying why. With a throwaway certificate (#226), it checks that nginx runs as `nginx`, starts before anything called `oms-server` exists, serves the app and its script over `https`, redirects plain HTTP to `https`, sends `Strict-Transport-Security` only when `OMS_HSTS` sets it, passes its health check, and passes `/ws` with its WebSocket headers to a stand-in for the server. Run it before changing the frontend, this Dockerfile or `nginx.conf`; it takes under a minute.

## Database Schema

The schema is initialized automatically on first boot from `init-db/001_schema.sql`. The server mirrors its reference data and orders into it, behind LMDB (#151): README's [PostgreSQL Integration](../README.md#postgresql-integration) says how. It includes:

**Enum types** (matching C++ and TypeScript): `side`, `order_type`, `order_status`, `exec_type`, `time_in_force`, `currency`, `capacity`, `settl_type`, `account_type`, `audit_action`

**Tables:**

| Table | Description |
|-------|-------------|
| `users` | User accounts (UUID primary keys); nothing authenticates against them |
| `instruments` | Reference data — symbol, security ID |
| `accounts` | Reference data — account name, firm, type |
| `clearing_firms` | Reference data — clearing firm names |
| `orders` | Order archive with full order parameters |
| `executions` | Execution reports (single-table inheritance via `exec_type`) |
| `audit_log` | Audit trail with JSONB detail; nothing enforces append-only |

**Indexes** optimized for frontend blotter queries: active orders (partial index), executions by order and time, audit log with GIN index on JSONB.

### An `oms-pgdata` volume from before #151

Postgres runs `init-db/` only on a new volume, so an older one keeps the schema it was made with, whose quantity columns are INTEGER and whose ClOrdID columns hold 64 characters. The server's writes then fail for an order above 2147483647 or with a longer ClOrdID, and it logs each. Nothing wrote to Postgres before #151, so starting over loses nothing: `docker compose down -v` deletes every volume, the server's data too. To keep the server's data, change just the columns:

```bash
docker compose exec oms-db psql -U oms -d oms -c "
  ALTER TABLE orders ALTER COLUMN order_qty TYPE BIGINT, ALTER COLUMN min_qty TYPE BIGINT,
    ALTER COLUMN leaves_qty TYPE BIGINT, ALTER COLUMN cum_qty TYPE BIGINT,
    ALTER COLUMN day_order_qty TYPE BIGINT, ALTER COLUMN day_cum_qty TYPE BIGINT,
    ALTER COLUMN cl_order_id TYPE TEXT, ALTER COLUMN orig_cl_order_id TYPE TEXT,
    ALTER COLUMN source TYPE TEXT, ALTER COLUMN destination TYPE TEXT;
  ALTER TABLE executions ALTER COLUMN last_qty TYPE BIGINT, ALTER COLUMN new_qty TYPE BIGINT;"
```

## Troubleshooting

### Port conflicts

If a port is already in use:

```bash
# Check what's using the port
ss -tlnp | grep 3000

# Override ports in docker-compose.yml
ports:
  - "3001:3000"   # Map to a different host port
```

### Build failures

```bash
# Rebuild from scratch (no cache)
docker compose build --no-cache

# Build a single service
docker compose build oms-server
```

### Server won't start

A server that can't start logs why, as a line with `[Fatal]` and `Can't start:`, and exits with status 1. It restarts and fails again until the cause is fixed.

```bash
# Check server logs
docker compose logs oms-server

# Common issues:
# - "mdb_env_open: Permission denied" → a volume an older image wrote as root (see "A volume written by an older image")
# - Other LMDB errors → corrupt data (docker compose down -v starts again, and deletes it)
```

### Frontend won't start

`docker compose logs oms-frontend` saying "can't read /etc/nginx/certs/tls.crt" (or `tls.key`) means `certs/` has no certificate or key, or a key nginx may not read: make them as the [Quick Start](#quick-start) does, and see [TLS](#tls) for the key's permissions (#226).

### Frontend shows "Disconnected" or "Not connected"

"Not connected: the server is down, or refused the access token" means no connection has opened since the token was entered: a browser doesn't tell the app why a WebSocket failed (#146).

1. Check that the browser accepted the certificate: one that hasn't refuses the WebSocket without asking. Open `https://localhost:3000` itself and accept it, then reload (see [TLS](#tls))
2. Check the token: `grep OMS_WS_TOKEN .env`, then **Change token** in the app's header
3. Check that `oms-server` is running: `docker compose ps`
4. Check server logs: `docker compose logs oms-server`
5. Verify nginx proxy: `curl -ik https://localhost:3000/ws` should get the server's `426 Upgrade Required` (not 404 or 502)
6. Check browser console for WebSocket connection errors: a 401 is the token, a 403 the page's Origin (`OMS_ALLOWED_ORIGINS`)

### Reset everything

```bash
docker compose down -v           # Stop and delete all data
docker system prune -f           # Clean up dangling images
docker compose up --build -d     # Fresh start
```
