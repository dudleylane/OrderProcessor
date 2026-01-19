# OrderProcessor

A high-performance, concurrent order processing library written in C++23. Provides ACID-compliant transaction processing, state machine-based order lifecycle management, and order matching against an in-memory order book.

**Author:** Sergey Mikhailik
**License:** GNU General Public License (GPL)

## Features

- **ACID Transactions:** Full transaction support with rollback capability
- **State Machine:** 20+ order states managed via Boost Meta State Machine (MSM)
- **Order Matching:** Price-time priority matching engine with order book
- **Concurrent Design:** Lock-free caching, thread-safe queues, parallel task execution
- **Persistence:** File-based storage with versioning and codecs

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

Install on CentOS 10:
```bash
sudo dnf install tbb-devel spdlog-devel boost-devel cmake gcc-c++
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

### Build Outputs

- **Library:** `build/liborderEngine.a`
- **Tests:** `build/orderProcessorTest`
- **Benchmarks:** `build/orderProcessorBench`

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

Baseline performance on 8-core CPU @ 3.7 GHz:

| Benchmark | Time (ns) | Iterations |
|-----------|-----------|------------|
| EventProcessing | 0.54-0.58 | 1,000,000,000 |
| OrderMatching | 0.54-0.57 | 1,000,000,000 |
| StateMachineTransitions | 0.55-0.62 | 1,000,000,000 |
| InterlockCache | 0.55-0.58 | 1,000,000,000 |

See `benchmark_results.json` for detailed results.

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
| **TaskManager** | oneTBB-based parallel task scheduling |
| **InterLockCache** | Wait-free object caching |

## Project Structure

```
OrderProcessor/
├── src/                    # Library source files
│   ├── Processor.cpp       # Main event processor
│   ├── StateMachine.cpp    # Order state machine
│   ├── OrderMatcher.cpp    # Matching engine
│   ├── OrderBookImpl.cpp   # Order book implementation
│   ├── TransactionMgr.cpp  # Transaction management
│   ├── FileStorage.cpp     # Persistence layer
│   └── ...
├── test/                   # Google Test unit tests
├── bench/                  # Google Benchmark performance tests
├── CMakeLists.txt          # Build configuration
└── CLAUDE.md               # Developer documentation
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
