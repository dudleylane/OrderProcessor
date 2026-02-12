# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```bash
# Build (from repo root)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# Run all tests
cd build && ctest --output-on-failure

# Run a single test suite
./build/orderProcessorTest --gtest_filter="ProcessorTest.*"

# Run a single test case
./build/orderProcessorTest --gtest_filter="ProcessorTest.ProcessSingleNewOrder"

# Run benchmarks
./build/orderProcessorBench --benchmark_format=console
```

**Dependencies (CentOS 10):** `sudo dnf install tbb-devel spdlog-devel boost-devel cmake gcc-c++ lmdb-devel`

**CMake options:** `-DBUILD_TESTS=ON` (default), `-DBUILD_BENCHMARKS=ON` (default)

**Build outputs:** `build/liborderEngine.a`, `build/orderProcessorTest`, `build/orderProcessorBench`

## Architecture

### Event Processing Pipeline

```
IncomingQueues → Processor → OrderStateMachine → OrderMatcher/OrderStorage → OutgoingQueues
                    ↓
              TransactionManager (ACID)
                    ↓
              FileStorage (persistence)
```

**Processor** (`src/Processor.cpp`) is the central coordinator. Every event follows this pattern:
1. Create a stack-allocated `Context` struct with pointers to all shared components
2. Acquire a `PooledTransactionScope` from the pre-allocated pool (zero heap allocation)
3. Restore the order's persisted state machine state via `setPersistance()`
4. Process the event through the Boost MSM state machine
5. Persist the new state machine state back to the order
6. Create a Transaction from the scope and enqueue to TransactionManager
7. Drain and process any deferred events generated during the action

### Key Architectural Patterns

**State Machine Persistence:** The Boost MSM state machine is NOT persistent per-order. Instead, each order stores an `OrderStatePersistence` object. Before processing, the state machine is loaded from the order; after processing, the new state is saved back. See `src/StateMachine.h` for the transition table (45+ transitions across 14 states using `mpl::vector45`).

**Deferred Event Chaining:** State machine actions can call `addDeferedEvent()` to queue follow-up processing (e.g., trade executions generating settlement events). After the primary transaction is enqueued, `processDeferedEvent()` drains these recursively — a deferred event may itself generate more deferred events.

**Context Pattern:** Every transaction gets a stack-allocated `Context` (`src/TransactionDef.h`) containing pointers to all shared components (OrderStorage, OrderBook, queues, matcher, ID generator, deferred events). This avoids globals and passes through all state machine actions.

**Event Queue:** A single `tbb::concurrent_queue` holds all 6 event types as `std::variant<OrderEvent, OrderCancelEvent, ...>`. Dispatching uses `std::visit()` to call the correct `onEvent()` overload.

### Zero-Allocation Hot Path

The critical performance optimization is `TransactionScopePool` (`src/TransactionScopePool.h`):
- Pre-allocates 1024 `TransactionScope` objects at startup
- CAS-based acquire/release with exponential backoff (`_mm_pause()`)
- `PooledTransactionScope` is RAII — auto-releases back to pool on destruction
- Falls back to heap allocation if pool is exhausted (tracked as "cache miss")
- `CacheAlignedAtomic<T>` (`src/CacheAlignedAtomic.h`) wraps atomics with `alignas(64)` to prevent false sharing

`InterLockCache` (`src/InterLockCache.h`) is a similar wait-free circular buffer cache using CAS for `popFront()`/`pushBack()`.

### Namespace Structure

```
COP::Queues     - InQueues, OutQueues, InQueueProcessor
COP::Proc       - Processor, OrderMatcher, DeferedEventContainer
COP::ACID       - TransactionManager, Transaction, Scope, TransactionScopePool
COP::Store      - OrderDataStorage
COP::OrdState   - OrderState_ (Boost MSM state machine), OrderStatePersistence
```

### Concurrency Model

- `tbb::concurrent_queue` — lock-free MPMC queues (IncomingQueues, OutgoingQueues)
- `tbb::concurrent_hash_map` — fine-grained bucket locking (OrderStorage executions)
- `tbb::spin_rw_mutex` — reader-writer lock for read-heavy data (WideDataStorage, OrderStorage orders)
- `tbb::spin_mutex` / `tbb::mutex` — mutual exclusion for short/longer critical sections
- `tbb::task_group` — task parallelism in TaskManager
- `InterLockCache` — wait-free object caching via CAS circular buffer
- `TransactionScopePool` — lock-free object pool with CAS ring buffer
- Statistics counters use `memory_order_relaxed`; synchronized data uses stronger ordering

## Code Conventions

- **Member variables:** trailing underscore (`varName_`)
- **Classes:** PascalCase (`ClassName`)
- **Namespaces:** `COP::SubModule::Component`
- **Type suffixes:** `_T` for templates, `_t` for typedefs
- **Low-latency:** All `alignas(64)` annotations are intentional false-sharing prevention — do not remove them
- **Classes marked `final`:** Enables compiler devirtualization — preserve when present

## Test Structure

Tests are in `test/` using Google Test. Test suite names match file names (e.g., `ProcessorTest`, `StateMachineTest`, `CodecsTest`). Mocks are in `test/mocks/`.

**Test fixture pattern:**
- `SetUp()` creates singletons (`WideDataStorage::create()`, `IdTGenerator::create()`, `OrderStorage::create()`)
- `TearDown()` destroys singletons in reverse order
- Use `TEST_F(SuiteName, TestName)` for fixture-based tests

**Test utilities:** `test/TestAux.cpp/h` (helper functions, mock contexts), `test/StateMachineHelper.cpp/h` (state machine test helpers), `test/TestFixtures.h` (shared fixtures).

Legacy test files (`test/test*.cpp`) are retained for reference during migration to Google Test format.

## Key Dependencies

- **oneTBB 2021.x+** — concurrent containers, mutex, spin_mutex, task scheduling
- **spdlog** — logging infrastructure
- **Boost (headers only)** — MPL for Meta State Machine (`BOOST_MPL_LIMIT_VECTOR_SIZE=50` is set in CMake)
- **Google Test/Mock** — fetched via CMake FetchContent (v1.14.0)
- **Google Benchmark** — fetched via CMake FetchContent (v1.8.3)
