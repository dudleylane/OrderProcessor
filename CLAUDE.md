# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OrderProcessor is a high-performance, concurrent order processing library written in C++ (C++03/C++98). It provides ACID-compliant transaction processing, state machine-based order lifecycle management, and order matching against an in-memory order book.

## Build System

**IDE:** Visual Studio 2005 (MSVC)
**Solution:** `OrderProcessor.sln`

### Required Environment Variables

Set these before building:
- `$(BOOST_INCLUDE)` - Path to Boost library (v1.39+)
- `$(TBB_HOME)` - Path to Intel TBB library (v2.1+)
- `$(MSM)` - Path to Meta State Machine library (v1.2+)

### Build Outputs

- **Library (Debug):** `lib/orderEngineD.lib`
- **Library (Release):** `lib/orderEngine.lib`
- **Test Executable:** `test/out/orderProcessorTest.exe`

### Running Tests

1. Build the OrderProcessorTest project
2. Copy DLLs to `test/out/`:
   - `boost_regex-vc80-mt-1_39.dll`
   - `tbb.dll`, `tbb_debug.dll`
   - `tbbmalloc.dll`, `tbbmalloc_debug.dll`
3. Run `test/out/orderProcessorTest.exe`

**Running single tests:** Modify the `tests[]` array in `test/orderProcessorTest.cpp` to comment out unwanted tests, then rebuild.

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
| **Queues** | `src/Queues/` | Thread-safe event input/output queues |
| **Processor** | `src/MatchingEngine/Processor.cpp` | Main event handler, central entry point |
| **State Machine** | `src/State/` | MSM-based order state management (20+ states) |
| **OrderMatcher** | `src/MatchingEngine/OrderMatcher.cpp` | Order matching logic |
| **OrderBook** | `src/DataModel/OrderBookImpl.cpp` | Price-sorted buy/sell sides |
| **Transaction** | `src/Transaction/` | ACID transaction coordination |
| **Storage** | `src/Storage/` | Persistence with versioning, codecs in `Storage/Codec/` |
| **TaskManager** | `src/Tasks/` | TBB-based parallel task scheduling |
| **SubscrManager** | `src/SubscriptionManager/` | Event subscription and filtering |

### Key Entry Point

The `Processor` class (`src/MatchingEngine/Processor.cpp`) is the central coordinator:
```cpp
class Processor: public InQueueProcessor,
                 public DeferedEventContainer,
                 public TransactionProcessor
```

### Concurrency Model

- **TBB containers:** `tbb::hash_map`, `tbb::mutex`, `tbb::atomic`
- **Wait-free cache:** `src/Base/InterLockCache.cpp`
- **Task parallelism:** TBB task scheduler in `TaskManager`
- **Transaction ordering:** `NLinkedTree` ensures dependency ordering

## Code Conventions

- **Member variables:** Trailing underscore (`varName_`)
- **Classes:** PascalCase (`ClassName`)
- **Namespaces:** `COP::SubModule::Component`
- **Type suffixes:** `_T` for templates, `_t` for typedefs

## Key Dependencies

- **Boost v1.39+:** Smart pointers, regex, templates
- **Intel TBB v2.1+:** Concurrent containers, task scheduling, atomics
- **MSM v1.2+:** Meta State Machine for order state management
- **Boost Logging Library v2:** Logging infrastructure

## Test Structure

Tests are in `test/` with a custom test framework (no external test library):
- `testProcessor.cpp` - Main processor tests
- `testStateMachine.cpp` - State machine transitions
- `testOrderBook.cpp` - Order book operations
- `testFileStorage.cpp` - Persistence tests
- `testIntegral.cpp` - Full integration tests
- `testEventBenchmark.cpp` - Performance benchmarks (disabled by default)

Helper utilities: `TestAux.cpp/h`, `StateMachineHelper.cpp/h`
