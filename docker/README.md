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
cp .env.example .env          # Its required settings are empty: compose refuses it until each is set
sed -i "s/^OMS_WS_TOKEN=.*/OMS_WS_TOKEN=$(openssl rand -hex 32)/" .env   # The access token
sed -i "s/^POSTGRES_PASSWORD=.*/POSTGRES_PASSWORD=$(openssl rand -hex 32)/" .env   # Postgres's superuser (#568)
sed -i "s/^OMS_DB_SERVER_PASSWORD=.*/OMS_DB_SERVER_PASSWORD=$(openssl rand -hex 32)/" .env   # The server's role (#568)
sed -i "s/^OMS_PUBLIC_HOST=.*/OMS_PUBLIC_HOST=localhost:3000/" .env   # Where browsers reach the app (#564)
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
2. **oms-server** waits for `oms-db` to be healthy (`pg_isready` over TCP, which the server Postgres runs while it initializes a new volume doesn't answer: #763), seeds the data volume if it has no instrument yet, then starts the C++ engine on port 8080
3. **oms-frontend** waits for `oms-server` to be healthy (it answers HTTP on 8080, which it does only after restoring its data), then serves the React app over TLS on port 3000

All three restart unless stopped (`restart: unless-stopped`), and each has a health check, a memory limit and a process limit in `docker-compose.yml`. The server's and the frontend's health checks are also in their images; compose repeats them because podman builds OCI images by default, and those don't keep a `HEALTHCHECK`. The server's 2 GB is a starting point: raise it for a large book or a large LMDB map. The server answers only after restoring its data directory, and its health check allows two minutes for that: raise `start_period` for a larger directory, or Docker Compose will find the server unhealthy and won't start the frontend.

On a host with SELinux enforcing, as el10 is by default, compose relabels `init-db/` so the database container can read it (the `z` on its mount, #201). Without that, Postgres can't read its init scripts and never starts.

CI brings this stack up with `scripts/check-compose-stack.sh` (#140), when `docker/` changes and weekly. The script checks that compose refuses to start without each of its required settings, `OMS_PUBLIC_HOST`, `POSTGRES_PASSWORD` and `OMS_DB_SERVER_PASSWORD` (#564, #568), and that the server refuses to start, exit 2, with neither a token nor identities, or with both (#146, #643), and publishes only the frontend's port, that every service becomes healthy, that Postgres loaded the schema, that the server seeded its volume, that `/ws` refuses an upgrade without the token or from another Origin, that a new order sent to the frontend's `/ws` with the token gets its execution report, that the order and the seeded reference data reach Postgres (#151), and that the server's role there can't create a table or truncate the orders (#568). Then it starts the stack again with identities instead of the token, from a file in `docker/ws-identities`: an identity's token opens a session, the shared one doesn't, and the identity's order comes back NEW (#643). It reaches nginx over `https` and `wss` with a certificate made for the run, verifying it (#226). Run it before changing `docker-compose.yml`, `nginx.conf`, `.env.example` or `init-db/`. It takes about three minutes, and it publishes no host port, so it can run beside a stack that is already up.

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
POSTGRES_PASSWORD=             # The superuser's password, required (#568): openssl rand -hex 32 makes one
OMS_DB_SERVER_PASSWORD=        # The server's role's password, required (#568). Quotes and backslashes are fine, since libpq gets it on its own (#642); leave out $, below
POSTGRES_DB=oms                # Database name
OMS_WS_TOKEN=                  # WebSocket access token, required: openssl rand -hex 32 makes one (#146)
OMS_ALLOWED_ORIGINS=           # Pages a browser may connect from, comma-separated; empty allows any (#146)
OMS_HSTS=                      # Strict-Transport-Security, such as max-age=31536000, once certs/ holds a certificate browsers trust (#226)
OMS_PUBLIC_HOST=               # Host and port browsers reach the app at, required: localhost:3000, or oms.example.com:3000 (#564)
```

A password in `.env` may hold a quote or a backslash (#642), but leave out `$`. podman-compose 1.5.0 reads `${NAME}` as a variable, in quotes too, and keeps a lone `$` and `$$` as written; Docker Compose reads a `$` its own way. A password without one, such as `openssl rand -hex 32` makes, is taken as written by both.

### Access

The server refuses a WebSocket client that doesn't offer the access token, `OMS_WS_TOKEN` in `.env` (#146). Without it or identities (below), the server refuses to start, since compose gives it `--require-ws-auth` (#643). Compose can require only one of the two variables, so the refusal is the server's, as it starts, not compose's: `docker compose up` brings the stack up, `docker compose ps` shows `oms-server` restarting with exit code 2, and `docker compose logs oms-server` says why. Before #643 compose itself refused to start without a token. A browser can't set a WebSocket's headers, so a client offers the token as a subprotocol, `oms-token.<token>`, beside the protocol's own, `oms-v1`; the server names only `oms-v1` in its answer. The app asks for the token when a tab opens it, and **Change token** in its header asks again. A token has at least <!-- DOC_CHECK:ws_min_token_length -->16<!-- /DOC_CHECK --> letters, digits, `.`, `_` or `-`.

`OMS_ALLOWED_ORIGINS`, comma-separated, such as `https://oms.example.com:3000`, limits the pages a browser may connect from: a page from another Origin is refused, even with the token. Empty, as in `.env.example`, allows a page from anywhere that has the token. A client that sends no Origin isn't a browser, and the token alone decides.

The server answers an upgrade without the right token with 401, one from an Origin off the list with 403, and a request that isn't a WebSocket upgrade with 426, then closes the connection.

Everyone with the shared token sees every account and order, and can trade on any account. For less, give each client an identity instead.

#### Identities

With `--ws-credentials` (#225), each client has a token of its own, and its session sees and trades only its identity's accounts: their orders, in the snapshot and in every report after, and its new orders on them alone. Book depth, which shows no account, still goes to every subscriber. README's [Standalone Server](../README.md#standalone-server) gives the file's format. To use it with this stack:

1. Make a token per identity with `openssl rand -hex 32`, and its hash with the server's `--hash-ws-token`: `printf '%s' "$TOKEN" | orderProcessorServer --hash-ws-token`, from any build of the server.
2. Write a file in `docker/ws-identities`, such as `docker/ws-identities/identities`, a line per identity: its name, its token's hash, and its accounts. Compose mounts the directory, read-only, at `/etc/oms/ws-identities` (#643), and git ignores what's in it. `chmod 0640` the file: the server's user in the image (uid 10001) is in group 0, which under rootless podman is your own group. Under Docker running as root, also `sudo chgrp 0` it, as the TLS key. The file holds only hashes, so 0644 would do too.
3. In `.env`, set `OMS_WS_CREDENTIALS` to the file as the server sees it, `/etc/oms/ws-identities/identities`, and leave `OMS_WS_TOKEN` empty: the server refuses the two together. Nothing in `docker-compose.yml` changes, where before #643 it had to be edited.

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

`docker-compose.yml` gives its own command, which adds `--keep-closed-days 1` (#597): Postgres keeps every order in this stack, so each start drops the closed orders of the business days before its own. Keep it in a command of your own:

```yaml
oms-server:
  command: ["--port", "8080", "--data-dir", "/data", "--keep-closed-days", "1", "--workers", "4"]
```

README's [Standalone Server](../README.md#standalone-server) table gives every option the server takes, and is the only table of them: this file had a copy, which fell six options behind it (#706). In the image, `--data-dir` is `/data`, its volume, as the CMD above gives it.

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

To load your own reference data instead of the test set, give `seedData` your file with `--file` (#585): README's [Standalone Server](../README.md#standalone-server) gives its format. Mount it read-only, by its absolute path (`z` relabels it for SELinux), and make it readable by the server's user, `oms` (`chmod 0644 reference.json`). Load it before the first `up`, so that the volume has your instruments when the server's container first starts: its `seedData --if-empty` then leaves the volume alone, where it would add the test set to a volume with no instrument.

```bash
docker compose build oms-server
docker compose run --rm --no-deps -v "$PWD/reference.json:/reference.json:ro,z" \
    --entrypoint seedData oms-server --data-dir /data --file /reference.json
docker compose up -d
```

On a volume in use, stop the server first and start it after, as above.

The file must give the clearing firm and the account the server takes as its defaults: `HOUSE-CLEARING` and `TRADING-1`, unless compose's `command:` names others with `--clearing-firm` and `--default-account`. A volume that holds reference data without them stops the server's start, with `Can't start:` naming the missing one and exit 1 (#640), since every order, or every order that names no account, would be refused. Under `--ws-credentials` the default account isn't needed.

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

### Backing up and checking the data

The server image includes `mdb_copy`, LMDB's copy, which reads the environment in one read transaction, so a copy taken while the server runs is consistent (#591). It writes the copy to its standard output. Run it in the server's own container: LMDB keeps its readers' process ids in the lock file, and a process in another container's PID namespace could be taken for one of the server's. Then check the copy with `seedData --check`, in a container of its own, so that the check's memory, which holds every order as the server does, isn't counted against the server's limit. The copy is mounted read-only, where LMDB needs no lock file: on a writable mount it would open `lock.mdb` for writing, and make one, as every reader does:

```bash
backup=oms-data-$(date -u +%Y%m%dT%H%M%SZ)
mkdir "$backup"
docker compose exec -T oms-server mdb_copy -c /data > "$backup/data.mdb"
chmod 0755 "$backup" && chmod 0644 "$backup/data.mdb"   # readable by the image's user, oms
docker compose run --rm --no-deps -v "$PWD/$backup:/backup:ro,z" --entrypoint seedData oms-server --check /backup
```

A server that can't start restarts again and again, and its container's `seedData --if-empty` opens the volume each time, so stop it first, with `docker compose stop oms-server`. Then copy the volume with `docker compose run --rm --no-deps -T --entrypoint mdb_copy oms-server -c /data > "$backup/data.mdb"` in place of the `exec` line.

`-c` leaves out the free pages, and `-T` keeps the copy's bytes off a terminal. `seedData --check` restores a directory as the server's start does, with the environment opened read-only, and writes nothing to its data file: it says what it found and exits 0 when every record restores, or names the record it refuses, by its key and version, and exits 1. A server's start names a record it refuses the same way. Keep the copy outside the volume: `docker compose down -v` deletes the volume, and every order with it.

### A volume written by an older image

Server images before #158 ran as root, so the files they wrote in `oms-data` belong to root. The server now runs as `oms` and can't open them: it exits with `LMDBStorage::load mdb_env_open: Permission denied`, and restarts again and again. Give the volume to `oms` once, and the server comes up at its next restart:

```bash
docker compose run --rm --no-deps --user 0 --entrypoint chown oms-server -R oms:oms /data
```

### A database made before the server had its own role

Since #568 the server connects to Postgres as `oms_server`, a role that may read and write only the rows it mirrors, where it connected as the superuser. `init-db/002_server_role.sh` makes the role, but Postgres runs `init-db` only when it initialises a new volume. On a volume made before, set `OMS_DB_SERVER_PASSWORD` in `.env`, run `docker compose up -d` (the server can't reach Postgres yet, and runs on LMDB alone meanwhile), then make the role once with the same script, which reads the password from `oms-db`'s environment:

```bash
docker compose exec oms-db sh /docker-entrypoint-initdb.d/002_server_role.sh
docker compose restart oms-server
```

## Build Details

### oms-server (Multi-stage)

| Stage | Base Image | Purpose |
|-------|-----------|---------|
| Builder | `centos:stream10` | Installs GCC 15 (gcc-toolset-15), CMake, TBB, spdlog, Boost, LMDB, OpenSSL; compiles Release build |
| Runtime | `centos:stream10` | Minimal image with only runtime libraries, and LMDB's tools for a backup (#591); copies `orderProcessorServer` and `seedData` |

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

**Enum types** (matching C++ and TypeScript): `side`, `order_type`, `order_status`, `exec_type`, `time_in_force`, `currency`, `capacity`, `settl_type`, `account_type`

**Tables:**

| Table | Description |
|-------|-------------|
| `instruments` | Reference data — symbol, security ID |
| `accounts` | Reference data — account name, firm, type |
| `clearing_firms` | Reference data — clearing firm names |
| `orders` | Order archive with full order parameters |
| `executions` | Unused: nothing writes execution reports to Postgres, which mirrors orders and reference data alone (#575, #599) |
| `schema_version` | The versions the schema has been at, a row a migration (#590): the server reads the highest as it connects |

**Indexes:** active orders (a partial index), and executions by order and time. The frontend doesn't query Postgres: it gets everything from the server over WebSocket.

Prices are `NUMERIC` with no precision, so they hold any price the engine accepts (#538), and read back with the places they were written with: `101.25`, where `NUMERIC(18,8)` gave `101.25000000`. Since #590 a new volume has no `users` or `audit_log` table and no `audit_action` type, which nothing read or wrote; a volume made before keeps them, and nothing drops them.

### Schema versions and migrations (#590)

Postgres runs `init-db/` only on a new volume, so an older one keeps the schema it was made with. Since #590 the schema has a version: `init-db` makes a new volume at the one the server's statements are written for, in `schema_version`, and each change to the schema comes with a numbered migration in `docker/migrations/` that brings an older volume up to it. The server never changes the schema itself. As it connects to Postgres, its writer reads the version:
- **No `schema_version`:** a volume made before #590. The writer warns once, and mirrors to it as before.
- **Another version:** the writer refuses it, as it refuses a statement the schema refuses: it logs once that the schema is at another version, counts each refusal, writes nothing, and tries again on its backoff, so a volume migrated meanwhile is taken at the next attempt.

To bring a volume up to date, run the migrations as the database's owner, with the stack up, from the repository's root:

```bash
scripts/pg-migrate.sh            # applies each migration above the volume's version, each in one transaction
scripts/pg-migrate.sh --check    # checks the migrations alone: numbered from 001 with no gap, and none holding DROP
```

It runs `psql` through compose (`docker compose exec -T oms-db psql -U oms -d oms`), or the command `PSQL` names. A migration holds no `DROP`, so nothing a volume holds is lost: a volume made before #590 keeps its `users` and `audit_log` tables, which nothing reads or writes.

The first migration versions a volume made before #590, whatever it was made with. It does what this section once gave as statements to run by hand, each of which leaves a volume that already has what it makes as it was:
- the quantity columns BIGINT and the ClOrdID columns TEXT, where a volume from before #151 held INTEGER and 64 characters (an order above 2147483647, or a longer ClOrdID, failed its write);
- `orders.stop_triggered` and the `TRIGGERED` execution type (#236), and `orders.day_start` (#244), without which the server couldn't prepare its order write and wrote nothing;
- the prices `NUMERIC` with no precision, where `NUMERIC(18,8)` refused 1e10 or more (#538);
- `schema_version` at version 1, which the server's role may read where the volume has the role (#568).

Starting over loses nothing that LMDB doesn't hold: `docker compose down -v` deletes every volume, the server's data too.

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
# - "Record ... version ..." or "Order ... version ...": a record the server can't restore, named by its key and version
# - Other LMDB errors → corrupt data. Before `docker compose down -v`, which starts again and deletes every order,
#   copy the volume and check the copy (see "Backing up and checking the data")
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
