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
- **Order Entry:** Full form with symbol, side (<!-- DOC_CHECK:side_count -->6<!-- /DOC_CHECK --> types), order type (<!-- DOC_CHECK:order_type_count -->4<!-- /DOC_CHECK --> types: Market/Limit/Stop/StopLimit), time-in-force (<!-- DOC_CHECK:tif_count -->7<!-- /DOC_CHECK --> types), capacity, currency
- **Order Blotter:** Live table with inline Cancel and Replace actions
- **Execution Blotter:** Trades, rejects, corrections, cancels with CSV export
- **Order Book Panel:** 8-level bid/ask depth with staleness indicator and per-instrument subscription
- **Position Summary:** Client-computed net positions with VWAP

### Monitoring Dashboard
- **Real-Time Metrics:** MetricsPublisher broadcasts 17 system metrics at 1/sec over WebSocket
- **Dashboard Tab:** Event/transaction throughput charts, queue depth, processor utilization bars
- **Memory Admin Tab:** TransactionScopePool health indicator, cache miss rate trending, arena details
- **Alert System:** Client-side rule engine with configurable thresholds, severity levels, localStorage persistence

### Persistence
- **LMDB Storage:** Memory-mapped key-value store with versioned records. An order keeps its newest version and the one before it, and the map doubles when a write finds it full (#148)
- **PostgreSQL Write-Behind:** PGWriteBehind builds with `BUILD_PG=ON`, but nothing connects it to the server yet, so nothing reaches Postgres (#151)
- **File Storage:** Legacy versioned file persistence (deprecated in favor of LMDB)

### Deployment
- **Docker Compose:** 3-service stack — PostgreSQL 16, C++ server, React/nginx frontend
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

Install on CentOS 10:
```bash
sudo dnf install gcc-toolset-15 tbb-devel spdlog-devel boost-devel cmake lmdb-devel numactl-devel
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
| `BUILD_PG` | OFF | Build PostgreSQL write-behind layer (requires libpqxx); nothing connects it to the server yet (#151) |
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

Seed a new data directory first. Until then it has no instruments, accounts or clearing firm, and the server answers every order with an `error` (#161). `seedData` seeds test data, so don't run it on a directory in real use.

Server options:

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 8080 | WebSocket listen port |
| `--data-dir` | `./data` | LMDB persistence directory |
| `--workers` | 0 | TBB worker threads, and as many event and transaction processor pairs. 0 leaves TBB at its default, a thread per core, and runs 2 pairs |
| `--cpu-affinity` | -1 | Pin main thread starting from this core (-1 = disabled) |
| `--huge-pages` | off | Reports whether huge pages are available; nothing allocates from them yet |
| `--clearing-firm` | `HOUSE-CLEARING` | Clearing firm put on every order; must exist in the data directory |
| `--default-account` | `TRADING-1` | Account for an order that names none; must exist in the data directory |
| `--debug` | off | Debug logging: about six lines per transaction, written inside the transaction manager's lock |
| `--lmdb-map-mb` | 256 | LMDB's starting map, in MiB. A write that finds the map full doubles it, and logs a warning |
| `--max-connections` | <!-- DOC_CHECK:ws_max_connections -->256<!-- /DOC_CHECK --> | WebSocket clients connected at once. One more is closed as soon as it's accepted |
| `--max-session-queue-mb` | <!-- DOC_CHECK:ws_max_session_queue_mb -->16<!-- /DOC_CHECK --> | What may wait to be sent to one client, in MiB. A client that doesn't read is dropped once more waits, and gets the current state again when it reconnects |

An option the server doesn't know, an option without its value, or a number outside the option's range is a usage error: the server prints why and its usage on standard error, and exits with status 2 (#173). `--help` prints the usage.

The log goes to `exchange.log` in the working directory. The environment variable `EXCHANGE_LOG` sends it elsewhere: a file's path, or `-` for standard output, which the Docker image uses, so `docker compose logs` shows it.

### Docker Compose (Full Stack)

```bash
cd docker
cp .env.example .env        # Set PostgreSQL credentials
docker compose up --build -d
```

The first `up` seeds the new data volume with test data, so the stack takes orders at once: the server's container runs `seedData --if-empty` before the server (#161).

This starts 3 services:

| Service | Port | Description |
|---------|------|-------------|
| `oms-db` | 5432 | PostgreSQL 16 (Alpine) with schema initialization |
| `oms-server` | 8080 | C++ OrderProcessor WebSocket server |
| `oms-frontend` | 3000 | React UI served by nginx, proxies `/ws` to server |

Open **http://localhost:3000** to access the OMS frontend.

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

The app connects to `/ws` on the host and port it was loaded from, and the dev server passes that on to the server at `ws://localhost:8080/ws`. The image's nginx does the same for the server's container. To point a build somewhere else, set `VITE_WS_URL` when building.

---

## Testing

```bash
cd build && ctest --output-on-failure
```

`ctest -N` lists every registered test. Run a specific suite:
```bash
./orderProcessorTest --gtest_filter="ProcessorTest.*"
```

### Local pre-push gate

A tracked hook runs the build, `ctest -j1 --timeout 300`, and the clang-format check before every push, and refuses the push if any fails. It builds the working tree, so it also refuses a push unless the working tree is the commit pushed: `HEAD`, with nothing changed or untracked. Its build directory must be a Release one configured as CI configures it (`BUILD_TESTS`, `BUILD_BENCHMARKS` and `BUILD_APP` on). Enable it once per clone:

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

Every row names the benchmark that produces it, so you can reproduce it:
`./build/orderProcessorBench --benchmark_filter=BM_ProcessNewOrder`.

Measured on the dev box (4 cores / 8 threads, Release, `-O3 -march=native -flto`). Your machine will
differ; these numbers are a shape, not a specification.

| Benchmark | Time | Throughput | What it covers |
|-----------|------|------------|----------------|
| `BM_ProcessNewOrder` | ~2.4 µs | ~410K orders/sec | `Processor::onEvent` through the state machine, `OrderDataStorage::save`, the book, and the transaction, executed inline |
| `BM_ProcessNewOrderWithMatch` | ~4.9 µs | ~205K orders/sec | the same, for an order that crosses: trade, deferred execution events, book removal |
| `BM_ProcessCancelOrder` | ~1.6 µs | ~635K orders/sec | cancel of a resting order, completed: decided after the order's earlier transactions, removed from the book, reported |
| `BM_TaskManagerThroughput/4` | ~1.9 ms per 500 orders | ~265K orders/sec | end to end through `TaskManager` with 4 event and 4 transaction processors |

`BM_TaskManagerThroughput` takes the processor count as its argument: ~205K orders/sec at 2, ~265K at
4, and little more (~270K) at 8 on a 4-core machine.

Two things these numbers deliberately exclude, both real in production:

- **Logging.** The benchmarks turn notes and debug logging off. `app/main.cpp` turns notes on; with
  them on, a new order cost about 27 µs when #25 measured it, against 2.4 µs without, so roughly 24 µs
  per order goes into spdlog. The server's debug logging is off unless `--debug` asks for it (#76).
  With it on, every transaction formats about five more lines inside the transaction manager's lock,
  and every removal from the book one more. Before #76 the rows above included them; in the same
  session a match then took ~10.4 µs, a cancel ~3.8 µs, and `BM_TaskManagerThroughput/4` ~5.9 ms per
  500 orders.
- **Persistence.** No `OrderSaver` is attached, so nothing reaches LMDB. With the dispatcher wired
  up, each order change costs an LMDB write, about 2 ms with fsync per commit (issue #20).

And one thing they include that production doesn't: the benchmark's `OutgoingQueues` keeps a copy of
every report until the run ends, where production's queues keep none. That is about 2% of the
instructions of a new order or a cancel.

For the queue, order book, state machine, arena, pool, NUMA and field-layout microbenchmarks, run the binary:
`./build/orderProcessorBench --benchmark_list_tests` shows everything available.

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
| `new_order` | `NewOrderRequest` | Submit a new order. `side`, `ordType`, `tif`, `currency` and `capacity` are required, and must be values the server knows: an order without one gets an `error`, and nothing is stored (#166). `orderQty` is required too, a whole number from 1 (#187) |
| `cancel_order` | `{orderId, clOrderId?}` | Cancel an existing order: answered with a `CANCELED` execution report, or a `cancel_reject`. With `clOrderId`, that names the order and `orderId` must match it. Without, `orderId` does; a number more than one order has (possible only in a data directory written before order numbers continued across restarts) gets an `error` asking for `clOrderId` |
| `replace_order` | `{orderId, clOrderId?, price?, orderQty?, tif?}` | Replace an existing order with one that has the given changes (an `orderQty` given must be from 1, #187): answered with two `REPLACE` execution reports, the original's (`REPLACED`) and the replacement's (`NEW`, or `PARTIAL_FILL` when the original has fills), whose new `orderId` names the original in `origOrderId`. A refused replace gets a `REJECT` report with `orderId` 0 saying why. `clOrderId` names the order as for `cancel_order`; a replace that changes nothing, or gives a `tif` the server doesn't know, gets an `error` |
| `subscribe_book` | `{symbol}` | Subscribe to order book updates for a symbol the server trades. Another symbol gets an `error` |
| `unsubscribe_book` | `{symbol}` | Unsubscribe from order book updates |

A client's message may be at most <!-- DOC_CHECK:ws_max_client_message_kib -->64<!-- /DOC_CHECK --> KiB; a larger one ends its session. What the server holds for clients is bounded too: `--max-connections` clients, and up to `--max-session-queue-mb` waiting for each.

Every order clears through the server's clearing firm (`--clearing-firm`), and an order with no `account` goes to the server's default account (`--default-account`). `seedData` seeds both. If either is missing from the data directory, the server logs a warning at startup and answers each order that needs it with an `error` message.

### Key Data Types

**Order** — 20+ fields including: `orderId`, `clOrderId`, `symbol`, `side`, `ordType`, `price`, `stopPx`, `avgPx`, `orderQty`, `cumQty`, `leavesQty`, `status`, `tif`, `capacity`, `currency`, `account`, `creationTime`, `lastUpdateTime`.

**ExecutionReport** — Base fields: `execId`, `orderId`, `type`, `orderStatus`, `market`, `transactTime`. Type-specific fields: `lastQty`/`lastPx` (trade), `rejectReason` (reject, and a rejected replace), `origOrderId` (replace/correct), `execRefId` (trade cancel/correct). An order cancel carries only the base fields. A report with `orderId` 0 is for an order the server refused without storing it, such as one whose ClOrdID is already in use; `rejectReason` says why.

**SystemMetrics** — 19 fields: event/transaction counters (created/processed/finished), processor availability, `queueDepth`, `poolSize`, `poolCacheMisses`, `poolArenaSize`, `activeSessions`, `activeOrders`, `storageMapSize` and `storageMapUsed` (LMDB's map and how much of it is in use, in bytes), `timestamp`.

**Enums** — `Side` (BUY, SELL, BUY_MINUS, SELL_PLUS, SELL_SHORT, CROSS), `OrderType` (MARKET, LIMIT, STOP, STOPLIMIT), `TimeInForce` (DAY, GTD, GTC, FOK, IOC, OPG, ATCLOSE), `OrderStatus` (<!-- DOC_CHECK:order_status_count -->12<!-- /DOC_CHECK --> statuses), `ExecType` (<!-- DOC_CHECK:exec_type_count -->13<!-- /DOC_CHECK --> types), `Currency` (USD, EUR), `Capacity` (<!-- DOC_CHECK:capacity_count -->6<!-- /DOC_CHECK --> types).

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

Write-behind persistence to PostgreSQL, built with `BUILD_PG=ON` (requires `libpqxx`). Nothing connects it to the server yet, so the server writes nothing to Postgres (#151).

- **PGWriteBehind:** Async write-behind from the order engine to PostgreSQL
- **PGRequestBuilder:** Maps C++ data structures to SQL statements
- **Schema:** Enum types (Side, OrderType, OrderStatus, TimeInForce, ExecType, Currency, Capacity) and tables matching the C++ data model
- **Docker:** `docker-compose.yml` includes a PostgreSQL 16 service with schema initialization via `docker/init-db/001_schema.sql`

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
├── test/                   # Google Test unit tests
│   └── mocks/              # Mock objects for testing
├── bench/                  # Google Benchmark performance tests
├── app/                    # WebSocket server application
│   ├── main.cpp            # Server entry point
│   ├── WsServer.cpp/h      # Boost Beast WebSocket server
│   ├── WsSession.cpp/h     # Per-client session with strand
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
│   └── .env.example        # PostgreSQL credentials template
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
