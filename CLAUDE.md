# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OrderProcessor is a high-performance, concurrent order processing library written in C++23. It provides ACID-compliant transaction processing, state machine-based order lifecycle management, and order matching against an in-memory order book.

## Build System

**Build Tool:** CMake 3.21+
**C++ Standard:** C++23
**Platform:** CentOS 10 / Linux (GCC)

### Required Dependencies

Install these before building:
- **oneTBB** - Intel Threading Building Blocks (concurrent containers, task scheduling)
- **spdlog** - Fast C++ logging library
- **Boost** - Headers only (for MPL, optional helpers)

On CentOS 10:
```bash
sudo dnf install tbb-devel spdlog-devel boost-devel
```

### Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

CMake Options:
- `-DBUILD_TESTS=ON` (default) - Build unit tests with Google Test
- `-DBUILD_BENCHMARKS=ON` (default) - Build benchmarks with Google Benchmark

### Build Outputs

- **Library:** `build/liborderEngine.a`
- **Test Executable:** `build/orderProcessorTest`
- **Benchmark Executable:** `build/orderProcessorBench`

### Running Tests

```bash
cd build
ctest --output-on-failure
```

Or run directly:
```bash
./orderProcessorTest
```

### Running Benchmarks

```bash
./orderProcessorBench --benchmark_format=console
```

## Architecture

### Event Processing Pipeline

```
IncomingQueues → Processor → OrderStateMachine → OrderMatcher/OrderStorage → OutgoingQueues
                    ↓
              TransactionManager (ACID)
                    ↓
              FileStorage (persistence)
```

### Core Modules

| Module | Location | Purpose |
|--------|----------|---------|
| **Queues** | `src/IncomingQueues.cpp`, `src/OutgoingQueues.cpp` | Lock-free event queues (MPMC) |
| **Processor** | `src/Processor.cpp` | Main event handler, central entry point |
| **State Machine** | `src/StateMachine.cpp`, `src/OrderStateMachineImpl.cpp` | Order state management (14 states, 47+ transitions) |
| **OrderMatcher** | `src/OrderMatcher.cpp` | Order matching logic |
| **OrderBook** | `src/OrderBookImpl.cpp` | Price-sorted buy/sell sides |
| **Transaction** | `src/TransactionMgr.cpp`, `src/TransactionScope.cpp` | ACID transaction coordination |
| **Storage** | `src/FileStorage.cpp`, `src/StorageRecordDispatcher.cpp` | Persistence with versioning |
| **TaskManager** | `src/TaskManager.cpp` | oneTBB-based parallel task scheduling |
| **Logger** | `src/Logger.cpp` | spdlog-based logging |

### Supporting Modules

| Module | Location | Purpose |
|--------|----------|---------|
| **Codecs** | `src/*Codec.cpp` | Serialization: `AccountCodec`, `ClearingCodec`, `InstrumentCodec`, `OrderCodec`, `RawDataCodec`, `StringTCodec` |
| **Filters** | `src/*Filter.cpp` | Order filtering: `EntryFilter`, `OrderFilter`, `FilterImpl` |
| **Data Storage** | `src/OrderStorage.cpp`, `src/WideDataStorage.cpp` | Runtime order and reference data storage |
| **Deferred Events** | `src/*DeferedEvent.cpp` | Async event handling: `CancelOrderDeferedEvent`, `ExecutionDeferedEvent`, `MatchOrderDeferedEvent` |
| **Subscriptions** | `src/SubscriptionLayerImpl.cpp`, `src/SubscrManager.cpp` | Event subscription management |
| **Utilities** | `src/IdTGenerator.cpp`, `src/ExchUtils.cpp`, `src/AllocateCache.cpp` | ID generation, exchange utilities, memory caching |
| **State Definitions** | `src/OrderStates.cpp`, `src/TrOperations.cpp` | Order states and transaction operations |
| **Queue Management** | `src/QueuesManager.cpp`, `src/EventManager.cpp` | Queue coordination and event dispatch |
| **Type Definitions** | `src/DataModelDef.cpp` | Data model definitions |

### Key Entry Point

The `Processor` class (`src/Processor.cpp`) is the central coordinator:
```cpp
class Processor: public InQueueProcessor,
                 public DeferedEventContainer,
                 public TransactionProcessor
```

### Supported Event Types

The Processor handles the following incoming events (defined in `src/QueuesDef.h`):

| Event Type | Purpose | Key Fields |
|------------|---------|------------|
| `OrderEvent` | New order submission | `order_` - pointer to OrderEntry |
| `OrderCancelEvent` | Cancel existing order | `id_` - order to cancel, `cancelReason_` |
| `OrderReplaceEvent` | Replace/modify order | `id_` - original order, `replacementOrder_` - new order |
| `OrderChangeStateEvent` | External state change | `id_` - order, `changeType_` (SUSPEND/RESUME/FINISH) |
| `TimerEvent` | Time-based triggers | `id_` - order, `timerType_` (EXPIRATION/DAY_END/DAY_START) |
| `ProcessEvent` | Internal processing | `type_` - event subtype, `id_` - related order |

