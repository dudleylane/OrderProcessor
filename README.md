# OrderProcessor

A high-performance, concurrent order processing engine written in C++23 with a React monitoring frontend. Provides ACID-compliant transaction processing, state machine-based order lifecycle management, order matching against an in-memory order book, and real-time system monitoring via WebSocket.

**Original Author:** Sergey Mikhailik
**Modernization & Extensions:** dudleylane, Claude
**License:** GNU Affero General Public License v3 (AGPL-3.0)

## Features

### Core Engine
- **ACID Transactions:** Full transaction support with operation-level rollback on failure
- **State Machine:** <!-- DOC_CHECK:order_state_count -->13<!-- /DOC_CHECK --> order states and <!-- DOC_CHECK:transition_count -->47<!-- /DOC_CHECK --> transitions, managed via Boost Meta State Machine (MSM)
- **Order Matching:** Price-time priority matching engine with per-instrument order books
- **Deferred Event Chaining:** Trade executions trigger cascading events (e.g., settlement)
- **Subscription Filtering:** Selective event delivery per client via SubscrManager

### Low-Latency Infrastructure
- **Zero-Allocation Hot Path:** 5-tier memory hierarchy — stack, arena (<!-- DOC_CHECK:arena_size -->2048<!-- /DOC_CHECK -->-byte bump), CAS pool (<!-- DOC_CHECK:pool_size -->1024<!-- /DOC_CHECK --> slots), TBB scalable allocator, heap fallback
- **Lock-Free Pool:** TransactionScopePool with exponential backoff CAS and cache miss tracking
- **CPU Affinity:** Thread-to-core pinning, for the main thread (`--cpu-affinity`) and the TaskManager's workers. `CpuAffinity` can also set SCHED_FIFO priority, but nothing calls it
- **NUMA-Aware Allocation:** `TransactionScopePool` can bind its slots to the local NUMA node (`mbind()` via NumaAllocator). The server builds its pools in the default mode, so it doesn't use this
- **Huge Pages:** `TransactionScopePool` can put its slots in 2 MB pages. The server doesn't ask it to: `--huge-pages` only reports whether huge pages are available
- **False-Sharing Prevention:** `CacheAlignedAtomic<T>` (`alignas(64)`) on all shared atomics
- **Branch Prediction:** `[[likely]]`, `[[unlikely]]`, `[[assume]]` (C++23) throughout hot path

### OMS Frontend (React + TypeScript)
- **Order Entry:** Full form with symbol, side (<!-- DOC_CHECK:side_count -->6<!-- /DOC_CHECK --> types), order type (<!-- DOC_CHECK:order_type_count -->5<!-- /DOC_CHECK --> types: Market/Limit/Stop/StopLimit/FxSwap), time-in-force (<!-- DOC_CHECK:tif_count -->7<!-- /DOC_CHECK --> types), capacity, currency
- **Order Blotter:** Live table with inline Cancel and Replace actions
- **Execution Blotter:** Trades, rejects, corrections, cancels with CSV export
- **Order Book Panel:** 8-level bid/ask depth with staleness indicator and per-instrument subscription
- **Position Summary:** Client-computed net positions with VWAP

### Monitoring Dashboard
- **Real-Time Metrics:** MetricsPublisher broadcasts 17 system metrics at 1/sec over WebSocket
- **Dashboard Tab:** Event/transaction throughput charts, queue depth, processor utilization bars
- **Memory Admin Tab:** TransactionScopePool health indicator, cache miss rate trending, arena details
- **Alert System:** Client-side rule engine with configurable thresholds, severity levels, localStorage persistence

