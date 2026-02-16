# OrderProcessor

A high-performance, concurrent order processing library written in C++23. Provides ACID-compliant transaction processing, state machine-based order lifecycle management, and order matching against an in-memory order book.

**Author:** Sergey Mikhailik
**License:** GNU General Public License (GPL)

## Features

- **ACID Transactions:** Full transaction support with rollback capability
- **State Machine:** 14 order states with 47+ transitions managed via Boost Meta State Machine (MSM)
- **Order Matching:** Price-time priority matching engine with order book
- **Concurrent Design:** Lock-free queues, reader-writer locks, concurrent hash maps, wait-free caching
- **Persistence:** File-based storage with versioning and codecs, LMDB key-value backend
- **WebSocket Server:** Production-ready WebSocket application with JSON serialization
- **NUMA Awareness:** NUMA-local memory allocation and CPU pinning for multi-socket systems

## Requirements

- **Platform:** Linux (CentOS 10 / RHEL 10 or compatible)
- **Compiler:** GCC 13+ with C++23 support
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
- **Server:** `build/orderProcessorServer` (WebSocket server, if `BUILD_APP=ON`)
- **Seed Data:** `build/seedData` (test data generator, if `BUILD_APP=ON`)

## Running Tests

```bash
cd build
ctest --output-on-failure
```

Or run directly:
```bash
./orderProcessorTest
```

Run specific test suite:
```bash
./orderProcessorTest --gtest_filter="CodecsTest.*"
```

## Running Benchmarks

```bash
./orderProcessorBench --benchmark_format=console
```

Export results to JSON:
```bash
./orderProcessorBench --benchmark_out=results.json --benchmark_out_format=json
```

### Performance Results (Release Build)

Benchmark results on 8-core CPU @ 3.8 GHz:

| Benchmark | Time (ns) | Throughput |
|-----------|-----------|------------|
| EventProcessing | ~7,200 | ~139K ops/sec |
| OrderMatching | ~7,300 | ~137K ops/sec |
| StateMachineTransitions | ~9,100 | ~110K ops/sec |
| InterlockCache | ~30-40 | ~25-33M ops/sec |
| IncomingQueues Push | ~15 | ~66M ops/sec |
| IncomingQueues Pop | ~15 | ~66M ops/sec |

Benchmarks measure complete operation cycles including state machine initialization, event processing, and storage operations.

**Ultra Low-Latency Optimizations (January 2026):**
- Lock-free TransactionScope object pooling eliminates heap allocations on hot path
- Cache line alignment (`alignas(64)`) prevents false sharing
- Relaxed memory ordering for statistics counters
- Exponential backoff with `_mm_pause()` on CAS contention
- NUMA-aware memory allocation for multi-socket locality
- CPU thread pinning via `CpuAffinity` utilities
- Huge page support for TLB efficiency

## Architecture

```
IncomingQueues → Processor → OrderStateMachine → OrderMatcher/OrderStorage → OutgoingQueues
                    ↓
              TransactionManager (ACID)
                    ↓
              FileStorage (persistence)
```

### Core Components

| Component | Description |
|-----------|-------------|
| **Processor** | Central event handler and coordinator |
| **OrderStateMachine** | Boost MSM-based order lifecycle management |
| **OrderMatcher** | Price-time priority matching engine |
| **OrderBook** | Price-sorted buy/sell order sides |
| **TransactionManager** | ACID transaction coordination |
| **TransactionScopePool** | Lock-free object pool for zero-allocation hot path |
| **TaskManager** | oneTBB-based parallel task scheduling |
| **InterLockCache** | Wait-free object caching |
| **LMDBStorage** | LMDB-backed persistent key-value storage |
| **NumaAllocator** | NUMA-aware memory allocation for multi-socket systems |

### Concurrency Features

| Component | Implementation | Benefit |
|-----------|---------------|---------|
| **IncomingQueues** | `tbb::concurrent_queue` + `std::variant` | Lock-free MPMC event ingestion |
| **OutgoingQueues** | `tbb::concurrent_queue` + `std::variant` | Lock-free event output |
| **WideDataStorage** | `tbb::spin_rw_mutex` | Concurrent reads for reference data |
| **OrderStorage** | `tbb::spin_rw_mutex` + `tbb::concurrent_hash_map` | Concurrent lookups, fine-grained execution access |
| **InterLockCache** | CAS-based circular buffer | Wait-free memory pooling |
| **TransactionScopePool** | Lock-free ring buffer with CAS | Zero-allocation transaction processing |
| **CacheAlignedAtomic** | `alignas(64)` wrapper | Prevents false sharing on contended atomics |
| **NumaAllocator** | `mbind()` + STL-compatible allocator | NUMA-local allocation reduces cross-socket latency |

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
│   ├── FileStorage.cpp/h   # Versioned file persistence layer
│   ├── PGWriteBehind.cpp/h # PostgreSQL write-behind (optional)
│   └── ...                 # Codecs, filters, subscriptions, utilities
├── test/                   # Google Test unit tests (33 test files, 463 tests)
│   └── mocks/              # Mock objects for testing
├── bench/                  # Google Benchmark performance tests (7 suites)
├── app/                    # WebSocket server application
│   ├── main.cpp            # Server entry point
│   ├── WsServer.cpp/h      # WebSocket server
│   ├── WsSession.cpp/h     # Per-client session management
│   ├── JsonSerializer.cpp/h # JSON encoding/decoding
│   └── seed_data.cpp       # Test data generator
├── docker/                 # Docker deployment
│   ├── docker-compose.yml  # Multi-service orchestration
│   ├── Dockerfile.oms-server   # C++ server image
│   └── Dockerfile.oms-frontend # React frontend image
├── docs/                   # Architecture documentation
│   └── ARCHITECTURE.md     # Comprehensive architecture document
├── data/                   # LMDB persistence data files
├── CMakeLists.txt          # Root build configuration
├── AUDIT_REPORT.md         # Comprehensive code audit report
└── README.md               # This file
```

## Migration History

This project was migrated from:
- Windows (MSVC 2005) → CentOS 10 (GCC/C++23)
- Boost 1.39 → Boost 1.83+
- TBB 2.1 → oneTBB 2021.x
- Boost Logging v2 → spdlog
- Custom test framework → Google Test/Benchmark

## License

This project is licensed under the GNU General Public License (GPL). See the source files for details.
