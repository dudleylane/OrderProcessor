# Docker Deployment Guide

This directory contains everything needed to run the full OrderProcessor OMS stack: PostgreSQL database, C++ WebSocket server, and React frontend.

## Prerequisites

- Docker or Podman with `docker compose` (or `podman-compose`)
- ~2 GB disk space for build images
- Ports 3000, 5432, and 8080 available

## Quick Start

```bash
cd docker
cp .env.example .env          # Set PostgreSQL credentials
docker compose up --build -d   # Build and start all services
```

Open **http://localhost:3000** to access the OMS frontend.

The first build takes several minutes (C++ compilation inside the container). Subsequent starts use the Docker layer cache and are fast.

## Services

| Service | Port | Image | Description |
|---------|------|-------|-------------|
| `oms-db` | 5432 | `postgres:16-alpine` | PostgreSQL database with OMS schema |
| `oms-server` | 8080 | `Dockerfile.oms-server` | C++ OrderProcessor WebSocket server |
| `oms-frontend` | 3000 | `Dockerfile.oms-frontend` | React UI served by nginx |

### Startup Order

1. **oms-db** starts first, initializes schema from `init-db/001_schema.sql`
2. **oms-server** waits for `oms-db` to be healthy (`pg_isready` check), then starts the C++ engine on port 8080
3. **oms-frontend** waits for `oms-server` to start, then serves the React app on port 3000

### Networking

```
Browser :3000 ──→ nginx (oms-frontend)
                    ├── /        → serves React SPA static files
                    └── /ws      → proxy_pass to oms-server:8080 (WebSocket upgrade)
```

The nginx config (`nginx.conf`) handles WebSocket upgrade headers so the frontend connects to `ws://localhost:3000/ws`, which is proxied internally to the C++ server.

## Configuration

### Environment Variables

Copy `.env.example` to `.env` and set values:

```bash
POSTGRES_USER=oms              # PostgreSQL username
POSTGRES_PASSWORD=changeme     # PostgreSQL password (change this!)
POSTGRES_DB=oms                # Database name
POSTGRES_PORT=5432             # Host port for PostgreSQL
```

### Server Options

The C++ server accepts command-line arguments via the `CMD` directive in `Dockerfile.oms-server`:

```dockerfile
ENTRYPOINT ["orderProcessorServer"]
CMD ["--port", "8080", "--data-dir", "/data"]
```

Override in `docker-compose.yml`:

```yaml
oms-server:
  command: ["--port", "8080", "--data-dir", "/data", "--workers", "4", "--huge-pages"]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 8080 | WebSocket listen port |
| `--data-dir` | `/data` | LMDB persistence directory (mounted as Docker volume) |
| `--workers` | 0 | Worker thread count (0 = auto-detect) |
| `--cpu-affinity` | -1 | Pin main thread starting from this core (-1 = disabled) |
| `--huge-pages` | off | Enable huge page allocation (requires host configuration) |

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

The `seedData` utility is included in the server image. To populate reference data:

```bash
docker compose exec oms-server seedData --data-dir /data
```

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

## Build Details

### oms-server (Multi-stage)

| Stage | Base Image | Purpose |
|-------|-----------|---------|
| Builder | `centos:stream10` | Installs GCC, CMake, TBB, spdlog, Boost, LMDB, numactl; compiles Release build |
| Runtime | `centos:stream10` | Minimal image with only runtime libraries; copies `orderProcessorServer` and `seedData` |

The build compiles with `-DCMAKE_BUILD_TYPE=Release` (LTO, -O3, -march=native) and disables tests and benchmarks to minimize build time.

### oms-frontend (Multi-stage)

| Stage | Base Image | Purpose |
|-------|-----------|---------|
| Builder | `node:20-slim` | Runs `npm ci` and `npm run build` (TypeScript + Vite) |
| Runtime | `nginx:alpine` | Serves the built SPA; uses custom `nginx.conf` for WebSocket proxying |

The `VITE_WS_URL` is set to `ws://localhost:3000/ws` at build time, so the frontend connects through the nginx proxy.

## Database Schema

The schema is initialized automatically on first boot from `init-db/001_schema.sql`. It includes:

**Enum types** (matching C++ and TypeScript): `side`, `order_type`, `order_status`, `exec_type`, `time_in_force`, `currency`, `capacity`, `settl_type`, `account_type`, `audit_action`

**Tables:**

| Table | Description |
|-------|-------------|
| `users` | Authentication and authorization (UUID primary keys) |
| `instruments` | Reference data — symbol, security ID |
| `accounts` | Reference data — account name, firm, type |
| `clearing_firms` | Reference data — clearing firm names |
| `orders` | Order archive with full order parameters |
| `executions` | Execution reports (single-table inheritance via `exec_type`) |
| `audit_log` | Append-only audit trail with JSONB detail |

**Indexes** optimized for frontend blotter queries: active orders (partial index), executions by order and time, audit log with GIN index on JSONB.

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

```bash
# Check server logs
docker compose logs oms-server

# Common issues:
# - "Address already in use" → port 8080 is taken
# - Database connection errors → oms-db not healthy yet (check health with docker compose ps)
# - LMDB errors → volume permissions or corrupt data (try docker compose down -v)
```

### Frontend shows "Disconnected"

1. Check that `oms-server` is running: `docker compose ps`
2. Check server logs: `docker compose logs oms-server`
3. Verify nginx proxy: `curl -i http://localhost:3000/ws` should return a WebSocket upgrade error (not 404)
4. Check browser console for WebSocket connection errors

### Reset everything

```bash
docker compose down -v           # Stop and delete all data
docker system prune -f           # Clean up dangling images
docker compose up --build -d     # Fresh start
```