### FIX Protocol Gateway (Optional)
- **FIX 4.4 Support:** Institutional clients connect via standard FIX protocol (`BUILD_FIX=ON`)
- **QuickFIX/C++ Engine:** ThreadedSocketAcceptor with thread-per-session isolation
- **Inbound:** NewOrderSingle (35=D), OrderCancelRequest (35=F), OrderCancelReplaceRequest (35=G), and NewOrderMultileg (35=AB) for FX swaps
- **FX Swaps:** a NewOrderSingle with OrdType G carries the near leg's SettlDate (64), and the far leg's price (6500) and settlement date (6501); a NewOrderMultileg carries both legs, each with its LegSettlDate (588). Each date is a LocalMktDate, `YYYYMMDD`, which the server takes as midnight UTC of that day, as the frontend sends a WebSocket swap's dates, and a swap's ExecutionReports give each leg's LegSettlDate the same way (#198). A side's two reports for one fill, its near leg's and its far leg's, carry the same TradeLinkID (820), drawn for that fill and that side, so a client can pair them (#199). A date that isn't one, such as `-5` or one with no such day, is refused before anything is stored, with an ExecutionReport (150=8, 39=8, 103=0) whose Text (58) names the field (#190). The gateway used to read and write these dates as the engine's milliseconds
- **Outbound:** ExecutionReport (35=8), OrderCancelReject (35=9). A rejected order's ExecutionReport (150=8, 39=8) carries OrdRejReason 103=0 (broker / exchange option) and the engine's reason in Text (58)
- **Gateway Refusals:** an order the gateway refuses before the engine sees it is answered to its sender alone, with such an ExecutionReport. OrdRejReason is 1 for an unknown symbol, 11 for a NewOrderMultileg that isn't a two-leg FX swap (#189), 13 for a quantity (#145), and 15 for an account the session may not trade (#147). Before #189 an unknown symbol on a NewOrderSingle got no answer, and a NewOrderMultileg's refusals went to every session logged on, as BusinessMessageRejects naming no order
- **ClOrdIDs per Session:** a ClOrdID belongs to the FIX session that sent it, as FIX scopes it. Two sessions may use the same ClOrdID, and a cancel or replace finds only the requesting session's own orders: one that names another session's ClOrdID is answered as an unknown order (102=1). The server stores a FIX order's ClOrdID with its session, as `FIX:<SenderCompID>-><TargetCompID>|<ClOrdID>`, and that is the `clOrderId` WebSocket clients see and send back; FIX messages carry the client's own. Orders stored before this keep their ClOrdID alone, and their own session can still cancel or replace them
- **ClOrdIDs in Use:** an order whose ClOrdID its session already uses is refused without being stored. A NewOrderSingle gets an ExecutionReport with 150=8, 39=8, OrdRejReason 103=6 and the reason in Text (58); a cancel/replace request gets an OrderCancelReject (35=9) with 434=2, CxlRejReason 102=6 and 58
- **Cancel (35=F):** the engine decides a cancel after the order's earlier transactions. An accepted one is acknowledged with an ExecutionReport: 150=4, 39=4, ClOrdID (11) from the request and OrigClOrdID (41) naming the cancelled order. A refused one gets an OrderCancelReject (35=9, 434=1) with the same 11 and 41, CxlRejReason 102 (0 too late, 1 unknown order, 3 a replace pending, 99 other), the order's status in 39 (8 when the order is unknown), and the reason in Text (58). A cancel that no FIX request asked for, such as the engine cancelling a market order it can't fill, or a cancel from the WebSocket side, carries the order's own ClOrdID in 11 and no 41
- **Cancel/Replace (35=G):** the engine decides a replace after the order's earlier transactions. An accepted one is acknowledged once, with the replacement's ExecutionReport: 150=5, 39=0 or 1, ClOrdID (11) from the request, OrigClOrdID (41) naming the replaced order, and the replacement's own OrderID (37); it keeps the original's fills. A refused one gets an OrderCancelReject (35=9, 434=2) with CxlRejReason 102 (0 too late, 1 unknown order, 3 a cancel or replace pending, 6 ClOrdID in use, 99 other), the order's status in 39, and the reason in Text (58). Side (54) and Symbol (55) must match the order. FIX 4.4 has no Replaced status, so a replaced order shows as 39=4 if a later request names it
- **Server Defaults:** FIX orders get the server's clearing firm, as WebSocket orders do, and an order with no Account (1) trades its counterparty's first account (#147). If the data directory has no clearing firm, the gateway answers the order with a BusinessMessageReject (35=j, BusinessRejectReason 4) instead of passing it to the engine
- **Logon (#147):** a counterparty's Logon (35=A) must carry the Username (553) and Password (554) that its line in the credentials file, `--fix-credentials`, gives. The file names each counterparty by its CompID, and the server makes a session for each: `config/fix.cfg` names none, and gives only the server's side, SenderCompID `ORDER_PROCESSOR`. A Logon with a wrong or missing password is refused with a Logout, and so is one from a CompID no line names, which QuickFIX drops; the server logs why. The file stores PBKDF2-HMAC-SHA256 hashes, which `orderProcessorServer --hash-fix-password` makes from a password on standard input
- **TLS (#228):** with `--fix-tls-cert` and `--fix-tls-key`, the acceptor serves TLS only (QuickFIX's `ThreadedSSLSocketAcceptor`), with TLS 1.2 and 1.3 unless `config/fix.cfg` sets `SSLProtocol`; a plain TCP client gets no answer. The Docker image serves FIX this way, with the stack's certificate. Without the flags FIX runs over plain TCP, where a counterparty's password and every order cross the network in the clear, and the server warns so at startup. QuickFIX's other SSL settings, such as `CertificationAuthoritiesFile` and `CertificateVerifyLevel` for counterparties' own certificates, are read from `config/fix.cfg`'s `[DEFAULT]` as QuickFIX documents them; this repository's tests don't cover them
- **Accounts (#147):** a counterparty trades only the accounts its line lists. An order naming another is refused with an ExecutionReport: 150=8, 39=8, OrdRejReason 103=15 (unknown account) and the reason in Text (58). Before #147 any FIX session could trade any account
- **Sessions:** `config/fix.cfg` runs one session a day. It is open all day (StartTime equals EndTime), and QuickFIX starts a new one, from sequence number 1, at its first check after 00:00 UTC. Within the day a client that reconnects keeps its sequence numbers (`ResetOnLogon=N`), so a ResendRequest (35=2) gets back the ExecutionReports sent while it was away, with PossDupFlag (43=Y); QuickFIX keeps them in `FileStorePath`. The FIX logs in `FileLogPath` rotate: `FileLogMaxMB` per file, and `FileLogBackups` older files (#160)
- **Gateway Pattern:** Thin adapter — FIX messages translate to internal events at the boundary; core engine is protocol-agnostic
- **MultiOutQueues:** Fan-out adapter routes execution reports to both WebSocket and FIX sessions simultaneously
- **8 FX Currencies:** USD, EUR, GBP, JPY, CHF, AUD, CAD, NZD

### Persistence
- **LMDB Storage:** Memory-mapped key-value store with versioned records. An order keeps its newest version and the one before it, and the map doubles when a write finds it full (#148)
- **PostgreSQL Write-Behind:** with `BUILD_PG=ON` and a connection string, the server mirrors its reference data and orders to Postgres, behind LMDB, which stays its record (#151)
- **File Storage:** Legacy versioned file persistence (deprecated in favor of LMDB)

### Deployment
- **Docker Compose:** 3-service stack — PostgreSQL 16, C++ server, React/nginx frontend. Only the frontend's port and the FIX acceptor's (5001) are published, and a WebSocket client needs the access token (#146)
- **Multi-Stage Dockerfiles:** CentOS Stream 10 build stage, minimal runtime images

---

## Requirements

- **Platform:** Linux (CentOS 10 / RHEL 10 or compatible)
- **Compiler:** GCC 15, from `gcc-toolset-15` on el10. The engine uses C++23's `<flat_map>`, which the system's GCC 14 doesn't have (#132)
- **Build System:** CMake 3.21+

### Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| oneTBB | 2021.x+ | Concurrent containers, mutex, task scheduling |
| spdlog | 1.x+ | Fast logging |
| Boost | 1.70+ | Headers only (MPL for Meta State Machine) |
| LMDB | Latest | Persistent key-value storage backend |
| numactl | Latest | NUMA-aware memory allocation |
| QuickFIX | 22 | FIX protocol engine (optional, `BUILD_FIX=ON`), from the maintained fork, at the ref that `QUICKFIX_REF` in `docker/Dockerfile.oms-server` pins: CI and the image both build it (#178) |

Install on CentOS 10:
```bash
# Core dependencies
sudo dnf install gcc-toolset-15 tbb-devel spdlog-devel boost-devel cmake lmdb-devel numactl-devel

# Optional: QuickFIX 22 for FIX protocol support. scripts/build-quickfix.sh builds the pinned ref
# (see docs/QUICKFIX_DEPENDENCY.md)
```

---

## Building

```bash
git clone https://github.com/dudleylane/OrderProcessor.git
cd OrderProcessor
source /opt/rh/gcc-toolset-15/enable   # GCC 15 first on PATH, for this shell
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | ON | Build unit tests with Google Test |
| `BUILD_BENCHMARKS` | ON | Build benchmarks with Google Benchmark |
| `BUILD_APP` | ON | Build WebSocket server and seed data utility |
| `BUILD_PG` | OFF | Build the PostgreSQL write-behind (requires libpqxx 8); the server mirrors to Postgres when it's given a connection string (#151) |
| `BUILD_FIX` | OFF | Build FIX 4.4 protocol gateway (requires QuickFIX/C++) |
| `BUILD_FUZZ` | OFF | Build the `parseClientMessage` fuzz target (`docs/DEBUGGING.md`, Fuzzing) |
| `WARNINGS_AS_ERRORS` | OFF | Fail the build on a warning in the project's own code; CI's Release job and the pre-push hook turn it on (#167) |
| `MARCH` | `native` | The CPU a Release build targets (`-march`): `native`, this machine's, or an ISA level such as `x86-64-v3`. The server image builds for `x86-64-v3` (#170) |

### Build Outputs

- **Library:** `build/liborderEngine.a`
- **Tests:** `build/orderProcessorTest`
- **Benchmarks:** `build/orderProcessorBench`
- **Server:** `build/orderProcessorServer` (WebSocket server)
- **Seed Data:** `build/seedData` (test data generator)

---

## Running

### Standalone Server

```bash
./build/seedData --data-dir ./data      # test instruments, accounts and the clearing firm; skips any that exist
./build/orderProcessorServer --port 8080 --data-dir ./data
```

With FIX gateway enabled (requires `BUILD_FIX=ON`):
```bash
./build/orderProcessorServer --port 8080 --data-dir ./data --fix-cfg config/fix.cfg --fix-credentials fix-credentials
```

Add `--fix-tls-cert tls.crt --fix-tls-key tls.key` to serve FIX over TLS (#228).

`fix-credentials` has a line per FIX counterparty: its CompID, Username, password hash and the accounts it may trade (#147). `orderProcessorServer --hash-fix-password` reads a password from standard input and prints its hash:

```
# CompID   Username   PasswordHash                          Accounts
CLIENT_A   client_a   pbkdf2-sha256$600000$<salt>$<key>     TRADING-1,TRADING-2
```

Seed a new data directory first. Until then it has no instruments, accounts or clearing firm, and the server answers every order with an `error` (#161). `seedData` seeds test data, so don't run it on a directory in real use.

Server options:

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 8080 | WebSocket listen port |
| `--bind` | `0.0.0.0` | The WebSocket server's address: an IP address, such as `127.0.0.1` for this host only (#146). The FIX acceptor listens where `--fix-cfg`'s settings say: `SocketAcceptPort`, on every interface unless `SocketAcceptAddress` is set |
| `--ws-token-file` | none | A file whose first line is the access token a WebSocket client must offer (#146). `OMS_WS_TOKEN` gives it too, but not both |
| `--allowed-origin` | any | An Origin a browser page may connect from; repeat it for more. `OMS_ALLOWED_ORIGINS` adds a comma-separated list (#146) |
| `--pg-connection` | none | The Postgres to mirror to, a libpq connection string (#151). `OMS_PG_CONNECTION` gives it too, but not both: prefer it, since every user on the host can see a flag in the process list. Needs `BUILD_PG=ON` |
| `--data-dir` | `./data` | LMDB persistence directory |
| `--workers` | 0 | TBB worker threads, and as many event and transaction processor pairs. 0 leaves TBB at its default, a thread per core, and runs 2 pairs |
| `--cpu-affinity` | -1 | Pin main thread starting from this core (-1 = disabled) |
| `--huge-pages` | off | Reports whether huge pages are available; nothing allocates from them yet |
| `--fix-cfg` | *(none)* | Path to QuickFIX settings file (enables FIX gateway); needs `--fix-credentials` |
| `--fix-credentials` | *(none)* | The FIX counterparties: who may log on, with which password, to trade which accounts (#147). Each gets a session |
| `--hash-fix-password` | | Read a password from standard input, print its hash for `--fix-credentials`, and exit |
| `--fix-tls-cert`, `--fix-tls-key` | *(none)* | Serve FIX over TLS only, with this certificate (PEM, followed by any intermediate ones) and its key (PEM, not encrypted); both or neither, and only with `--fix-cfg` (#228). Without them FIX is plain TCP, and the server warns |
| `--clearing-firm` | `HOUSE-CLEARING` | Clearing firm put on every order; must exist in the data directory |
| `--default-account` | `TRADING-1` | Account for an order that names none; must exist in the data directory |
| `--debug` | off | Debug logging: about six lines per transaction, written inside the transaction manager's lock |
| `--lmdb-map-mb` | 256 | LMDB's starting map, in MiB. A write that finds the map full doubles it, and logs a warning |
| `--max-connections` | <!-- DOC_CHECK:ws_max_connections -->256<!-- /DOC_CHECK --> | WebSocket clients connected at once. One more is closed as soon as it's accepted |
| `--max-session-queue-mb` | <!-- DOC_CHECK:ws_max_session_queue_mb -->16<!-- /DOC_CHECK --> | What may wait to be sent to one client, in MiB. A client that doesn't read is dropped once more waits, and gets the current state again when it reconnects |

Without `--ws-token-file` or `OMS_WS_TOKEN`, any client that reaches the server may connect, and the server warns once at startup (#146). A token has at least <!-- DOC_CHECK:ws_min_token_length -->16<!-- /DOC_CHECK --> letters, digits, `.`, `_` or `-` (`openssl rand -hex 32` makes one); see [Connecting](#connecting) for how a client offers it. A token file with nothing on its first line, or `OMS_WS_TOKEN` set but empty, is refused rather than taken as no token.

An option the server doesn't know, an option without its value, or a number outside the option's range is a usage error: the server prints why and its usage on standard error, and exits with status 2 (#173). `--help` prints the usage.

The log goes to `exchange.log` in the working directory. The environment variable `EXCHANGE_LOG` sends it elsewhere: a file's path, or `-` for standard output, which the Docker image uses, so `docker compose logs` shows it.

### Docker Compose (Full Stack)

```bash
cd docker
cp .env.example .env        # Set PostgreSQL credentials
sed -i "s/^OMS_WS_TOKEN=.*/OMS_WS_TOKEN=$(openssl rand -hex 32)/" .env   # The access token (#146)
mkdir -p certs && openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" -keyout certs/tls.key -out certs/tls.crt \
    && chmod 0640 certs/tls.key   # A self-signed TLS certificate (#226)
docker compose up --build -d
```

The first `up` seeds the new data volume with test data, so the stack takes orders at once: the server's container runs `seedData --if-empty` before the server (#161).

This starts 3 services:

| Service | Host port | Description |
|---------|-----------|-------------|
| `oms-db` | none (5432 in the stack) | PostgreSQL 16 (Alpine) with schema initialization |
| `oms-server` | 5001, the FIX acceptor (8080 only in the stack) | C++ OrderProcessor WebSocket server and FIX acceptor |
| `oms-frontend` | 3000 | React UI served by nginx over TLS, proxies `/ws` to server |

Open **https://localhost:3000** to access the OMS frontend, and accept the self-signed certificate the browser warns about. The app asks for the access token, the `OMS_WS_TOKEN` in `docker/.env`. nginx serves the app and its WebSocket over TLS only (#226). [docker/README.md](docker/README.md#tls) says how to use a real certificate, and how Docker running as root needs the key's group set.

```bash
docker compose logs -f              # Follow all logs
docker compose logs -f oms-server   # Server logs only
docker compose down                 # Stop all services
docker compose down -v              # Stop and delete volumes
```

### Frontend Development

```bash
cd src/oms-frontend
npm install
npm run dev                         # Vite dev server at http://localhost:3000
```

The app asks for the server's access token first: leave it empty for a server run without one. It connects to `/ws` on the host and port it was loaded from, and the dev server passes that on to the server at `ws://localhost:8080/ws`. The image's nginx does the same for the server's container. To point a build somewhere else, set `VITE_WS_URL` when building.

The styles are Tailwind CSS 4 (#220), so the app needs a browser from 2023 or later: Safari 16.4, Chrome 111 or Firefox 128, the oldest that have the CSS features Tailwind 4 uses (cascade layers, `@property`, `color-mix()`). It keeps Tailwind 3's look:
- **The palette:** `src/tailwind-v3-palette.css` keeps Tailwind 3's colours, and its header says how to take Tailwind 4's.
- **Changed defaults:** `src/index.css` keeps the defaults Tailwind 4 changed: border colour, the button cursor, placeholder colour, and table-cell and option padding.
- **Hover:** Tailwind 4 applies `hover:` styles only on a device that can hover, so a touch screen no longer shows them after a tap.

---

## Testing

```bash
cd build && ctest --output-on-failure
```

458 tests across 33 test files covering all core components. Run a specific suite:
```bash
./orderProcessorTest --gtest_filter="ProcessorTest.*"
```

### Local pre-push gate

A tracked hook runs the build, `ctest -j1 --timeout 300`, and the clang-format check before every push, and refuses the push if any fails. It builds the working tree, so it also refuses a push unless the working tree is the commit pushed: `HEAD`, with nothing changed or untracked. Its build directory must be a Release one configured as CI configures it (`BUILD_TESTS`, `BUILD_BENCHMARKS` and `BUILD_APP` on, and on this branch `BUILD_FIX`). Enable it once per clone:

```bash
git config core.hooksPath scripts/hooks
```

Bypass deliberately with `git push --no-verify` and record why in the commit message.

### Lint

`scripts/lint.sh` runs shellcheck over the shell scripts, actionlint over the workflows, with shellcheck over their `run:` blocks, and hadolint over the Dockerfiles (#174). Each runs from its official container image, pinned by digest, over a read-only copy of the tracked files, with no network. CI runs it on every push, in the Format Check workflow; the pre-push hook doesn't. It needs podman, and its first run pulls about 145 MB of images.

### Benchmarks

```bash
./orderProcessorBench --benchmark_format=console
```

Export to JSON for comparison:
```bash
./orderProcessorBench --benchmark_out=results.json --benchmark_out_format=json
```

### Comparing Benchmarks Before and After a Change

`scripts/benchmark-regression.sh` compares the benchmark binary from before a change with the one after it, on the same machine. Nothing is committed: a comparison only means something on the machine that made it.

```bash
# Before the change: build, then save the benchmark binary as the baseline
./scripts/benchmark-regression.sh --update-baseline

# Make your change, rebuild, then compare the two binaries
./scripts/benchmark-regression.sh --no-build --pinned 2-5 --filter 'Pool|StateMachine'
```

The two binaries run in alternating rounds, and the order flips every round, so drift between runs (clock frequency, temperature, other load) falls on both alike. Each benchmark is judged by the median, over rounds, of its after/before ratio: by real time for benchmarks that ask for it (`/real_time`), by CPU time otherwise. The report lists every benchmark that moved more than the threshold, with the number of rounds in which it was slower and its range across rounds. The script exits 1 if a benchmark got slower by more than the threshold, and 2 if one failed to run or anything else went wrong.

Options: `--threshold N` (default 5%), `--rounds N` (default 6), `--repetitions N` (per binary per round, default 1), `--filter REGEX`, `--pinned CORES`, `--memory-cap GIB` (default half of RAM, 0 for none), `--baseline FILE`, `--build-dir DIR`, `--no-build`.

Pin to a range of cores, not one: the TaskManager benchmarks need worker threads (#120). A comparison runs the suite twice per round, so use `--filter` to pick the benchmarks your change touches. The run prints the machine, kernel, CPU, governor, compiler and both commits, and warns when the CPU governor or missing real-time privileges make the numbers noisy. Save a new baseline after any toolchain, kernel or hardware change.

Each run's address space is capped, at half of RAM by default. Many benchmarks keep what every iteration creates until they end, and run more iterations the faster they get, so a benchmark that keeps more than expected fails with exit 2 instead of pushing the machine into swap. Use `--memory-cap 0` with a sanitizer build, which reserves a huge address space up front.

### Performance Results (Release Build)

The benchmarks measure the engine with note and debug logging off (#121). Logging is a real cost in
production; it is simply not what these numbers are for. For the queue, order book, state machine,
arena, pool, NUMA, field-layout and FIX gateway microbenchmarks, run the binary:
`./build/orderProcessorBench --benchmark_list_tests` shows everything available, and
`scripts/benchmark-regression.sh` compares two builds (above).

`BM_ProcessNewOrder`, `BM_ProcessNewOrderWithMatch` and `BM_ProcessCancelOrder` drive a real `Processor`
through the state machine, `OrderDataStorage::save`, the book and the transaction, executed inline, and
`BM_TaskManagerThroughput/N` runs batches of orders through `TaskManager` with N event and N transaction
processors (#162). This branch's `BM_ProcessSwapWithMatch` times a swap that crosses: OrderMatcher's swap
matching, and the deferred events that report and book each leg. No `OrderSaver` is attached, so nothing
reaches LMDB.

---

## Architecture

```
                            ┌──────────────────────────────┐
                            │      React Frontend          │
                            │  Orders│Book│Exec│Positions   │
                            │  Dashboard│Memory│Alerts      │
                            └───────────┬──────────────────┘
                                        │ WebSocket (JSON)
                            ┌───────────▼──────────────────┐
                            │      WsServer (Beast)         │
                            │  SessionMgr │ MetricsPublisher│
                            └───────────┬──────────────────┘
                                        │
  IncomingQueues → Processor → OrderStateMachine → OrderMatcher/OrderStorage → OutgoingQueues
                      ↓
                TransactionManager (ACID)
                      ↓
                LMDBStorage (persistence)
```

### Core Components

| Component | Description |
|-----------|-------------|
| **Processor** | Central event handler and coordinator |
| **OrderStateMachine** | Boost MSM-based order lifecycle (14 states, 47+ transitions) |
| **OrderMatcher** | Price-time priority matching engine |
| **OrderBook** | Price-sorted buy/sell order sides per instrument |
| **TransactionManager** | ACID transaction coordination with dependency tree |
| **TransactionScopePool** | Lock-free CAS object pool for zero-allocation hot path |
| **TaskManager** | oneTBB-based parallel task scheduling with CPU pinning |
| **LMDBStorage** | LMDB-backed persistent key-value storage with versioning |
| **MetricsPublisher** | Periodic system metrics broadcast over WebSocket |
| **WsServer** | Boost Beast WebSocket server with per-session strands |

### Concurrency

| Component | Implementation | Benefit |
|-----------|---------------|---------|
| **IncomingQueues** | `tbb::concurrent_queue` + `std::variant` | Lock-free MPMC event ingestion |
| **TransactionScopePool** | CAS ring buffer with exponential backoff | Zero-allocation transaction processing |
| **OrderStorage** | `tbb::spin_rw_mutex` + `tbb::concurrent_hash_map` | Concurrent lookups, fine-grained access |
| **WideDataStorage** | `tbb::spin_rw_mutex` | Concurrent reads for reference data |
| **CacheAlignedAtomic** | `alignas(64)` wrapper | Prevents false sharing on contended atomics |
| **SessionManager** | `tbb::spin_rw_mutex` | Thread-safe broadcast to WebSocket clients |

### Memory Allocation Tiers

| Tier | Mechanism | When Used |
|------|-----------|-----------|
| 1. Stack | `Context` struct, local vars | Always — pointers to shared components |
| 2. Arena | 2KB bump allocator per TransactionScope | Operations within a transaction (~12 ops fit) |
| 3. Pool | 1024-slot CAS ring buffer | TransactionScope lifecycle — zero-alloc acquire/release |
| 4. TBB scalable | `tbb::scalable_allocator<T>` | NLinkedTree node allocation |
| 5. Heap | `::operator new` fallback | Pool exhaustion (tracked via `cacheMisses()`) |

---

## WebSocket Protocol

The server communicates with clients via JSON messages over WebSocket. All messages have a `type` field for dispatch. A server message's payload is in a `data` field, except `error`'s, whose text is a top-level `message`. A client's `new_order`, `cancel_order` and `replace_order` put theirs in `data` too, `{"type": "new_order", "data": {...}}`, while `subscribe_book` and `unsubscribe_book` give `symbol` at the top level.

### Connecting

A client opens a WebSocket on `/ws` and offers the subprotocol `oms-v1`. Through the Docker stack, nginx serves `/ws` over TLS only, so the client opens `wss://<host>:3000/ws` (#226). When the server has an access token (#146), the client offers it too, as a second subprotocol, `oms-token.<token>`: a browser can't set a WebSocket's headers, but it can name subprotocols. The server names `oms-v1` in its answer when the client offered it, never the token's. It refuses an upgrade without the right token with 401, one whose `Origin` isn't among `--allowed-origin` and `OMS_ALLOWED_ORIGINS` (when either is set) with 403, and a request that isn't a WebSocket upgrade with 426. Anyone with the token may still do everything the protocol allows, on any account: authorization is #225.

### Server → Client

| Type | Payload | When Sent |
|------|---------|-----------|
| `connected` | — | On WebSocket handshake |
| `instrument_list` | `Instrument[]` | On connect |
| `account_list` | `Account[]` | On connect |
| `order_snapshot` | `Order[]` | On connect (all current orders) |
| `order_update` | `Order` | On any order state change |
| `execution_report` | `ExecutionReport` | On trade, reject, cancel, replace, correct |
| `book_update` | `OrderBookSnapshot` | On book change (subscribed symbols only) |
| `cancel_reject` | `{orderId, reason}` | When a cancel is refused; `reason` says why: unknown order, too late (with the order's status), or a replace pending |
| `business_reject` | `{refId, reason}` | Not sent: nothing in the server raises one |
| `metrics_update` | `SystemMetrics` | Every 1 second (broadcast to all) |
| `error` | `{message}` | On server error |

### Client → Server

| Type | Payload | Description |
|------|---------|-------------|
| `new_order` | `NewOrderRequest` | Submit a new order. `side`, `ordType`, `tif`, `currency` and `capacity` are required, and must be values the server knows: an order without one gets an `error`, and nothing is stored (#166). `orderQty` is required too, a whole number from 1 (#187), and a `LIMIT` or `STOPLIMIT` order's `price` above 0 (#188) |
| `new_swap_order` | `NewSwapOrderRequest` | Submit an FX swap: `symbol`, `side`, `orderQty`, `nearPrice`, `farPrice`, and `settlDate` and `farSettlDate` in ms since the epoch, the far one after the near one; `account`, `currency`, `capacity` and `tif` (GTC unless given) as for a new order. `side`, `currency` and `capacity` are required, and they and a `tif` must be values the server knows: a swap without one gets an `error`, and nothing is stored (#166). `orderQty` is required too, a whole number from 1 (#187) |
| `cancel_order` | `{orderId, clOrderId?}` | Cancel an existing order: answered with a `CANCELED` execution report, or a `cancel_reject`. With `clOrderId`, that names the order and `orderId` must match it. Without, `orderId` does; a number more than one order has (possible only in a data directory written before order numbers continued across restarts) gets an `error` asking for `clOrderId` |
| `replace_order` | `{orderId, clOrderId?, price?, orderQty?, tif?}` | Replace an existing order with one that has the given changes (an `orderQty` given must be from 1, #187, and a `price` above 0, #188): answered with two `REPLACE` execution reports, the original's (`REPLACED`) and the replacement's (`NEW`, or `PARTIAL_FILL` when the original has fills), whose new `orderId` names the original in `origOrderId`. A refused replace gets a `REJECT` report with `orderId` 0 saying why. `clOrderId` names the order as for `cancel_order`; a replace that changes nothing, or gives a `tif` the server doesn't know, gets an `error` |
| `subscribe_book` | `{symbol}` | Subscribe to order book updates for a symbol the server trades. Another symbol gets an `error` |
| `unsubscribe_book` | `{symbol}` | Unsubscribe from order book updates |

A client's message may be at most <!-- DOC_CHECK:ws_max_client_message_kib -->64<!-- /DOC_CHECK --> KiB; a larger one ends its session. What the server holds for clients is bounded too: `--max-connections` clients, and up to `--max-session-queue-mb` waiting for each.

Every order clears through the server's clearing firm (`--clearing-firm`), and an order with no `account` goes to the server's default account (`--default-account`). `seedData` seeds both. If either is missing from the data directory, the server logs a warning at startup and answers each order that needs it with an `error` message.

### Key Data Types

**Order** — 20+ fields including: `orderId`, `clOrderId`, `symbol`, `side`, `ordType`, `price`, `stopPx`, `avgPx`, `orderQty`, `cumQty`, `leavesQty`, `status`, `tif`, `capacity`, `currency`, `account`, `creationTime`, `lastUpdateTime`, and on an FX swap `settlDate`, `farPrice` and `farSettlDate`.

**ExecutionReport** — Base fields: `execId`, `orderId`, `type`, `orderStatus`, `market`, `transactTime`. Type-specific fields: `lastQty`/`lastPx` (trade), `rejectReason` (reject, and a rejected replace), `origOrderId` (replace/correct), `execRefId` (trade cancel/correct), and `execLegType` (`NEAR` or `FAR`) on an FX swap's fill, which is reported once per leg. An order cancel carries only the base fields. A report with `orderId` 0 is for an order the server refused without storing it, such as one whose ClOrdID is already in use; `rejectReason` says why.

**SystemMetrics** — 19 fields: event/transaction counters (created/processed/finished), processor availability, `queueDepth`, `poolSize`, `poolCacheMisses`, `poolArenaSize`, `activeSessions`, `activeOrders`, `storageMapSize` and `storageMapUsed` (LMDB's map and how much of it is in use, in bytes), `timestamp`.

**Enums** — `Side` (BUY, SELL, BUY_MINUS, SELL_PLUS, SELL_SHORT, CROSS), `OrderType` (MARKET, LIMIT, STOP, STOPLIMIT, FXSWAP), `TimeInForce` (DAY, GTD, GTC, FOK, IOC, OPG, ATCLOSE), `OrderStatus` (<!-- DOC_CHECK:order_status_count -->12<!-- /DOC_CHECK --> statuses), `ExecType` (<!-- DOC_CHECK:exec_type_count -->13<!-- /DOC_CHECK --> types), `Currency` (USD, EUR, GBP, JPY, CHF, AUD, CAD, NZD), `Capacity` (<!-- DOC_CHECK:capacity_count -->6<!-- /DOC_CHECK --> types).

---

## Monitoring

The MetricsPublisher runs on the server's ASIO io_context thread and broadcasts system metrics every second to all connected WebSocket clients. The React frontend provides three monitoring views:

### Dashboard
- **Summary cards:** Active sessions, active orders, queue depth, pool cache misses, events/sec, transactions/sec
- **Throughput charts:** Event and transaction processing rates as time-series line charts
- **Queue depth chart:** Event queue depth over time as area chart
- **Processor utilization:** Progress bars showing busy/total for event and transaction processors

### Memory Admin
- **Pool health indicator:** Green (0 cache misses in 60s), Yellow (1-5), Red (>5)
- **Pool overview:** Pool size (1024 slots), arena size (2048 bytes), allocation mode, cumulative cache misses
- **Cache miss rate chart:** Misses per second over time — non-zero indicates pool exhaustion and heap fallback
- **Arena details:** Operation capacity (~12 per scope), CAS backoff strategy, false-sharing prevention

### Alerts
- **Configurable rules:** Metric, operator, threshold, severity (INFO/WARNING/CRITICAL)
- **Default rules:** Pool cache misses > 0 (WARNING), queue depth > 100 (WARNING), queue depth > 500 (CRITICAL)
- **Alert history:** Timestamped alerts with severity coloring and acknowledge workflow
- **Persistence:** Rules saved to localStorage; 30-second cooldown per rule to prevent spam
- **Critical banner:** Red banner at top of page when unacknowledged critical alerts exist

---

## PostgreSQL Integration

With `BUILD_PG=ON` (libpqxx 8) and a libpq connection string, from `OMS_PG_CONNECTION` or `--pg-connection`, the server mirrors to Postgres what it keeps in LMDB (#151): its instruments, accounts and clearing firms, and every version of every order. LMDB stays the server's record. Postgres is a copy for queries and reports, written behind it on a thread of its own, so nothing waits for it. Without a connection string the server keeps LMDB only, as it always did.

- **When:** each save is queued as LMDB takes it, and the writer writes them in order, in transactions of up to 256. At startup the server first queues everything its data directory holds, which LMDB's load restores without saving. A rollback takes an order's row back to the version LMDB kept, or removes it if LMDB kept none.
- **Failures:** a write Postgres refuses is logged and counted, and the rest of its transaction's writes land. A lost connection is retried, with the same writes in the same order. Connecting gives up after `connect_timeout`, 5 seconds unless the string sets one, and at shutdown what can't be written then is dropped, counted and logged.
- **While Postgres is down:** a write of a row that is still queued replaces the queued one, in its place (#231). Each write sets a whole row or deletes it, so only the newest matters. The queue then holds at most one write per row changed during the outage, however many changes there were, and reference data still lands before the orders that name it.
- **What it holds:** a write can reach Postgres before LMDB commits it, so after a crash Postgres may hold a version LMDB lost. The next start sends what LMDB holds again, but doesn't delete such a row. Executions aren't mirrored, as LMDB doesn't keep them either, and nothing uses the `users` and `audit_log` tables yet (#225).
- **Schema:** `docker/init-db/001_schema.sql`. `PGSchemaTest` checks that every enum string the writer sends is one of its type's values.
- **Tests:** `scripts/pg-for-tests.sh start NAME` starts a throwaway Postgres with the schema and prints its connection string; with it in `PG_CONNECTION_STRING`, `ctest -R 'PGWriteBehind|Postgres'` runs the writer's tests. CI runs them so, in the Release and sanitizer jobs.
- **Docker:** compose builds the server with `BUILD_PG=ON` and points it at `oms-db` (docker/README.md)

---

## Project Structure

```
OrderProcessor/
├── src/                    # Library source
│   ├── Processor.cpp/h     # Central event coordinator
│   ├── StateMachine.cpp/h  # Boost MSM order state machine
│   ├── OrderMatcher.cpp/h  # Price-time priority matching engine
│   ├── OrderBookImpl.cpp/h # Order book (buy/sell sides)
│   ├── TransactionMgr.cpp/h # ACID transaction management
│   ├── TransactionScopePool.h  # Lock-free object pool
│   ├── LMDBStorage.cpp/h   # LMDB persistent key-value backend
│   ├── NumaAllocator.h     # NUMA-aware memory allocation
│   ├── CacheAlignedAtomic.h # Cache-aligned atomic wrapper
│   ├── CpuAffinity.h       # CPU thread pinning utilities
│   ├── HugePages.h         # Huge page allocation utilities
│   └── oms-frontend/       # React TypeScript frontend
│       ├── src/App.tsx      # Main app with 6 tabs
│       ├── src/components/  # UI components
│       ├── src/hooks/       # WebSocket state + alerts hooks
│       └── src/types/       # TypeScript type definitions
├── test/                   # Google Test unit tests (33 files, 458 tests)
│   └── mocks/              # Mock objects for testing
├── bench/                  # Google Benchmark performance tests
├── app/                    # WebSocket server application
│   ├── main.cpp            # Server entry point
│   ├── WsServer.cpp/h      # Boost Beast WebSocket server
│   ├── WsSession.cpp/h     # Per-client session with strand
│   ├── WsAuth.h            # Who may open a session: access token and Origins (#146)
│   ├── JsonSerializer.cpp/h # JSON encoding/decoding
│   ├── MetricsPublisher.cpp/h # Periodic system metrics broadcast
│   ├── SessionManager.cpp/h # Thread-safe session registry + broadcast
│   ├── WsOutQueues.cpp/h   # Execution event → WebSocket bridge
│   └── seed_data.cpp       # Test data generator
├── docker/                 # Docker deployment
│   ├── docker-compose.yml  # PostgreSQL + C++ server + React frontend
│   ├── Dockerfile.oms-server   # Multi-stage C++ build (CentOS Stream 10)
│   ├── Dockerfile.oms-frontend # Multi-stage React build (Node 24 + nginx)
│   ├── nginx.conf          # Frontend proxy config (serves :3000, proxies /ws)
│   ├── init-db/            # PostgreSQL schema initialization
│   └── .env.example        # PostgreSQL credentials and the access token, a template
├── scripts/                # Tooling
│   └── benchmark-regression.sh  # Before/after benchmark comparison
├── docs/                   # Architecture documentation
│   └── ARCHITECTURE.md     # Comprehensive architecture document
├── CMakeLists.txt          # Root build configuration
├── AUDIT_REPORT.md         # Code audit report
└── README.md               # This file
```

---

## Migration History

This project was migrated from:
- Windows (MSVC 2005) → CentOS 10 (GCC/C++23)
- Boost 1.39 → Boost 1.83+
- TBB 2.1 → oneTBB 2021.x
- Boost Logging v2 → spdlog
- Custom test framework → Google Test/Benchmark

## License

This project is licensed under the GNU Affero General Public License v3.0. See [LICENSE](LICENSE) for full terms.