### Concurrency Model

- **std::atomic:** Lock-free atomics for counters and flags
- **oneapi::tbb::concurrent_queue:** Lock-free MPMC queue for event ingestion (`IncomingQueues`)
- **oneapi::tbb::concurrent_hash_map:** Fine-grained bucket-level locking (`OrderStorage` executions)
- **oneapi::tbb::spin_rw_mutex:** Reader-writer lock for read-heavy data (`WideDataStorage`, `OrderStorage` orders)
- **oneapi::tbb::mutex:** Mutual exclusion for shared data
- **oneapi::tbb::spin_mutex:** Lightweight locking for short critical sections
- **oneapi::tbb::task_group:** Task parallelism in TaskManager
- **InterLockCache:** Wait-free object caching (`src/InterLockCache.h`)
- **NLinkedTree:** Transaction dependency ordering (`src/NLinkedTree.cpp`)

### Lock-Free Components

| Component | Implementation | Operations |
|-----------|---------------|------------|
| **IncomingQueues** | `tbb::concurrent_queue` + `std::variant` | `push()`, `pop()`, `size()` are lock-free |
| **OutgoingQueues** | `tbb::concurrent_queue` + `std::variant` | `push()` is lock-free |
| **WideDataStorage** | `tbb::spin_rw_mutex` | Concurrent `get()` reads, exclusive `add()`/`restore()` writes |
| **OrderStorage** | `tbb::spin_rw_mutex` + `tbb::concurrent_hash_map` | Concurrent order lookups; fine-grained execution access |
| **InterLockCache** | CAS-based circular buffer | Wait-free `popFront()`, `pushBack()` |
| **IdTGenerator** | `std::atomic<u64>` | Lock-free monotonic ID generation |
| **Logger flags** | `std::atomic<char>` | Lock-free flag checking |

## Code Conventions

- **Member variables:** Trailing underscore (`varName_`)
- **Classes:** PascalCase (`ClassName`)
- **Namespaces:** `COP::SubModule::Component`
- **Type suffixes:** `_T` for templates, `_t` for typedefs

## Key Dependencies

- **oneTBB 2021.x+:** Lock-free concurrent_queue, mutex, spin_mutex, task scheduling
- **spdlog:** Logging infrastructure
- **Boost (headers):** MPL for Meta State Machine
- **Google Test:** Unit testing framework
- **Google Benchmark:** Performance benchmarking

## Test Structure

Tests are in `test/` using Google Test framework (24 test files):

### Core Tests
- `CodecsTest.cpp` - Codec encode/decode tests
- `IncomingQueuesTest.cpp` - Lock-free event queue tests
- `OutgoingQueuesTest.cpp` - Outgoing queue tests
- `InterlockCacheTest.cpp` - Lock-free cache tests
- `NLinkTreeTest.cpp` - Transaction tree tests
- `ProcessorTest.cpp` - Main processor tests
- `StateMachineTest.cpp` - State machine transitions
- `StatesTest.cpp` - Order state tests
- `OrderBookTest.cpp` - Order book operations
- `OrderMatcherTest.cpp` - Order matching tests
- `OrderStorageTest.cpp` - Order storage tests

### Transaction Tests
- `TransactionMgrTest.cpp` - Transaction manager tests
- `TransactionScopeTest.cpp` - Transaction scope tests
- `TrOperationsTest.cpp` - Transaction operations tests

### Storage Tests
- `FileStorageTest.cpp` - Persistence tests
- `StorageRecordDispatcherTest.cpp` - Storage dispatcher tests
- `WideDataStorageTest.cpp` - Wide data storage tests

### Other Tests
- `DeferedEventsTest.cpp` - Deferred event tests
- `FiltersTest.cpp` - Order filter tests
- `IdTGeneratorTest.cpp` - ID generator tests
- `QueuesManagerTest.cpp` - Queue manager tests
- `SubscriptionTest.cpp` - Subscription layer tests
- `TaskManagerTest.cpp` - Task manager tests
- `IntegrationTest.cpp` - Full integration tests

### Test Utilities
- `TestAux.cpp/h` - Test helper functions and mock contexts
- `StateMachineHelper.cpp/h` - State machine test helpers
- `TestFixtures.h` - Shared test fixtures

### Mock Objects (`test/mocks/`)
- `MockDefered.h` - Mock deferred event handlers
- `MockOrderBook.h` - Mock order book
- `MockQueues.h` - Mock queue implementations
- `MockStorage.h` - Mock storage layer
- `MockTasks.h` - Mock task manager
- `MockTransaction.h` - Mock transaction components

### Legacy Test Files

The `test/` directory contains 10 legacy test files (`test*.cpp`) with original test implementations. These files are retained for reference during migration to Google Test format.

Benchmarks in `bench/`:
- `EventProcessingBench.cpp` - Event queue throughput
- `OrderMatchingBench.cpp` - Order matching performance
- `StateMachineBench.cpp` - State transition performance
- `InterlockCacheBench.cpp` - Lock-free cache performance
