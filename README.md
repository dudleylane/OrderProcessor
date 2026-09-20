# OrderProcessor

A high-performance, concurrent order processing engine written in C++23 with a React monitoring frontend. Provides ACID-compliant transaction processing, state machine-based order lifecycle management, order matching against an in-memory order book, and real-time system monitoring via WebSocket.

**Original Author:** Sergey Mikhailik
**Modernization & Extensions:** dudleylane, Claude
**License:** GNU General Public License v3 (AGPL-3.0)

## Features

### Core Engine
- **ACID Transactions:** Full transaction support with operation-level rollback on failure
- **State Machine:** 14 order states with 47+ transitions managed via Boost Meta State Machine (MSM)
- **Order Matching:** Price-time priority matching engine with per-instrument order books
- **Deferred Event Chaining:** Trade executions trigger cascading events (e.g., settlement)
- **Subscription Filtering:** Selective event delivery per client via SubscrManager

### Low-Latency Infrastructure
- **Zero-Allocation Hot Path:** 5-tier memory hierarchy — stack, arena (<!-- DOC_CHECK:arena_size -->2048<!-- /DOC_CHECK -->-byte bump), CAS pool (<!-- DOC_CHECK:pool_size -->1024<!-- /DOC_CHECK --> slots), TBB scalable allocator, heap fallback
- **Lock-Free Pool:** TransactionScopePool with exponential backoff CAS and cache miss tracking
- **CPU Affinity:** Thread-to-core pinning with SCHED_FIFO real-time scheduling
- **NUMA-Aware Allocation:** `mbind()` via NumaAllocator for multi-socket locality
- **Huge Pages:** 2MB page support for TLB efficiency
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
- **LMDB Storage:** Memory-mapped key-value store with versioned records
- **PostgreSQL Write-Behind:** Optional async persistence via PGWriteBehind (requires `BUILD_PG=ON`)
- **File Storage:** Legacy versioned file persistence (deprecated in favor of LMDB)

### Deployment
- **Docker Compose:** 3-service stack — PostgreSQL 16, C++ server, React/nginx frontend
- **Multi-Stage Dockerfiles:** CentOS Stream 10 build stage, minimal runtime images

---

## Requirements

- **Platform:** Linux (CentOS 10 / RHEL 10 or compatible)
- **Compiler:** GCC 13+ with C++23 support (GCC 15.2.1 recommended)
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
sudo dnf install tbb-devel spdlog-devel boost-devel cmake gcc-c++ lmdb-devel numactl-devel
```

---

## Building

```bash
git clone https://github.com/dudleylane/OrderProcessor.git
cd OrderProcessor
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
| `BUILD_PG` | OFF | Build PostgreSQL write-behind layer (requires libpqxx) |

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
./build/orderProcessorServer --port 8080 --data-dir ./data
```

Server options:

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 8080 | WebSocket listen port |
| `--data-dir` | `./data` | LMDB persistence directory |
| `--workers` | 0 | Worker thread count (0 = auto) |
| `--cpu-affinity` | -1 | Pin main thread starting from this core (-1 = disabled) |
| `--huge-pages` | off | Enable huge page allocation |

### Docker Compose (Full Stack)

```bash
cd docker
cp .env.example .env        # Set PostgreSQL credentials
docker compose up --build -d
```

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

The dev server proxies WebSocket connections to `ws://localhost:8080/ws` by default. Override with the `VITE_WS_URL` environment variable.

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

A tracked hook runs the build, `ctest -j1`, and the clang-format check before every push and refuses the push if any fails. Enable it once per clone:

```bash
git config core.hooksPath scripts/hooks
```

Bypass deliberately with `git push --no-verify` and record why in the commit message.

### Benchmarks

```bash
./orderProcessorBench --benchmark_format=console
```

Export to JSON for comparison:
```bash
./orderProcessorBench --benchmark_out=results.json --benchmark_out_format=json
```

### Comparing Benchmarks Before and After a Change

`scripts/benchmark-regression.sh` compares a run against a baseline you record yourself. A baseline is only meaningful on the machine that produced it, so none is committed.

```bash
# Record "before" on this machine, pinned to an isolated core
./scripts/benchmark-regression.sh --update-baseline --pinned 3

# Make your change, rebuild, then compare against it
./scripts/benchmark-regression.sh --no-build --pinned 3
```

It reports every benchmark that moved more than the threshold, and exits non-zero if any regressed.

Options: `--threshold N` (default 5%), `--repetitions N` (default 3), `--filter REGEX`, `--pinned CORES`, `--baseline FILE`, `--build-dir DIR`, `--no-build`.

The run prints the machine, kernel, CPU, governor, compiler and commit it measured, and warns when the CPU governor or missing real-time privileges make the numbers noisy. Re-record the baseline after any toolchain, kernel or hardware change.

### Performance Results (Release Build)

Every row names the benchmark that produces it, so you can reproduce it:
`./build/orderProcessorBench --benchmark_filter=BM_ProcessNewOrder`.

Measured on the dev box (4 cores / 8 threads, Release, `-O3 -march=native -flto`). Your machine will
differ; these numbers are a shape, not a specification.

| Benchmark | Time | Throughput | What it covers |
|-----------|------|------------|----------------|
| `BM_ProcessNewOrder` | ~2.4 µs | ~420K orders/sec | `Processor::onEvent` through the state machine, `OrderDataStorage::save`, the book, and the transaction, executed inline |
| `BM_ProcessNewOrderWithMatch` | ~11.5 µs | ~88K orders/sec | the same, for an order that crosses: trade, deferred execution events, book removal |
| `BM_ProcessCancelOrder` | ~1.1 µs | ~930K orders/sec | cancel of a resting order |
| `BM_TaskManagerThroughput/4` | ~8.8 ms per 500 orders | ~57K orders/sec | end to end through `TaskManager` with 4 event and 4 transaction processors |

`BM_TaskManagerThroughput` takes the processor count as its argument: ~30K orders/sec at 2, ~57K at
4, and no further gain at 8 on a 4-core machine.

Two things these numbers deliberately exclude, both real in production:

- **Logging.** The benchmarks turn notes off. `app/main.cpp` turns them on, and with them on a new
  order costs about 27 µs instead of 2.4 µs: roughly 24 µs per order inside spdlog.
- **Persistence.** No `OrderSaver` is attached, so nothing reaches LMDB. With the dispatcher wired
  up, each order change costs an LMDB write, about 2 ms with fsync per commit (issue #20).

For the queue, order book, state machine, arena and cache microbenchmarks, run the binary:
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
| **InterLockCache** | CAS-based circular buffer | Wait-free memory pooling |
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

The server communicates with clients via JSON messages over WebSocket. All messages have a `type` field for dispatch.

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
| `cancel_reject` | `{orderId, reason}` | When cancel request is rejected |
| `business_reject` | `{refId, reason}` | On business logic rejection |
| `metrics_update` | `SystemMetrics` | Every 1 second (broadcast to all) |
| `error` | `{message}` | On server error |

### Client → Server

| Type | Payload | Description |
|------|---------|-------------|
| `new_order` | `NewOrderRequest` | Submit a new order |
| `cancel_order` | `{orderId, clOrderId?}` | Cancel an existing order |
| `replace_order` | `{orderId, price?, orderQty?, tif?}` | Modify an existing order |
| `subscribe_book` | `{symbol}` | Subscribe to order book updates for a symbol |
| `unsubscribe_book` | `{symbol}` | Unsubscribe from order book updates |

### Key Data Types

**Order** — 20+ fields including: `orderId`, `clOrderId`, `symbol`, `side`, `ordType`, `price`, `stopPx`, `avgPx`, `orderQty`, `cumQty`, `leavesQty`, `status`, `tif`, `capacity`, `currency`, `account`, `creationTime`, `lastUpdateTime`.

**ExecutionReport** — Base fields: `execId`, `orderId`, `type`, `orderStatus`, `market`, `transactTime`. Type-specific fields: `lastQty`/`lastPx` (trade), `rejectReason` (reject), `origOrderId` (replace/correct), `execRefId` (cancel/correct).

**SystemMetrics** — 17 fields: event/transaction counters (created/processed/finished), processor availability, `queueDepth`, `poolSize`, `poolCacheMisses`, `poolArenaSize`, `activeSessions`, `activeOrders`, `timestamp`.

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

Optional write-behind persistence to PostgreSQL. Enable with `BUILD_PG=ON` (requires `libpqxx`).

- **PGWriteBehind:** Async write-behind from the order engine to PostgreSQL
- **PGRequestBuilder:** Maps C++ data structures to SQL statements
- **Schema:** Enum types (Side, OrderType, OrderStatus, TimeInForce, ExecType, Currency, Capacity) and tables matching the C++ data model
- **Docker:** `docker-compose.yml` includes a PostgreSQL 16 service with schema initialization via `docker/init-db/001_schema.sql`

---

## Project Structure

```
OrderProcessor/
├── src/                    # Library source (54 headers, 47 implementations)
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
├── bench/                  # Google Benchmark performance tests (17 suites)
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
│   ├── Dockerfile.oms-frontend # Multi-stage React build (Node 20 + nginx)
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
