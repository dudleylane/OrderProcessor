# OrderProcessor Architecture Document

**Version:** 1.0
**Date:** January 2026
**System:** OrderProcessor - High-Performance Concurrent Order Processing Library

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [System Architecture Overview](#2-system-architecture-overview)
3. [Component Architecture](#3-component-architecture)
4. [Data Flow Diagrams](#4-data-flow-diagrams)
5. [State Machine Design](#5-state-machine-design)
6. [Concurrency Model](#6-concurrency-model)
7. [Transaction Management (ACID)](#7-transaction-management-acid)
8. [Storage & Persistence](#8-storage--persistence)
9. [Test Coverage Matrix](#9-test-coverage-matrix)
10. [Appendix: File Reference](#10-appendix-file-reference)

---

## 1. Executive Summary

OrderProcessor is a high-performance, concurrent order processing library written in C++23. It provides:

- **ACID-compliant** transaction processing
- **State machine-based** order lifecycle management (<!-- DOC_CHECK:order_state_count -->13<!-- /DOC_CHECK --> states, <!-- DOC_CHECK:transition_count -->49<!-- /DOC_CHECK --> transitions)
- **Order matching** against an in-memory order book with price-time priority
- **Lock-free** and fine-grained locking for maximum throughput
- **File-based persistence** with versioned records

### Key Technologies

| Technology | Purpose |
|------------|---------|
| C++23 | Core language standard |
| Intel oneTBB | Concurrent containers, task scheduling |
| Boost.MSM | Meta State Machine for order lifecycle |
| spdlog | High-performance logging |
| Google Test | Unit and integration testing |
| Google Benchmark | Performance benchmarking |

### Ultra Low-Latency Optimizations (January 2026)

| Optimization | Implementation | Benefit |
|--------------|---------------|---------|
| Zero-Allocation Hot Path | `TransactionScopePool` | Eliminates heap allocations during event processing |
| Cache Line Alignment | `alignas(64)` / `CacheAlignedAtomic` | Prevents false sharing on contended atomics |
| Relaxed Memory Ordering | `memory_order_relaxed` | Reduces memory barrier overhead for counters |
| CAS Backoff | `_mm_pause()` with exponential backoff | Reduces contention on CAS loops |
| Devirtualization | `final` classes | Enables compiler inlining of virtual calls |
| CPU Pinning | `CpuAffinity` utilities | Reduces context switches and cache pollution |
| Huge Pages | `HugePages` utilities | Reduces TLB misses for large allocations |
| NUMA Allocation | `NumaAllocator` with STL-compatible `NumaNodeAllocator<T>` | Reduces cross-socket memory traffic |

---

## 2. System Architecture Overview

### 2.1 High-Level Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           EXTERNAL SYSTEMS                                   │
│           ┌──────────────┐                    ┌──────────────┐               │
│           │  FIX Clients  │                    │  WS Clients   │               │
│           │ (FIX 5.0SP2) │                    │  (JSON/WS)   │               │
│           └──────┬───────┘                    └──────┬───────┘               │
│                  │                                    │                       │
│           ┌──────▼───────┐                    ┌──────▼───────┐               │
│           │  FixGateway   │                    │  WsServer     │               │
│           │  (QuickFIX)  │                    │  (Beast)      │               │
│           └──────┬───────┘                    └──────┬───────┘               │
│                  └─────────────┬──────────────────────┘                       │
│                                │                                              │
│                    ┌───────────▼──────────┐                                  │
│                    │   MultiOutQueues      │ (fan-out on outbound)            │
│                    └──────────────────────┘                                  │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         QUEUE LAYER                                          │
│  ┌─────────────────────────────────┐    ┌─────────────────────────────────┐ │
│  │       IncomingQueues            │    │        OutgoingQueues           │ │
│  │  ┌─────┬─────┬─────┬─────┐     │    │  ┌─────────────────────────┐    │ │
│  │  │Order│Cncl │Rplc │Timer│     │    │  │ ExecReport │ CnclReject │    │ │
│  │  │Queue│Queue│Queue│Queue│     │    │  │   Queue    │   Queue    │    │ │
│  │  └─────┴─────┴─────┴─────┘     │    │  └─────────────────────────┘    │ │
│  └─────────────────────────────────┘    └─────────────────────────────────┘ │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                       PROCESSING LAYER                                       │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                         Processor                                    │    │
│  │   • Event Dispatcher       • Transaction Creator                    │    │
│  │   • Deferred Event Handler • Component Coordinator                  │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                               │                                              │
│              ┌────────────────┼────────────────┐                            │
│              ▼                ▼                ▼                            │
│  ┌───────────────────┐ ┌────────────┐ ┌──────────────────┐                  │
│  │  OrderStateMachine│ │OrderMatcher│ │ TransactionScope │                  │
│  │  (Boost.MSM)      │ │            │ │                  │                  │
│  │  • States         │ │ • Price-   │ │ • Operation      │                  │
│  │  • Transitions    │ │   Time     │ │   Collection     │                  │
│  │  • Event Handlers │ │   Priority │ │ • Atomic Exec    │                  │
│  └───────────────────┘ └────────────┘ └──────────────────┘                  │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                       DATA LAYER                                             │
│  ┌─────────────────────┐  ┌──────────────────┐  ┌────────────────────────┐  │
│  │     OrderBook       │  │ OrderDataStorage │  │   TransactionMgr       │  │
│  │  ┌───────┬───────┐  │  │                  │  │                        │  │
│  │  │ Buy   │ Sell  │  │  │ • Orders by ID   │  │ • NLinkedTree          │  │
│  │  │ Side  │ Side  │  │  │ • Orders by      │  │   (Dependency Graph)   │  │
│  │  │(Desc) │(Asc)  │  │  │   ClOrderId      │  │ • Transaction Queue    │  │
│  │  └───────┴───────┘  │  │ • Executions     │  │ • Completion Observer  │  │
│  └─────────────────────┘  └──────────────────┘  └────────────────────────┘  │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     PERSISTENCE LAYER                                        │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    FileStorage                                        │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │  StorageRecordDispatcher (Codec Router)                        │  │   │
│  │  │  ┌──────────┬──────────┬──────────┬──────────┬──────────────┐  │  │   │
│  │  │  │Instrument│ Account  │ Clearing │   Order  │  Execution   │  │  │   │
│  │  │  │  Codec   │  Codec   │  Codec   │   Codec  │    Codec     │  │  │   │
│  │  │  └──────────┴──────────┴──────────┴──────────┴──────────────┘  │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                      CONCURRENCY LAYER                                       │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                       TaskManager                                    │    │
│  │   ┌─────────────────────────────────────────────────────────────┐   │    │
│  │   │              oneapi::tbb::task_group                         │   │    │
│  │   │  ┌─────────────────┐     ┌─────────────────────────────┐    │   │    │
│  │   │  │Event Processors │     │ Transaction Processors      │    │   │    │
│  │   │  │  (Parallel)     │     │      (Parallel)             │    │   │    │
│  │   │  └─────────────────┘     └─────────────────────────────┘    │   │    │
│  │   └─────────────────────────────────────────────────────────────┘   │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                             NLinkedTree                              │   │
│  │                       (Transaction Dependency)                       │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Namespace Organization

```
COP (Concurrent Order Processor)
├── Proc        → Order processing core (Processor, OrderMatcher, DeferedEventContainer)
├── Queues      → Event queue management (InQueues, OutQueues, InQueueProcessor)
├── ACID        → Transaction management (TransactionManager, Transaction, Scope, TransactionScopePool)
├── OrdState    → Order state machine (OrderState_, OrderStatePersistence)
├── Store       → Storage and persistence (OrderDataStorage)
├── SubscrMgr   → Subscription management (SubscrManager, OrderFilter, EntryFilter)
├── SL          → Subscription layer (SubscriptionLayerImpl)
├── EventMgr    → Event management (EventManager, EventDispatcher)
├── PG          → PostgreSQL write-behind (PGWriteBehind, PGRequestBuilder) [optional]
├── App         → WebSocket server + FIX gateway (WsServer, FixGateway, MultiOutQueues) [optional]
├── Tasks       → Task scheduling (TaskManager, oneTBB integration)
├── Impl        → Internal implementation details
└── aux         → Utility functions and helpers
```

---

## 3. Component Architecture

### 3.1 Processor (Central Coordinator)

**Location:** `src/Processor.h`, `src/Processor.cpp`

```
┌─────────────────────────────────────────────────────────────────┐
│                         Processor                                │
├─────────────────────────────────────────────────────────────────┤
│ Implements:                                                      │
│   • InQueueProcessor     - Event consumption                    │
│   • DeferedEventContainer - Deferred event storage              │
│   • DeferedEventFunctor  - Deferred event execution             │
│   • TransactionProcessor - Transaction handling                 │
├─────────────────────────────────────────────────────────────────┤
│ Dependencies:                                                    │
│   ┌──────────────────┐  ┌──────────────────┐                   │
│   │ IdTValueGenerator│  │ OrderDataStorage │                   │
│   └──────────────────┘  └──────────────────┘                   │
│   ┌──────────────────┐  ┌──────────────────┐                   │
│   │    OrderBook     │  │   InQueues       │                   │
│   └──────────────────┘  └──────────────────┘                   │
│   ┌──────────────────┐  ┌──────────────────┐                   │
│   │    OutQueues     │  │TransactionManager│                   │
│   └──────────────────┘  └──────────────────┘                   │
│   ┌──────────────────┐  ┌──────────────────┐                   │
│   │   OrderState     │  │   OrderMatcher   │                   │
│   └──────────────────┘  └──────────────────┘                   │
├─────────────────────────────────────────────────────────────────┤
│ Key Methods:                                                     │
│   process()              → Pop and process next event           │
│   onEvent(OrderEvent)    → Handle new order                     │
│   onEvent(CancelEvent)   → Handle cancellation                  │
│   onEvent(ReplaceEvent)  → Handle replacement                   │
│   processDeferedEvent()  → Execute deferred operations          │
└─────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `ProcessorTest.cpp` | `ProcessorTest.*` | Event pipeline validation |
| `IntegrationTest.cpp` | `IntegrationTest.*` | End-to-end integration |
| `EventProcessingBench.cpp` | `BM_EventProcessing*` | Throughput benchmarking |

---

### 3.2 IncomingQueues (Event Input)

**Location:** `src/IncomingQueues.h`, `src/IncomingQueues.cpp`

```
┌─────────────────────────────────────────────────────────────────┐
│                      IncomingQueues                              │
├─────────────────────────────────────────────────────────────────┤
│ Implements:                                                      │
│   • InQueues          - Queue interface                         │
│   • InQueuesPublisher - Event publishing                        │
│   • InQueuesContainer - Queue management                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              Queue Organization by Source                │    │
│  │                                                          │    │
│  │   Source A          Source B          Source C          │    │
│  │  ┌────────┐        ┌────────┐        ┌────────┐        │    │
│  │  │Orders  │        │Orders  │        │Orders  │        │    │
│  │  ├────────┤        ├────────┤        ├────────┤        │    │
│  │  │Cancels │        │Cancels │        │Cancels │        │    │
│  │  ├────────┤        ├────────┤        ├────────┤        │    │
│  │  │Replaces│        │Replaces│        │Replaces│        │    │
│  │  ├────────┤        ├────────┤        ├────────┤        │    │
│  │  │States  │        │States  │        │States  │        │    │
│  │  ├────────┤        ├────────┤        ├────────┤        │    │
│  │  │Timers  │        │Timers  │        │Timers  │        │    │
│  │  └────────┘        └────────┘        └────────┘        │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
│  Thread Safety: std::atomic<InQueueProcessor*> + tbb::mutex     │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│ Event Types Supported:                                           │
│   • OrderEvent         - New order submission                   │
│   • OrderCancelEvent   - Order cancellation request             │
│   • OrderReplaceEvent  - Order modification request             │
│   • OrderChangeStateEvent - State change notification           │
│   • ProcessEvent       - Process control message                │
│   • TimerEvent         - Time-based event                       │
└─────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `IncomingQueuesTest.cpp` | `IncomingQueuesTest.*` | Queue insertion/retrieval |

---

### 3.3 OutgoingQueues (Event Output)

**Location:** `src/OutgoingQueues.h`, `src/OutgoingQueues.cpp`

```
┌─────────────────────────────────────────────────────────────────┐
│                       OutgoingQueues                             │
├─────────────────────────────────────────────────────────────────┤
│ Implements: OutQueues                                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │            Outbound Event Routing                        │    │
│  │                                                          │    │
│  │   Target A          Target B          Target C          │    │
│  │  ┌────────────┐    ┌────────────┐    ┌────────────┐    │    │
│  │  │ExecReports │    │ExecReports │    │ExecReports │    │    │
│  │  ├────────────┤    ├────────────┤    ├────────────┤    │    │
│  │  │CnclRejects │    │CnclRejects │    │CnclRejects │    │    │
│  │  ├────────────┤    ├────────────┤    ├────────────┤    │    │
│  │  │BizRejects  │    │BizRejects  │    │BizRejects  │    │    │
│  │  └────────────┘    └────────────┘    └────────────┘    │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
│  Thread Safety: oneapi::tbb::mutex per queue                    │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│ Event Types Produced:                                            │
│   • ExecReportEvent    - Execution report (fill, ack, etc.)    │
│   • CancelRejectEvent  - Cancel request rejection              │
│   • BusinessRejectEvent - Business rule rejection              │
└─────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `OutgoingQueuesTest.cpp` | `OutgoingQueuesTest.*` | Output queue validation |
| `IntegrationTest.cpp` | `IntegrationTest.*` | End-to-end output verification |

---

### 3.4 OrderBook (Price-Sorted Order Storage)

**Location:** `src/OrderBookImpl.h`, `src/OrderBookImpl.cpp`

```
┌─────────────────────────────────────────────────────────────────┐
│                        OrderBook                                 │
├─────────────────────────────────────────────────────────────────┤
│ Implements: OrderBook interface                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                   Per-Instrument Structure                 │  │
│  │                                                            │  │
│  │   Instrument: AAPL                                         │  │
│  │  ┌─────────────────────┐    ┌─────────────────────────┐   │  │
│  │  │      BUY SIDE       │    │       SELL SIDE         │   │  │
│  │  │   (Price DESC)      │    │     (Price ASC)         │   │  │
│  │  │                     │    │                         │   │  │
│  │  │  $150.50 ─► [Ord1]  │    │  $150.55 ─► [Ord4]     │   │  │
│  │  │  $150.45 ─► [Ord2]  │    │  $150.60 ─► [Ord5]     │   │  │
│  │  │  $150.40 ─► [Ord3]  │    │  $150.65 ─► [Ord6]     │   │  │
│  │  │           ...       │    │           ...           │   │  │
│  │  └─────────────────────┘    └─────────────────────────┘   │  │
│  │         ▲                            ▲                     │  │
│  │         │                            │                     │  │
│  │    buyLock_                     sellLock_                  │  │
│  │  (tbb::mutex)                (tbb::mutex)                  │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│ Operations:                                                      │
│   add(order)       → Insert at price level                      │
│   remove(order)    → Remove from price level                    │
│   find(criteria)   → Locate matching orders                     │
│   restore(order)   → Recovery from persistence                  │
│ Stop book (#223), per instrument:                               │
│   addStop/removeStop → hold a stop order, or stop holding it    │
│   holdsStops         → whether the instrument holds any         │
│   triggeredStops(px) → the held stops a trade at px reaches     │
├─────────────────────────────────────────────────────────────────┤
│ Data Structure:                                                  │
│   std::multimap<Price, OrderEntry*, PriceComparator>            │
│   • Buy side: Greater-than comparator (best price first)        │
│   • Sell side: Less-than comparator (best price first)          │
│   • Stop book: buy stops by stop price, lowest first, sell      │
│     stops highest first, each price in arrival order, under     │
│     stopLock_; a count lets a trade skip the lock when the      │
│     instrument holds none                                       │
└─────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `OrderBookTest.cpp` | `OrderBookTest.*` | Insert/remove/lookup operations |
| `IntegrationTest.cpp` | `IntegrationTest.*` | Order book integration scenarios |

---

### 3.5 OrderMatcher (Matching Engine)

**Location:** `src/OrderMatcher.h`, `src/OrderMatcher.cpp`

```
┌─────────────────────────────────────────────────────────────────┐
│                       OrderMatcher                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                 Matching Algorithm                         │  │
│  │                                                            │  │
│  │   Incoming BUY Order                                       │  │
│  │         │                                                  │  │
│  │         ▼                                                  │  │
│  │   ┌─────────────────────────────────────────────┐         │  │
│  │   │ 1. Locate SELL side for instrument          │         │  │
│  │   └─────────────────────────────────────────────┘         │  │
│  │         │                                                  │  │
│  │         ▼                                                  │  │
│  │   ┌─────────────────────────────────────────────┐         │  │
│  │   │ 2. Check price compatibility                │         │  │
│  │   │    BUY price >= SELL price (for limit)      │         │  │
│  │   │    Market orders, released STOPs: always    │         │  │
│  │   │    match                                    │         │  │
│  │   └─────────────────────────────────────────────┘         │  │
│  │         │                                                  │  │
│  │         ▼                                                  │  │
│  │   ┌─────────────────────────────────────────────┐         │  │
│  │   │ 3. Calculate fill quantity                  │         │  │
│  │   │    min(incoming_qty, resting_qty)           │         │  │
│  │   └─────────────────────────────────────────────┘         │  │
│  │         │                                                  │  │
│  │         ▼                                                  │  │
│  │   ┌─────────────────────────────────────────────┐         │  │
│  │   │ 4. Create ExecutionDeferedEvent             │         │  │
│  │   │    (deferred for transaction processing)    │         │  │
│  │   └─────────────────────────────────────────────┘         │  │
│  │         │                                                  │  │
│  │         ▼                                                  │  │
│  │   ┌─────────────────────────────────────────────┐         │  │
│  │   │ 5. For each held stop the trade reaches,    │         │  │
│  │   │    queue a ReleaseStopDeferedEvent (#223)   │         │  │
│  │   └─────────────────────────────────────────────┘         │  │
│  │         │                                                  │  │
│  │         ▼                                                  │  │
│  │   ┌─────────────────────────────────────────────┐         │  │
│  │   │ 6. Continue matching if qty remaining       │         │  │
│  │   └─────────────────────────────────────────────┘         │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│ Priority: Price-Time (FIFO at each price level)                 │
└─────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `OrderMatcherTest.cpp` | `OrderMatcherTest.*` | Order matching unit tests |
| `IntegrationTest.cpp` | `IntegrationTest.*` | Order matching in full flow |
| `ProcessorBench.cpp` | `BM_ProcessNewOrderWithMatch`, `BM_ProcessSwapWithMatch`, `BM_TaskManagerTrades` | Matching through a `Processor`, a swap's too, and through the worker pool |
| `OrderMatchingBench.cpp` | `BM_OrderBook*` | The book the matcher searches: add, remove, find, top of book |
| `FixGatewayBench.cpp` | `BM_SingleLegMatch`, `BM_FxSwapMatch` | One `OrderMatcher::match()`, outright and swap, with `BUILD_FIX=ON` (#306) |

---

### 3.6 FIX Gateway (Optional, `BUILD_FIX=ON`)

**Location:** `app/FixGateway.h`, `app/FixGateway.cpp`, `app/FixOutQueues.cpp`, `app/MultiOutQueues.cpp`

```
┌─────────────────────────────────────────────────────────────────┐
│                    FIX Protocol Layer                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────────────────────────────────────────────┐       │
│  │              FixGateway                               │       │
│  │  Implements: FIX::Application, FIX 5.0 SP2 / FIXT.1.1 │       │
│  │                                                       │       │
│  │  INBOUND:                                             │       │
│  │    NewOrderSingle (35=D)  → OrderEvent                │       │
│  │    OrderCancelRequest (35=F) → OrderCancelEvent       │       │
│  │    OrderCancelReplaceRequest (35=G) → OrderReplaceEvent│      │
│  │    OrderStatusRequest (35=H) → answered here (#383)   │       │
│  │                    ↓                                   │       │
│  │           IncomingQueues::push()                       │       │
│  │                                                       │       │
│  │  OUTBOUND:                                            │       │
│  │    ExecutionEntry → FIX50SP2::ExecutionReport (35=8)  │       │
│  │    CancelReject   → FIX50SP2::OrderCancelReject (35=9)│       │
│  │                    ↓                                   │       │
│  │           Session::sendToTarget()                      │       │
│  └──────────────────────────────────────────────────────┘       │
│                                                                  │
│  ┌──────────────────────────────────────────────────────┐       │
│  │              MultiOutQueues                            │       │
│  │  Implements: OutQueues                                 │       │
│  │                                                       │       │
│  │  Processor::outQueues_ → push() → WsOutQueues         │       │
│  │                                 → FixOutQueues         │       │
│  │                                                       │       │
│  │  Each delegate silently ignores events not targeting   │       │
│  │  its protocol (WsOutQueues broadcasts to WS sessions,  │       │
│  │  FixOutQueues routes to FIX session by source string)  │       │
│  └──────────────────────────────────────────────────────┘       │
│                                                                  │
│  Session Routing:                                                │
│    source_ = "FIX:SENDER->TARGET" on each order                │
│    sessionMap_ maps source string → FIX::SessionID              │
│    Populated on onLogon(), cleared on onLogout()                │
│                                                                  │
│  Threading: ThreadedSocketAcceptor (thread-per-session)         │
│    FixGateway is thread-safe:                                    │
│      • IncomingQueues::push() — lock-free tbb::concurrent_queue │
│      • WideDataStorage reads — spin_rw_mutex (shared)           │
│      • Session::sendToTarget() — internally synchronized        │
│      • sessionMap_ — protected by spin_rw_mutex                 │
│                                                                  │
│  PIMPL isolation (FixServer) keeps QuickFIX headers confined    │
│    to FixServer.cpp — no header leakage to other TUs            │
├─────────────────────────────────────────────────────────────────┤
│ FIX↔COP Enum Mapping:                                           │
│   Side:  '1'↔BUY  '2'↔SELL  '5'↔SELL_SHORT  '6'↔CROSS        │
│   OrdType: '1'↔MARKET  '2'↔LIMIT  '3'↔STOP  '4'↔STOPLIMIT    │
│   TIF:  '0'↔DAY  '1'↔GTC  '3'↔IOC  '4'↔FOK  '2'↔OPG         │
│   Currency: "USD","EUR","GBP","JPY","CHF","AUD","CAD","NZD"     │
└─────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `FixFullPipelineTest.cpp` | `FixFullPipelineTest.EveryComponentTouched` | Two FIX orders through the whole pipeline, checking each component's mark |
| `FixEndToEndTest.cpp` | `FixEndToEndTest.*` | Full order lifecycle via FIX |
| `FixGatewayTest.cpp` | `FixEnumTest.*` | Enum conversion round-trips |
| `FixGatewayTest.cpp` | `FixGatewayInboundTest.*` | FIX message translation |
| `FixGatewayTest.cpp` | `FixtSessionTest.*` | A real FIXT.1.1 session from `config/fix.cfg`: what it sends, validated with FIX 5.0 SP2 |
| `FixGatewayTest.cpp` | `MultiOutQueuesTest.*` | Fan-out adapter |
| `FixRotatingLogTest.cpp` | `FixRotatingLogTest.*` | The FIX log's files rotate and stay bounded (#160) |
| `FixCredentialsTest.cpp` | `FixCredentialsTest.*` | The FIX credentials file, and its PBKDF2 password hashes (#147) |
| `FixRefusalTest.py` | `FixRefusalsReachOnlyTheirSender` (ctest) | A real server: an order the gateway refuses is answered to its sender alone (#189) |
| `FixLogonTest.py` | `FixLogonNeedsCredentials` (ctest) | A real server: only a counterparty with its password logs on, and it trades only its accounts (#147) |
| `FixResendTest.py` | `FixClientRecoversMissedReports` (ctest) | A real server: a client that reconnects gets back, with a ResendRequest, the reports it missed (#160) |
| `FixGatewayBench.cpp` | `BM_FixBuildExecutionReport`, `BM_FixBuildSwapLegReport`, `BM_FixSendExecutionReport`, `BM_FixOrderAndAcknowledgement` | The gateway's ExecutionReport builder, for an outright trade and a swap leg's; a trade's report from `FixOutQueues::push` to a logged-on session from `config/fix.cfg`; and a new order through the gateway to the engine's queue followed by its acknowledgement to the session, without the engine between them (#395), each order with a ClOrdID of its own (#402) |
| `FixGatewayBench.cpp` | `BM_FixCancelRequest`, `BM_FixReplaceRequest`, `BM_FixReplaceRefusal`, `BM_FixCancelOfUnknownOrder`, `BM_FixEngineCancelReject`, `BM_FixEngineCancelRejectOfReplacedOrder`, `BM_FixOrderStatusRequest`, `BM_FixRepeatedClOrdId` | A session's requests about its working order and the refusals it is sent (#423): a cancel and a replace through the gateway to the engine's queue; the engine's refusal of a replacement and of a cancel, through `FixOutQueues` to the session, and of a cancel of an order already replaced, whose reject describes the order working in its place (#385); the gateway's own refusal of a cancel for an order the session never sent; a status request (35=H) answered; and a new order that repeats the order's ClOrdID, refused before the engine sees it. Each fails unless the session was sent, or the engine's queue handed, what it expects |
| `FixBenchConfigTest.sh` | `FixBenchmarksFindTheirConfig` (ctest, with `BUILD_BENCHMARKS=ON`) | The session benchmarks read `fix.cfg` and its dictionaries from `OP_BENCH_CONFIG_DIR` when it is set, as a benchmark comparison sets it to the copy saved with its baseline, and from `config/` in their tree otherwise; a directory without them fails, naming what it lacks (#415) |

---

## 4. Data Flow Diagrams

### 4.1 New Order Flow

```
┌──────────────┐
│External Order│
│   (FIX/API)  │
└──────┬───────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ IncomingQueues.push(OrderEvent)                                          │
│   • Route to source-specific queue                                       │
│   • Add to processing queue                                              │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Processor.process()                                                      │
│   • Pop from processing queue                                            │
│   • Create TransactionScope                                              │
│   • Create Context                                                       │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ OrderStateMachine.onOrderReceived()                                      │
│   • Validate order                                                       │
│   • Create OrderEntry                                                    │
│   • Add to OrderDataStorage, locked until initialised (#13), and marked  │
│     until its transaction is enqueued (#83)                              │
│   • Transition: INITIAL → RCVD_NEW → NEW                                 │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ TransactionScope collects operations, which run when it executes:        │
│   • MatchOrderTrOperation (the order's booking)                          │
│   • AddToOrderBookTrOperation (a limit order; a market one isn't booked) │
│   • CreateExecReportTrOperation (the NEW report)                         │
│   • PersistOrderTrOperation, put ahead of them all (#28)                 │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ TransactionMgr.enqueue(transaction)                                      │
│   • Add to NLinkedTree dependency graph                                  │
│   • Mark objects as "in transaction"                                     │
│   • Schedule execution                                                   │
│   • The processor then clears the order's mark: a cancel or replace of   │
│     the order can now be enqueued after this transaction (#83)           │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ TaskManager executes the transaction, its operations in order:           │
│   • Persist: LMDBStorage writes the order's new version, so nothing      │
│     is reported before it is durable (#20, #28)                          │
│   • Match: OrderMatcher.match() finds a contra order at a crossing       │
│     price; each fill becomes a deferred execution event                  │
│   • Book: OrderBook.add() rests what is left                             │
│   • Report: OutgoingQueues gets the NEW execution report                 │
│   • Then each deferred fill runs as a transaction of its own, which      │
│     persists both orders and reports the trade to both sides             │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ OutgoingQueues → clients (WsOutQueues broadcasts over WebSocket)         │
│   • Execution Report (NEW)                                               │
│   • Execution Report (TRADE), to both sides, if matched                  │
└──────────────────────────────────────────────────────────────────────────┘
```

### 4.2 Order Cancellation Flow

```
┌────────────────┐
│ Cancel Request │
│   (FIX/API)    │
└───────┬────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ IncomingQueues.push(OrderCancelEvent)                                    │
└──────────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Processor.onEvent(OrderCancelEvent)                     (event worker)   │
│   • Locate order in OrderDataStorage; unknown → CancelRejectEvent        │
│   • Wait while the order is marked: the transaction that books it is     │
│     not enqueued yet, and the cancel must come after it (#83)            │
│   • Enqueue a transaction holding CancelOrderTrOperation, related to     │
│     the order and its instrument                                         │
└──────────────────────────────────────────────────────────────────────────┘
        │  runs after every earlier transaction on the order or instrument,
        │  and after the fills those transactions caused (#73)
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ CancelOrderTrOperation.execute()                   (transaction worker)  │
│   • Queue CancelRequestDeferedEvent                                      │
└──────────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Processor.process(onExecCancel)                          (same worker)   │
│   • Not live (FILLED, CANCELED, ...) → CancelRejectTrOperation           │
│   • Live (NEW, PARTFILL, SUSPENDED, DFD) → OrderStateMachine             │
│       onExecCancel: NO_CNL_REPLACE → CNCL_REPLACED, leavesQty = 0        │
│       Create RemoveFromOrderBookTrOperation                              │
│       Create CreateExecReportTrOperation (CANCELED)                      │
│   • No transition (a replace pending) → CancelRejectTrOperation          │
└──────────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Transaction Execution:                                                   │
│   • PersistOrderTrOperation (first, #28)                                 │
│   • OrderBook.remove(order)                                              │
│   • OutQueues.push(ExecReport CANCELED), or the cancel reject            │
└──────────────────────────────────────────────────────────────────────────┘
```

Suspend, resume and finish (`OrderChangeStateEvent`) and the timer events (expiration, day end, day start) take the
same path since #96. The handler waits for the order's booking (#83) and enqueues a `ChangeOrderStateTrOperation`,
related to the order and its instrument. Its `StateChangeDeferedEvent` is decided by `Processor::process(OrderStateChange)`
on the transaction worker, under the order's lock, after the order's earlier transactions and their fills.

### 4.3 Trade Execution Flow

```
┌───────────────────────────────────────────────────────────────┐
│              ExecutionDeferedEvent (from Matcher)             │
└───────────────────────────────┬───────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Processor.processDeferedEvent()                                          │
│   • Pop from deferred event queue                                       │
│   • Create new TransactionScope                                         │
└──────────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ OrderStateMachine.onTradeExecution()                                     │
│                                                                          │
│   ┌─────────────────────────────────────────────────────────────┐       │
│   │ Aggressor Order (Incoming)                                   │       │
│   │   IF remaining_qty > 0: NEW → PARTFILL                      │       │
│   │   IF remaining_qty = 0: NEW → FILLED                        │       │
│   └─────────────────────────────────────────────────────────────┘       │
│                                                                          │
│   ┌─────────────────────────────────────────────────────────────┐       │
│   │ Passive Order (Resting)                                      │       │
│   │   IF remaining_qty > 0: NEW/PARTFILL → PARTFILL             │       │
│   │   IF remaining_qty = 0: NEW/PARTFILL → FILLED               │       │
│   └─────────────────────────────────────────────────────────────┘       │
└──────────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ TransactionScope collects:                                               │
│   • CreateTradeExecReportTrOperation (for aggressor)                    │
│   • CreateTradeExecReportTrOperation (for passive)                      │
│   • RemoveFromOrderBookTrOperation (if filled)                          │
│   • ExecutionEntry creation                                             │
└──────────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Outputs:                                                                 │
│   • ExecReport TRADE to aggressor                                       │
│   • ExecReport TRADE to passive order owner                             │
│   • Updated order states persisted                                      │
│   • No execution record: reports aren't persisted (#33)                 │
└──────────────────────────────────────────────────────────────────────────┘
```

**A fill's own figures (#411).** A booking's transaction decides its fills as it drains its deferred events, and each fill's persist and reports go into a transaction of their own, which shares no object with the instrument's next booking. That booking can match the same resting order, and apply its own fill, before the first fill's transaction runs. So each decision's record and report carry what that decision left the order with (`OrderFigures`: status, LeavesQty, CumQty, AvgPx, the day figures, the stop's mark and the state machine's state), read under the order's write lock as the decision is made. `Processor::persistOrder()` hands them to the order's `PersistOrderTrOperation`, and `CreateTradeExecReportTrOperation` to its report's `ExecReportEvent::figures_`, which the WebSocket `order_update` writes, and on this branch the FIX gateway's ExecutionReport too (`FixOutQueues`: LeavesQty, CumQty, AvgPx, the day figures and the WorkingIndicator, §3.6). A swap's far leg is reported, with no decision of its own, just after its near leg's fill, and takes what that fill left under the order's read lock: the transaction draining the fill names the instrument, as every one that decides about the order does, so nothing decides about it between the two. A second decision about the order in the same transaction replaces the persist's figures, so the transaction still writes the order once (#148). `ReportRestatementTrOperation` takes a restatement's figures the same way (#441). A stop's release persists the order as it is when the transaction runs: its own operation decides as it executes, and its transaction names the instrument.

**What readers outside a transaction see (#404).** They give each order as its last report left it, since the order itself can have what no report has said, nor made durable: a fill applied by a booking whose own transaction hasn't run. `processEvent_GenerateExecution`, where every report is made, copies the report's figures into the order's `published_` under the write lock it already holds, after the transaction's persists have committed, and restore sets them from the record. `order_snapshot` gathers the orders under the store's lock and reads each one's `published_` under the order's own lock once the store's is released: a replace holds its original's write lock while it stores the replacement, which takes the store's, so the snapshot would deadlock with it. An order whose first report isn't out is left out of the snapshot; the session joined before the snapshot was taken, so that report's `order_update` follows it. On this branch the FIX gateway's status answer (35=H, #383), its answer to a duplicate (#382), and the OrdStatus (39) of a 35=9 refusing a replace, whether the gateway refuses it or the engine does without giving the original's status, read `published_` the same way (`seenOf()`, `reportedStatus()`); an order whose first report isn't out is Pending New, with the OrderQty it was stored with and nothing filled.

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `TaskManagerTest.cpp` | `AFillsReportAndRecordCountNoLaterFill`, `EachFillsReportAndRecordCountTheFillsUpToIt` | Each fill's report and record count no later fill: with the first fill's transaction held until the next booking has filled the order again, and with nothing held (#411) |
| `ProcessorTest.cpp` | `AFillsRecordCountsNoLaterFillACrashLost`, `AnOrderIsWrittenWithWhatItsTransactionsLastDecisionLeft` | Restarted after a crash that lost the later fill's transaction, the order counts each fill once; a transaction writes what its last decision about the order left (#411) |
| `CodecsTest.cpp`, `PGRequestBuilderTest.cpp`, `JsonSerializerTest.cpp`, `WsAuthorizationTest.cpp` | `AnOrderIsWrittenWithTheFiguresGiven`, `FromOrderWritesTheFiguresADecisionLeft`, `AnOrderUpdateGives*` | The record, the Postgres row and the order update take the figures given (#411) |
| `TransactionScopeTest.cpp` | `FindOperationGivesTheOperationOfItsTypeAndObject` | The persist a later decision in the transaction goes to (#411) |
| `ProcessorTest.cpp` | `EachOfASwapFillsLegsIsReportedWithWhatTheFillLeft` | Both legs of each swap fill report what that fill left, with the resting order's second fill applied before the first fill's transaction runs (#411) |
| `FixGatewayTest.cpp` | `FixtSessionTest.AReportGivesWhatItsOwnFillLeftTheOrder` | A FIX report gives the figures the engine gave it, and the order as it is without them (#411) |

### 4.4 Order Replace Flow

```
┌─────────────────┐
│ Replace Request │
│   (FIX/API)     │
└────────┬────────┘
         │
         ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ IncomingQueues.push(OrderReplaceEvent): the queue owns the replacement   │
└──────────────────────────────────────────────────────────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Processor.onEvent(OrderReplaceEvent)                    (event worker)   │
│   • No replacement, or the original unknown → OrderRejectEvent           │
│   • Wait while the original is marked, as a cancel does (#83)            │
│   • Enqueue a transaction holding ReplaceOrderTrOperation (a copy of     │
│     the replacement), related to the original and its instrument         │
└──────────────────────────────────────────────────────────────────────────┘
         │  runs after every earlier transaction on the original or instrument,
         │  and after the fills those transactions caused
         ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ ReplaceOrderTrOperation.execute()                  (transaction worker)  │
│   • Queue ReplaceRequestDeferedEvent (it takes the replacement)          │
└──────────────────────────────────────────────────────────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Processor.process(onReplace)                             (same worker)   │
│   • Ask the book whether a market replacement has an order to trade      │
│     with, before taking the original's lock (a matcher can hold the      │
│     book's lock while it waits on an order)                              │
│   • Lock the original. Not NEW/PARTFILL, or a cancel/replace pending,    │
│     or the replacement changes instrument or side, or it is invalid,     │
│     or it is a market order with something left and nothing to trade     │
│     with → refuse                                                        │
│   • A quantity at or below the original's cumQty is amended to it, with  │
│     nothing left: the rest of the order is canceled, as FIX does (#381)  │
│   • Replacement takes the original's cumQty, avgPx and executions;       │
│     stored under a fresh id (a ClOrdID in use → refuse, #67), and        │
│     marked until the transaction below, which books it, is enqueued      │
│     (#83)                                                                │
│   • Original:    onExecReplace: NO_CNL_REPLACE → CNCL_REPLACED,          │
│                  RemoveFromOrderBook, leavesQty = 0, REPLACE report      │
│   • Replacement: onReplace: RCVD_NEW → NEW (PARTFILL with fills),        │
│                  MatchOrder, AddToOrderBook, REPLACE report; one with    │
│                  nothing left → FILLED, REPLACE report only (#381)       │
│   • A held stop's replacement is held at its own stop; a released        │
│     one's stays released if it is still a stop order (#223), and a       │
│     new stopPx for it is refused (#238)                                  │
│   • A refusal → RefuseReplaceTrOperation (OrderRejectEvent)              │
└──────────────────────────────────────────────────────────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Transaction Execution:                                                   │
│   • PersistOrderTrOperation for both orders (first, #28)                 │
│   • OrderBook.remove(original); MatchOrder(replacement), then            │
│     OrderBook.add(replacement) unless it is a market order               │
│   • OutQueues: REPLACE for the original (REPLACED) and the replacement,  │
│     both naming the original in origOrderId; or the refusal              │
└──────────────────────────────────────────────────────────────────────────┘
```

`ProcessorBench.cpp`'s `BM_ProcessReplaceOrder` times a replace through a `Processor`, its transaction run inline: a resting order's price replaced, and the replacement booked in its place (#457).

---

### 4.5 Stop Order Flow (#223)

```
┌──────────────────────────────────────────────────────────────────────────┐
│ STOP or STOPLIMIT received (stopPx above 0, or refused)                  │
│   • processReceive: NEW, then AddToStopBookTrOperation instead of        │
│     MatchOrder and AddToOrderBook: held in its instrument's stop book,   │
│     in neither side of the price book                                    │
└──────────────────────────────────────────────────────────────────────────┘
       │  ... later, any trade in the instrument ...
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ OrderMatcher.match()                               (transaction worker)  │
│   • Queue the trade's ExecutionDeferedEvent                              │
│   • holdsStops(instrument)? Then triggeredStops(lastPx): buy stops at or │
│     below it, lowest first; sell stops at or above it, highest first;    │
│     each price in arrival order. Queue a ReleaseStopDeferedEvent for     │
│     each, once per chain of events (claimStopRelease)                    │
│   • Queued after the trade: a match rolled back, or a fill that throws,  │
│     drops them with it                                                   │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ ReleaseStopDeferedEvent.execute()                         (same worker)  │
│   • Wait until the stop's booking is enqueued (#83)                      │
│   • Transaction: ReleaseStopTrOperation, then PersistOrderTrOperation,   │
│     related to the stop and its instrument                               │
└──────────────────────────────────────────────────────────────────────────┘
       │  runs after every earlier transaction on the stop or the
       │  instrument: after the whole sweep whose trade reached it, since
       │  a sweep continues within its own transaction's deferred events
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ ReleaseStopTrOperation.execute()                   (transaction worker)  │
│   • Still held and working? If not (canceled, replaced, suspended, or    │
│     released by an earlier trade's release), do nothing                  │
│   • removeStop; stopTriggered_ = true; match: a STOP as a market order   │
│     (no contra: canceled), a STOPLIMIT as a limit order, then booked     │
│     with what is left                                                    │
│   • Rollback: undo the booking, drop its deferred events, clear the      │
│     flag, hold the stop again                                            │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ PersistOrderTrOperation, then ReportReleaseTrOperation (#236)            │
│   • The order's new version, with stopTriggered_, is written first       │
│   • Then, if the release released the order, a TRIGGERED execution       │
│     report (FIX's ExecType L), the order NEW. The trades the release     │
│     led to are reported after it, by their own transactions              │
└──────────────────────────────────────────────────────────────────────────┘
```

- **Records:** `stopTriggered_` is persisted (`OrderCodec` format version 2). Version 0 and 1 records decode as held.
- **Restore:** a working held stop goes back into the stop book. A released STOP is left to match, as an open market order is, and a released STOPLIMIT is booked as a limit order. `app/main.cpp` waits for the transaction manager to hold nothing (`pendingTransactions()`) between the restored orders it matches, since a release matches on a transaction worker.
- **Last trade (#237):** each trade's transaction persists its instrument's last price, and an id drawn when the trade was made, in the commit that holds its fills (`PersistLastTradeTrOperation`, record type `LASTTRADE`, keyed by the instrument's number with date 2). A restore releases the held stops that price reached and that arrived before the trade, through `Processor::releaseRestored()`, once `app/main.cpp` has matched the orders that lost their fills: a lost release ran after the trades that crossing orders lost.

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `ProcessorTest.cpp` | `ProcessorTest.*Stop*` | Hold, release, order of release, cascade, cancel/replace/suspend, races with the release, restarts |
| `OrderBookTest.cpp` | `OrderBookTest.*Stop*` | The stop book's order and bounds |
| `TrOperationsTest.cpp` | `MatchOrderOperationTest.ReleaseStop_*` | The release's execute, rollback and no-ops |
| `TrOperationsTest.cpp` | `MatchOrderOperationTest.ReportRelease_*` | The TRIGGERED report, and none for a release that released nothing |
| `IntegrationTest.cpp` | `IntegrationTest.Stops*` | A sweep that reaches a stop, on the real manager with four transaction workers |
| `StorageRecordDispatcherTest.cpp` | `OnlyAWorkingOrderIsBookedOrLeftToMatch`, golden records | Restore placement, format version 2 |

---

### 4.6 Time in Force (#240)

The engine read an order's time in force and acted on none of it until #240.

- **IOC and FOK never rest.** `OrderParams::book()` is `NONE` for them, as for a market order (`cancelsWhatIsLeft()`), but they keep their limit: `tradesWith()` still compares prices. When the sweep finds no contra, `OrderMatcher::match()` cancels what is left (`CancelOrderDeferedEvent`), as it does a market order's.
- **FOK is checked first.** `OrderMatcher::matchNew()`, which a booking (`MatchOrderTrOperation`) and a release (`ReleaseStopTrOperation`) call for an order's first match, walks the other side in priority order with `findContra()`'s test, adding up what the orders it would take have left (`canFillAtOnce()`). Short, the order is canceled with nothing traded. The check and the sweep run in one transaction, and every transaction that books, cancels or matches on the instrument relates to it, so they see one book.
- **Stop orders** take their time in force when released: a held stop doesn't match, and its release calls `matchNew()`.
- **DAY and GTD expire on a timer.** `ExpiryScheduler` (one per server, created in `app/main.cpp`) holds a timer per order:
  - a GTD order's at its `expireTime_`, as `TimerEvent::EXPIRATION`;
  - a DAY order's at the first 17:00 New York after its `creationTime_` (21:00 or 22:00 UTC, with daylight time), or a replacement's after its `lastUpdateTime_`, the replace's time (it keeps its original's `creationTime_`), as `TimerEvent::DAY_END`.
  - The processor sets an order's timer when it accepts the order, or a replacement (`Processor::scheduleExpiry()`). Its thread pushes each timer's event into the incoming queues when it comes due.
  - The change is decided on the transaction worker (`Processor::process(OrderStateChange)`), with the order's other transactions. An order that isn't live (filled, canceled, replaced or expired) decides nothing: `Processor` returns before the state machine, which has no row for it. Without the return the order would be persisted unchanged, and nothing would fail: the state machine's `no_transition` throws, but Boost.MSM hands the throw to `exception_caught`, which is empty (#432).
  - `DAY_END` expires a live DAY order and starts a done-for-day order's next day (`onNewDay`, as before). For any other order it does nothing.
- **A closed order's LeavesQty is 0, whatever closed it** (#386, as ruled on 2026-10-26): its client's cancel or a replace (#73, #74), the engine's cancel of what an IOC, FOK, market or released stop order couldn't trade at once, an expiry, done for day, or a reject (#427). Each closing action in `OrdStateImpl` sets it at once, as the cancel did. That also keeps the matcher off an expired order until it leaves the book: the removal is a transaction of its own, enqueued as the decision ends, so a match enqueued before it runs first, and the matcher passes by only an order with no leaves. Before #386 such a match took the expired order, the state machine dropped the trade without a word (Expired has no row for one, and `exception_caught` is empty), and the matching order traded with nothing: one that crossed a further order was booked across it. The order's reports and its record carry the 0, so what was left is `orderQty_ - cumQty_`. Done for day isn't final: a new day (`onNewDay`) or a resume (`onContinue`) books the order again with what it hasn't filled, and its fills, not its leaves, make it NEW or PARTFILL. A reject's action sets it too, as ruled (#427): whether it stores the order, as it does one its checks refuse (since #436 a market order with nothing to trade with too, which its receive had stored before refusing it), or keeps a copy its receive stored, and in the legacy replace's and `onOrderRejected`'s rejects, which nothing produces.
- **Restore:** `App::scheduleRestoredExpiries()` sets the restored live DAY and GTD orders' timers before the server takes orders. One whose time passed while the server was down fires as soon as the timers start.
- **OPG and ATCLOSE** need auctions the engine doesn't have; they rest as GTC orders.

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `ProcessorTest.cpp` | `*Ioc*`, `*Fok*` | IOC's rest canceled; FOK in full or not at all; a released IOC stop |
| `ProcessorTest.cpp` | `*Gtd*`, `*Day*`, `ATimerOfAnOrderThatFilledDecidesNothing`, `AReplacementHasATimerOfItsOwn*` | Expiry at the order's time, and nothing for an order no longer live |
| `ProcessorTest.cpp` | `AnOrderThatExpiresAfterAPartFill*`, `AnOrderDoneForTheDay*`, `AMarketOrdersRestTheEngineCanceled*`, `AMatchBetweenAnExpiry*` | A closed order's LeavesQty is 0, in its reports and its record; done for day, then booked again with what it hasn't filled; a match ahead of an expired order's removal from the book passes it by (#386) |
| `ProcessorTest.cpp` | `AnOrderTheEngineStoresAndRejects*` | A stored reject has no leaves, in its report and its record, both ways the engine stores one (#427) |
| `StatesTest.cpp` | `ARejected*HasNoLeaves` | Each reject's action leaves none, whether it stores the order or keeps the copy its receive stored (#427) |
| `StateMachineTest.cpp` | `New_To_DoneForDay_OnFinished`, `New_To_Rejected_OnOrderRejected` | No leaves done for the day, all of them back with the new day (#386); none rejected (#427) |
| `FixEndToEndTest.cpp` | `AnOrderTheEngineCloses_IsReportedWithNoLeaves`, `AnOrderTheEngineStoresAndRejects_IsReportedWithNoLeaves` | LeavesQty (151) 0 over FIX, in the order's report and a status answer: an IOC's rest the engine canceled, an expiry (#386), and a reject the engine stored (#427) |
| `ExpirySchedulerTest.cpp` | `ExpirySchedulerTest.*` | The day's end, which orders have a timer, firing order, the thread |
| `ServerDataTest.cpp` | `RestoredDayAndGtdOrdersThatAreLiveGetTheirTimersBack` | Timers set again at a restart |
| `ProcessorBench.cpp` | `BM_ProcessIocRemainder`, `BM_ProcessFokFill`, `BM_ProcessExpiry`, `BM_ProcessInvalidOrder`, `BM_ProcessMarketOrderWithNoMarket` | Timed through a `Processor`: an IOC's rest canceled by the engine, an expiry decided by the transaction its timer enqueues (#426), a FOK order the book fills at once, checked first (#432), and both ways the engine rejects an order it stores (#435) |

### 4.7 Business Days and Day Figures (#244)

An order keeps FIX's day figures for the business day that began at its `dayStart_` (17:00 New York, `ExpiryScheduler::dayStartOf()`): `dayOrderQty_`, what it had left to trade when the day began, and `dayCumQty_` and `dayAvgPx_`, the day's fills. Nothing kept them before #244.

- **Acceptance:** an order's first day is the one its `creationTime_` falls in, the day a DAY order expires with (#240). A replacement takes over its original's day and fills, and its `dayOrderQty_` is its own quantity less what it traded before the day.
- **Fills:** `ExecutionDeferedEvent` reads the processor's clock once per match (`Context::now()`), so every side of a trade counts it in one day. `OrderParams::applyTrade()` begins the day figures first if the trade is in a later day than theirs (`startDay()`): a fill after the day's end, before the order's restatement ran, is the new day's.
- **Restatement:** `ExpiryScheduler::restatementOf()` gives an order that rests past its day, any time in force but DAY, a `TimerEvent::RESTATEMENT` at the day's end. A GTD order that expires first gets none. `Processor::process(OrderStateChange::RESTATE)` decides it on the transaction worker without the state machine, since a restatement changes no state. A live order begins the day (unless a fill already did, which keeps that fill), is persisted, gets a `RESTATED` report (`ReportRestatementTrOperation`, after the persist, #28) with the figures the restatement left, taken under the order's write lock as the persist's are, and gets its next restatement. The restatement's transaction names the order alone, so a booking on the instrument can fill the order before it runs, and its report doesn't count that fill (#441). An order no longer live is restated no more.
- **Restore:** format version 4 of the order record carries `dayStart_`; an older record's is 0. `App::scheduleRestoredExpiries()` sets each live order's restatement too, so one whose day ended while the server was down is restated as the server starts.
- **Lookups:** each thread keeps the business day it last answered, for `dayStartOf()` and `dayEndAfter()` alike, and reads the zone database only when its day changes, under a mutex of `ExpiryScheduler`'s. libstdc++'s `time_zone` fills in its transitions on first use, under a lock made of atomics inside `libstdc++.so` that ThreadSanitizer can't see; the mutex is what lets the TSan job check the lookups.

**Associated Test Cases:**

| Test File | Test Case | Description |
|-----------|-----------|-------------|
| `ProcessorTest.cpp` | `*DayFigures*`, `*Restate*`, `*Restatement*` | The day's fills, a fill after the day's end, the restatement and its report, a replacement's day |
| `ExpirySchedulerTest.cpp` | `*DayStart*`, `*Restatement*`, `DaysLookedUpFromSeveralThreadsAtOnceAgree` | The day a time is in, across the clock changes and from several threads at once; which orders are restated |
| `StorageRecordDispatcherTest.cpp` | `*Golden*` | Format version 4, and `dayStart_` 0 from older ones |
| `ServerDataTest.cpp` | `AnOrderWhoseDayEndedWhileTheServerWasDownIsRestatedAtOnce` | A restart's restatement |
| `TaskManagerTest.cpp` | `ARestatementsReportCountsNoLaterFill` | A restatement's report has what the restatement left, with its transaction held until a booking has filled the order (#441) |
| `FixGatewayTest.cpp` | `FixtSessionTest.ARestatementGivesWhatItLeftTheOrder` | A FIX restatement gives the figures the engine gave its report, not a fill applied after (#441) |
| `ProcessorBench.cpp` | `BM_ProcessRestatement` | Timed through a `Processor`: a restatement decided by the transaction its timer enqueues, a day after the order rested (#451) |

## 5. State Machine Design

### 5.1 Order State Diagram

```
                              ┌─────────────────────────────────────────────────────────────┐
                              │                    ZONE 1 (Primary States)                   │
                              └─────────────────────────────────────────────────────────────┘

                                                    ┌──────────┐
                                                    │ INITIAL  │
                                                    └────┬─────┘
                                                         │ onOrderReceived
                                                         ▼
                                                    ┌──────────┐
                                          ┌─────────│ RCVD_NEW │─────────┐
                                          │         └────┬─────┘         │
                                          │              │               │
                              onRplOrderReceived    onOrderReceived   onExternalOrderReject
                                          │              │               │
                                          ▼              ▼               ▼
                                   ┌────────────┐  ┌──────────┐    ┌──────────┐
                                   │PEND_REPLACE│  │   NEW    │    │ REJECTED │
                                   └────────────┘  └────┬─────┘    └──────────┘
                                                        │
                        ┌───────────────────────────────┼───────────────────────────────┐
                        │                               │                               │
                        ▼                               ▼                               ▼
                   ┌──────────┐                   ┌──────────┐                    ┌──────────┐
         ┌────────│ PARTFILL │◄──────────────────│   NEW    │────────────────────│ REJECTED │
         │        └────┬─────┘  onTradeExecution └────┬─────┘  onOrderRejected   └──────────┘
         │             │                              │
         │ onTrade     │ onTradeExecution             │ onExpired
         │ Execution   │ (remaining=0)                │
         │             ▼                              ▼
         │        ┌──────────┐                   ┌──────────┐
         └───────►│  FILLED  │                   │ EXPIRED  │
                  └──────────┘                   └──────────┘



         ┌──────────┐            ┌──────────┐            ┌──────────┐
         │DONEFORDAY│◄───────────│   NEW    │───────────►│SUSPENDED │
         └──────────┘ onFinished └──────────┘ onSuspended└──────────┘


                              ┌─────────────────────────────────────────────────────────────┐
                              │               ZONE 2 (Cancel/Replace States)                 │
                              └─────────────────────────────────────────────────────────────┘

                                                    ┌────────────────┐
                                                    │ NO_CNL_REPLACE │
                                                    └───────┬────────┘
                                                            │
                                    ┌───────────────────────┼───────────────────────┐
                                    │                       │                       │
                        onCancelReceived                    │              onReplaceReceived
                                    │                       │                       │
                                    ▼                       │                       ▼
                             ┌─────────────┐                │               ┌─────────────┐
                             │GOING_CANCEL │                │               │GOING_REPLACE│
                             └──────┬──────┘                │               └──────┬──────┘
                                    │                       │                      │
                    ┌───────────────┼───────────────┐       │      ┌───────────────┼───────────────┐
                    │               │               │       │      │               │               │
              onCancelRejected  onExecCancel        │       │      │        onExecReplace   onReplaceRejected
                    │               │               │       │      │               │               │
                    ▼               ▼               │       │      │               ▼               ▼
             ┌────────────┐  ┌─────────────┐        │       │      │        ┌─────────────┐ ┌────────────┐
             │NO_CNL_REPL │  │CNCL_REPLACED│        │       │      │        │CNCL_REPLACED│ │NO_CNL_REPL │
             └────────────┘  └─────────────┘        │       │      │        └─────────────┘ └────────────┘
                                                    │       │      │
                                                    └───────┴──────┘
```

### 5.2 State Transition Table

| From State | Event | To State | Action |
|------------|-------|----------|--------|
| INITIAL | onOrderReceived | RCVD_NEW | Create OrderEntry |
| RCVD_NEW | onOrderReceived | NEW | Add to book, send ACK |
| RCVD_NEW | onOrderReceived (refused) | REJECTED | An order `isValid()` refuses, or a market order with nothing to trade with: stored and reported, with no exception raised (#436) |
| RCVD_NEW | onExternalOrderReject | REJECTED | Send REJECT |
| RCVD_NEW | onRplOrderReceived | PEND_REPLACE | Queue replace (not driven since #74) |
| RCVD_NEW | onReplace | NEW | An accepted replacement: into the book, matched (#74) |
| RCVD_NEW | onReplace (original had fills) | PARTFILL | The same, carrying the original's fills (#74) |
| NEW | onTradeExecution (partial) | PARTFILL | Create TRADE report |
| NEW | onTradeExecution (full) | FILLED | Create TRADE report, remove from book |
| NEW | onOrderRejected | REJECTED | Send REJECT |
| NEW | onExpired | EXPIRED | Remove from book |
| NEW | onFinished | DONEFORDAY | End of session |
| NEW | onSuspended | SUSPENDED | Halt trading |
| PARTFILL | onTradeExecution (partial) | PARTFILL | Create TRADE report |
| PARTFILL | onTradeExecution (full) | FILLED | Create TRADE report, remove from book |
| PARTFILL | onTradeCrctCncl | NEW/PARTFILL | Trade correction |
| PARTFILL | onExpired | EXPIRED | Partial fill expiration |
| FILLED | onTradeCrctCncl | PARTFILL/NEW | Trade bust |
| NO_CNL_REPLACE | onCancelReceived | GOING_CANCEL | Begin cancel |
| NO_CNL_REPLACE | onReplaceReceived | GOING_REPLACE | Begin replace (not driven since #74) |
| NO_CNL_REPLACE | onExecReplace | CNCL_REPLACED | The original of an accepted replace: out of the book, leaves 0 (#74) |
| NO_CNL_REPLACE | onExecCancel | CNCL_REPLACED | Client cancel, decided at once: remove from book, leaves 0 (#73) |
| GOING_CANCEL | onExecCancel | CNCL_REPLACED | Remove from book (an order an older server left pending) |
| GOING_CANCEL | onCancelRejected | NO_CNL_REPLACE | Cancel failed |
| GOING_REPLACE | onExecReplace | CNCL_REPLACED | Update order (not driven since #74) |
| GOING_REPLACE | onReplaceRejected | NO_CNL_REPLACE | Replace failed (not driven since #74) |

**Associated Test Cases:**

| Test File | Test Cases | Coverage |
|-----------|------------|----------|
| `StatesTest.cpp` | `StatesTest.*` | Rcvd_New's transitions and refusals, and what a reject keeps |
| `StateMachineTest.cpp` | `StateMachineTest.*` | Full lifecycle scenarios |
| `StateMachineHelper.cpp` | Helper class | Test event dispatching |

---

## 6. Concurrency Model

### 6.1 Concurrency Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         CONCURRENCY LAYERS                                   │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 1: Lock-Free Components                                                │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                  TransactionScopePool                               │   │
│   │                                                                      │   │
│   │   ┌────┐   ┌────┐   ┌────┐   ┌────┐   ┌────┐                       │   │
│   │   │Scp1│──►│Scp2│──►│Scp3│──►│Scp4│──►│... │  (1024 pre-allocated) │   │
│   │   └────┘   └────┘   └────┘   └────┘   └────┘                       │   │
│   │      ▲                                   ▲                          │   │
│   │      │                                   │                          │   │
│   │   head_ (alignas(64))              tail_ (alignas(64))             │   │
│   │                                                                      │   │
│   │   acquire(): atomic_compare_exchange_strong                         │   │
│   │   release(): atomic_compare_exchange_strong                         │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                    Atomic Counters/Flags                             │   │
│   │   • processor_ in IncomingQueues (lock-free attachment)             │   │
│   │   • Task counters in TaskManager (CacheAlignedAtomic)               │   │
│   │   • Transaction IDs (memory_order_relaxed)                          │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 2: Fine-Grained Locking                                                │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                        OrderBook                                     │   │
│   │                                                                      │   │
│   │   Instrument A           Instrument B           Instrument C        │   │
│   │  ┌────────────────┐     ┌────────────────┐     ┌────────────────┐  │   │
│   │  │ buyLock_  [M]  │     │ buyLock_  [M]  │     │ buyLock_  [M]  │  │   │
│   │  │ sellLock_ [M]  │     │ sellLock_ [M]  │     │ sellLock_ [M]  │  │   │
│   │  └────────────────┘     └────────────────┘     └────────────────┘  │   │
│   │                                                                      │   │
│   │   [M] = tbb::mutex (allows concurrent buy/sell on same instrument)  │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                      OrderDataStorage                                │   │
│   │                                                                      │   │
│   │   orderLock_ [M]  ─── protects order maps                           │   │
│   │   execLock_  [M]  ─── protects execution maps                       │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 3: Task Parallelism                                                    │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                       TaskManager                                    │   │
│   │                                                                      │   │
│   │   ┌─────────────────────────────────────────────────────────────┐   │   │
│   │   │           oneapi::tbb::global_control                        │   │   │
│   │   │              (Thread Pool Size)                              │   │   │
│   │   └─────────────────────────────────────────────────────────────┘   │   │
│   │                           │                                         │   │
│   │                           ▼                                         │   │
│   │   ┌─────────────────────────────────────────────────────────────┐   │   │
│   │   │             oneapi::tbb::task_group                          │   │   │
│   │   │                                                              │   │   │
│   │   │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐    │   │   │
│   │   │  │EvtProc 1 │  │EvtProc 2 │  │TrxProc 1 │  │TrxProc 2 │    │   │   │
│   │   │  └──────────┘  └──────────┘  └──────────┘  └──────────┘    │   │   │
│   │   │       ▲              ▲              ▲              ▲        │   │   │
│   │   │       │              │              │              │        │   │   │
│   │   │       └──────────────┴──────────────┴──────────────┘        │   │   │
│   │   │                        Parallel Execution                    │   │   │
│   │   └─────────────────────────────────────────────────────────────┘   │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 4: Transaction Dependency Ordering                                     │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                       NLinkedTree                                    │   │
│   │                                                                      │   │
│   │   Transaction Dependency Graph:                                      │   │
│   │                                                                      │   │
│   │          T1 (Order A)                                               │   │
│   │            │                                                         │   │
│   │            ▼                                                         │   │
│   │          T2 (Order A, B)  ◄─── Must wait for T1                     │   │
│   │           / \                                                        │   │
│   │          /   \                                                       │   │
│   │         ▼     ▼                                                      │   │
│   │   T3(B,C)    T4(A,D) ◄─── Can run in parallel after T2              │   │
│   │         \     /                                                      │   │
│   │          \   /                                                       │   │
│   │           ▼ ▼                                                        │   │
│   │          T5 (C, D)   ◄─── Must wait for T3 and T4                   │   │
│   │                                                                      │   │
│   │   Ready Queue: Transactions with no pending dependencies            │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 6.2 Deadlock Prevention Strategy

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      DEADLOCK PREVENTION                                     │
└─────────────────────────────────────────────────────────────────────────────┘

1. Single-Direction Dependencies (NLinkedTree)
   ┌───────────────────────────────────────────────────────────────────────┐
   │   • Transactions form a DAG (Directed Acyclic Graph)                  │
   │   • New transactions can only depend on existing transactions         │
   │   • No circular waits possible                                        │
   └───────────────────────────────────────────────────────────────────────┘

2. Lock Ordering
   ┌───────────────────────────────────────────────────────────────────────┐
   │   Always acquire in order:                                            │
   │     1. OrderBook locks (buy before sell)                             │
   │     2. OrderStorage locks (order before execution)                   │
   │     3. Queue locks (input before output)                             │
   │     4. FileStorage locks                                             │
   └───────────────────────────────────────────────────────────────────────┘

3. Lock-Free Where Possible
   ┌───────────────────────────────────────────────────────────────────────┐
   │   • Atomic processor attachment                                       │
   │   • Atomic counter updates                                           │
   │   • NumaAllocator: NUMA-local memory binding via mbind()             │
   └───────────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Cases | Coverage |
|-----------|------------|----------|
| `NLinkTreeTest.cpp` | `AddSingleNode`, `ClearTree` | Dependency graph |
| `TaskManagerTest.cpp` | `TaskManagerTest.*` | Task parallelism |
| `TransactionScopePoolTest.cpp` | `TransactionScopePoolTest.*` | Lock-free object pool |
| `CacheAlignedAtomicTest.cpp` | `CacheAlignedAtomicTest.*` | Cache-aligned atomics |
| `CpuAffinityHugePagesTest.cpp` | `CpuAffinityHugePagesTest.*` | CPU pinning, huge pages |
| `NumaAllocatorTest.cpp` | `NumaAllocatorTest.*` | NUMA-aware allocation |

---

## 7. Transaction Management (ACID)

### 7.1 Transaction Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        TRANSACTION SYSTEM                                    │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                       TransactionScope                                       │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                    Operation Collection                              │   │
│   │                                                                      │   │
│   │   Stage 1 (Sub-Transaction)                                         │   │
│   │   ┌─────────────────────────────────────────────────────────────┐   │   │
│   │   │ Op1: CreateExecReportTrOperation (NEW ack)                  │   │   │
│   │   │ Op2: AddToOrderBookTrOperation                              │   │   │
│   │   └─────────────────────────────────────────────────────────────┘   │   │
│   │                                                                      │   │
│   │   Stage 2 (Sub-Transaction)                                         │   │
│   │   ┌─────────────────────────────────────────────────────────────┐   │   │
│   │   │ Op3: MatchOrderTrOperation                                  │   │   │
│   │   │ Op4: CreateTradeExecReportTrOperation                       │   │   │
│   │   └─────────────────────────────────────────────────────────────┘   │   │
│   │                                                                      │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│   executeTransaction()                                                       │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  for each stage:                                                     │   │
│   │    for each operation:                                               │   │
│   │      if operation.execute() fails:                                  │   │
│   │        rollback all operations in this stage                        │   │
│   │        return failure                                                │   │
│   │    mark stage complete                                               │   │
│   │  return success                                                      │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                       TransactionMgr                                         │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                    Transaction Queue                                 │   │
│   │                                                                      │   │
│   │   ┌────┐   ┌────┐   ┌────┐   ┌────┐                                │   │
│   │   │ T1 │──►│ T2 │──►│ T3 │──►│ T4 │                                │   │
│   │   └────┘   └────┘   └────┘   └────┘                                │   │
│   │                                                                      │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                    NLinkedTree (Dependency)                          │   │
│   │                                                                      │   │
│   │   Objects tracked:                                                   │   │
│   │     • instrument_ObjectType                                         │   │
│   │     • order_ObjectType                                              │   │
│   │     • execution_ObjectType                                          │   │
│   │                                                                      │   │
│   │   Dependency rules:                                                  │   │
│   │     • T2 depends on T1 if they share any object                    │   │
│   │     • Ready queue: transactions with no pending dependencies        │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.2 ACID Properties Implementation

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ ATOMICITY                                                                    │
│                                                                              │
│   Implementation: TransactionScope with staged rollback                     │
│                                                                              │
│   execute()     ─► Success ─► Commit all operations                        │
│        │                                                                     │
│        └─► Failure ─► Rollback operations in reverse order                  │
│                                                                              │
│   Each operation implements:                                                 │
│     • execute()  - Perform the change                                       │
│     • rollback() - Undo the change                                          │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ CONSISTENCY                                                                  │
│                                                                              │
│   Implementation:                                                            │
│     • State machine enforces valid order state transitions                  │
│     • Codec validation ensures data integrity on serialization              │
│     • Invariant checks in OrderBook (price ordering)                        │
│     • Pre-conditions validated before operation execution                   │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ ISOLATION                                                                    │
│                                                                              │
│   Implementation: NLinkedTree dependency graph                              │
│                                                                              │
│   ┌───────────────────────────────────────────────────────────────────┐    │
│   │  Transaction T2 (touches Order A and Order B)                     │    │
│   │                                                                    │    │
│   │  Dependencies:                                                     │    │
│   │    • If T1 touches Order A → T2 waits for T1                      │    │
│   │    • If T3 only touches Order C → T3 runs parallel to T2          │    │
│   │                                                                    │    │
│   │  Effect: Serializable isolation for overlapping transactions      │    │
│   └───────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ DURABILITY                                                                   │
│                                                                              │
│   Implementation: FileStorage with versioned records                        │
│                                                                              │
│   ┌───────────────────────────────────────────────────────────────────┐    │
│   │  Record Format:                                                    │    │
│   │                                                                    │    │
│   │  <S[size].[valid].[id:16bytes].[version]...[body]...E>           │    │
│   │                                                                    │    │
│   │  • [size]    - 4 bytes, record length                             │    │
│   │  • [valid]   - 1 byte, record validity flag                       │    │
│   │  • [id]      - 16 bytes, unique identifier                        │    │
│   │  • [version] - 4 bytes, record version for updates                │    │
│   │  • [body]    - Variable, serialized data                          │    │
│   └───────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│   Recovery Process:                                                          │
│     1. Load file                                                            │
│     2. Parse records via StorageRecordDispatcher                            │
│     3. Decode via appropriate Codec                                         │
│     4. Restore OrderDataStorage and OrderBook                               │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.3 Transaction Operations

| Operation Type | Description | Rollback Action |
|----------------|-------------|-----------------|
| `CreateExecReportTrOperation` | Create execution report | Remove report |
| `CreateTradeExecReportTrOperation` | Create trade execution | Remove execution |
| `CreateRejectExecReportTrOperation` | Create rejection report | Remove report |
| `CreateReplaceExecReportTrOperation` | Create replace report | Remove report |
| `CreateCorrectExecReportTrOperation` | Create correction | Remove correction |
| `AddToOrderBookTrOperation` | Add order to book | Remove from book |
| `RemoveFromOrderBookTrOperation` | Remove order from book | Re-add to book |
| `EnqueueEventTrOperation` | Queue deferred event | Dequeue event |
| `MatchOrderTrOperation` | Initiate order matching | Cancel match |

**Associated Test Cases:**

| Test File | Test Cases | Coverage |
|-----------|------------|----------|
| `NLinkTreeTest.cpp` | `AddSingleNode`, `ClearTree` | Dependency tracking |
| `TransactionMgrTest.cpp` | `TransactionMgrTest.*` | Transaction manager tests |
| `TransactionScopeTest.cpp` | `TransactionScopeTest.*` | Transaction scope tests |
| `IntegrationTest.cpp` | `IntegrationTest.*` | Full transaction flow |

---

## 8. Storage & Persistence

### 8.1 Storage Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        STORAGE LAYERS                                        │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 1: In-Memory (OrderDataStorage)                                        │
│                                                                              │
│   ┌───────────────────────────────────────────────────────────────────────┐ │
│   │                     Order Maps                                         │ │
│   │                                                                        │ │
│   │   byId_: map<IdT, OrderEntry*>                                        │ │
│   │   ┌────────┬────────────────────────────────────────────────────────┐ │ │
│   │   │ ID 001 │ ─► OrderEntry { symbol: AAPL, qty: 100, price: 150.5 } │ │ │
│   │   │ ID 002 │ ─► OrderEntry { symbol: MSFT, qty: 200, price: 280.0 } │ │ │
│   │   │ ID 003 │ ─► OrderEntry { symbol: AAPL, qty: 50,  price: 151.0 } │ │ │
│   │   └────────┴────────────────────────────────────────────────────────┘ │ │
│   │                                                                        │ │
│   │   byClOrdId_: map<RawDataEntry, OrderEntry*>                          │ │
│   │   ┌──────────────┬──────────────────────────────────────────────────┐ │ │
│   │   │ "CLIENT-001" │ ─► OrderEntry (ID 001)                           │ │ │
│   │   │ "CLIENT-002" │ ─► OrderEntry (ID 002)                           │ │ │
│   │   └──────────────┴──────────────────────────────────────────────────┘ │ │
│   │                                                                        │ │
│   │   Locks: orderLock_ (tbb::mutex), execLock_ (tbb::mutex)             │ │
│   └───────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 2: Price-Sorted (OrderBook)                                            │
│                                                                              │
│   ┌───────────────────────────────────────────────────────────────────────┐ │
│   │                  Instrument: AAPL                                      │ │
│   │                                                                        │ │
│   │   BUY SIDE (Descending Price)      SELL SIDE (Ascending Price)        │ │
│   │   ┌─────────────────────────┐      ┌─────────────────────────┐       │ │
│   │   │ $151.00 ─► [Order 003]  │      │ $151.50 ─► [Order 005]  │       │ │
│   │   │ $150.50 ─► [Order 001]  │      │ $152.00 ─► [Order 006]  │       │ │
│   │   │ $150.00 ─► [Order 004]  │      │ $152.50 ─► [Order 007]  │       │ │
│   │   └─────────────────────────┘      └─────────────────────────┘       │ │
│   │          ▲                                 ▲                          │ │
│   │    Best Bid                          Best Ask                         │ │
│   │                                                                        │ │
│   │   Data Structure: std::multimap<Price, OrderEntry*, Comparator>       │ │
│   └───────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 3: File Persistence (FileStorage)                                      │
│                                                                              │
│   ┌───────────────────────────────────────────────────────────────────────┐ │
│   │                     File Format                                        │ │
│   │                                                                        │ │
│   │   ┌──────────────────────────────────────────────────────────────┐   │ │
│   │   │ Record 1: <S0042.1.ORDER001........v001.[OrderCodec data]E>  │   │ │
│   │   ├──────────────────────────────────────────────────────────────┤   │ │
│   │   │ Record 2: <S0038.1.INSTR001........v001.[InstrCodec data]E>  │   │ │
│   │   ├──────────────────────────────────────────────────────────────┤   │ │
│   │   │ Record 3: <S0045.1.EXEC0001........v001.[ExecCodec data]E>   │   │ │
│   │   ├──────────────────────────────────────────────────────────────┤   │ │
│   │   │ Record 4: <S0042.1.ORDER001........v002.[Updated data]E>     │   │ │
│   │   │                                        ▲                      │   │ │
│   │   │                                Version 2 (update)             │   │ │
│   │   └──────────────────────────────────────────────────────────────┘   │ │
│   │                                                                        │ │
│   │   Metadata Index: Tracks latest record offset per ID                  │ │
│   └───────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 4: Key-Value Persistence (LMDBStorage)                                │
│                                                                              │
│   ┌───────────────────────────────────────────────────────────────────────┐ │
│   │                     LMDB Backend                                       │ │
│   │                                                                        │ │
│   │   Location: data/data.mdb, data/lock.mdb                             │ │
│   │                                                                        │ │
│   │   Features:                                                            │ │
│   │     • Memory-mapped I/O for high-performance reads                    │ │
│   │     • ACID-compliant transactions at the storage level                │ │
│   │     • Zero-copy reads via memory mapping                              │ │
│   │     • Concurrent reader access (single writer)                        │ │
│   │     • Map doubles when a write finds it full (#148);                  │ │
│   │       starts at --lmdb-map-mb, 256 MB by default                      │ │
│   │     • An order keeps its newest version and the one                   │ │
│   │       before it; update() erases older ones (#148)                    │ │
│   │                                                                        │ │
│   │   API: put(key, value), get(key), del(key), iterate()                │ │
│   └───────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 8.2 Codec System

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         CODEC ARCHITECTURE                                   │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                    StorageRecordDispatcher                                   │
│                                                                              │
│   Incoming Buffer                                                            │
│        │                                                                     │
│        ▼                                                                     │
│   ┌────────────────────────────────────────────────────────────────────┐    │
│   │                    Record Type Router                               │    │
│   └────────────────────────────────────────────────────────────────────┘    │
│        │                                                                     │
│        ├─── INSTRUMENT_RECORDTYPE ──► InstrumentCodec ──► InstrumentEntry   │
│        │                                                                     │
│        ├─── STRING_RECORDTYPE ──────► StringTCodec ─────► StringT           │
│        │                                                                     │
│        ├─── ACCOUNT_RECORDTYPE ─────► AccountCodec ─────► AccountEntry      │
│        │                                                                     │
│        ├─── CLEARING_RECORDTYPE ────► ClearingCodec ────► ClearingEntry     │
│        │                                                                     │
│        ├─── RAWDATA_RECORDTYPE ─────► RawDataCodec ─────► RawDataEntry      │
│        │                                                                     │
│        ├─── ORDER_RECORDTYPE ───────► OrderCodec ───────► OrderEntry        │
│        │                                                                     │
│        ├─── EXECUTION_RECORDTYPE ───► none written; skipped on load         │
│        │                                                                     │
│        └─── IDLIMIT_RECORDTYPE ─────► (id limit, #81) ──► restoredIdLimit   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                      Codec Interface                                         │
│                                                                              │
│   class InstrumentCodec {   // and the other codecs alike                    │
│       static constexpr u32 FORMAT_VERSION = 1;      // #149, #172            │
│       static void encode(const InstrumentEntry &, std::string *buf,          │
│                          IdT *id, u32 *version);                             │
│       static void decode(const IdT &id, u32 formatVersion,                   │
│                          const char *buf, size_t size, InstrumentEntry *);   │
│   };                                                                         │
│                                                                              │
│   Record: a 4-byte word (type in the low 16 bits, FORMAT_VERSION in the      │
│   high 16), then the codec's fields, most followed by '.'. Fields are their  │
│   in-memory bytes, x86-64 little-endian; a string is an 8-byte size_t        │
│   length, then its bytes.                                                    │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Ids across restarts (#58, #81).** Order ids and execution ids come from the shared `IdTGenerator`, which starts at 1 in every run. Transaction ids come from a counter of their own in `app/main.cpp`, because nothing persists them. After loading, the server continues the shared counter past two points: the largest order number restored (#58), and the limit in the `IDLIMIT_RECORDTYPE` record (#81).

From then on, no id is returned before a limit above it is saved. `reserve()` saves one 10,000 ids ahead, and `getId()` saves the next one when the counter reaches it. So execution ids, whose reports aren't persisted, never repeat after a restart, even after a crash. A clean stop saves the exact next id (`releaseReservation()`), so the next run continues without a gap; after a crash, ids jump by up to one block.

The record is keyed (0,1), and only its newest version is kept. A data directory written with it can't be loaded by an older server, which throws on the unknown record type.

**Orders a crash left without their fills (#207).** An order's booking and its fills are separate transactions, each persisted when it commits. The booking writes the order as accepted, and its match only starts the fills, which a later transaction writes. A crash between the two leaves the order on disk as booked, though it had traded. `finishLoad()` restores orders in id order, which is the order they arrived in. It books an open order only if it wouldn't have traded with one that arrived before it: one the matcher's own search finds among the orders booked so far (`OrderMatcher::findContra()`), or one already left out (`OrderMatcher::tradesWith()`). An open market order is always left out, since its own transaction trades or cancels it. `ordersToMatch()` lists those left out, in that order, and the server matches each (`Processor::matchRestored()`) before it takes orders, with the operations its booking ran. So each trades as it did, with the earlier order and at its price, and its fills persist and report like any others. If they don't reach disk either, the next restart finds the same orders and matches them again.

**A record has what its decision left (#411).** The transaction that persists a fill can run after a later fill of the order was applied (§4.3), so a record is written from the figures its decision left (`OrderSaver::save(order, figures)`, which `OrderCodec::encode()` and `PGRequestBuilder::fromOrder()` take), not from the order as it is when the transaction runs. A record that counted the later fill, committed before that fill's own transaction, left the order on disk with a trade its counterparty never had: restored, the counterparty was open, and the restart matching above filled it from the order a second time. The layout is unchanged, so the codec's version is too.

**Record format (#149).** A record's first 4 bytes hold its type in the low 16 bits and, in the high 16, the version of the layout its codec wrote (`FORMAT_VERSION`). The order codec's is 4: version 1 added an order's execution instructions (#172), 2 whether a stop order's stop was reached (#223), 3 its ClOrdID, its original's, its source and its destination (#192), and 4 the start of the business day its day figures are for (#244). Every other codec's is 0, so their records' first bytes are the type alone, as they always were. The dispatcher refuses a version newer than the codec reads, by name. Each decoder checks that the record holds every field it reads and that it has read the whole record, so a record cut short or with bytes after its last field is refused rather than misread. `static_assert`s pin the size of every field stored as its in-memory bytes.

`StorageRecordDispatcherTest` holds golden records: the bytes each record type was written as before #149, and an order's in every format version since. The build must write the newest of each and read every field back from all of them, and every prefix of them, and every one with a byte more, must be refused. To change a layout on purpose, bump that codec's `FORMAT_VERSION`, keep decoding the old version, and add golden records for the new one; running the suite with `GOLDEN_PRINT=1` prints what the build writes.

**On this branch** the instrument codec's is 1 (#172): version 0's strings, then the instrument's type and an FX pair's base and term currencies. An older record restores without the three, and a value past the last this build knows is refused. `StorageRecordDispatcherTest` holds the instrument's record in both versions.

**Associated Test Cases:**

| Test File | Test Cases | Coverage |
|-----------|------------|----------|
| `CodecsTest.cpp` | `InstrumentCodecFilled`, `OrderCodecFilled`, etc. | All codec types |
| `FileStorageTest.cpp` | `FileStorageTest.*` | File I/O operations |
| `StorageRecordDispatcherTest.cpp` | `StorageRecordDispatcherTest.*` | Record routing; golden records (#149) |
| `OrderStorageTest.cpp` | `OrderStorageTest.*` | Order storage operations |
| `WideDataStorageTest.cpp` | `WideDataStorageTest.*` | Reference data storage |
| `LMDBStorageTest.cpp` | `LMDBStorageTest.*` | LMDB key-value backend |
| `ProcessorBench.cpp` | `BM_ProcessNewOrderWithMatchRecorded` | A match's records encoded by the dispatcher, without LMDB's write (#442) |

---

## 9. Test Coverage Matrix

`ctest -N` lists every registered test. This section names the files, not how many tests each holds: those counts went stale here, as they did in README (#11).

### 9.1 Component Coverage Summary

| Component | Tests | Benchmarks |
|-----------|-------|------------|
| **Codecs** | `CodecsTest.cpp`; `StorageRecordDispatcherTest.cpp`, which holds the golden records (#149) | `ProcessorBench.cpp` (`BM_ProcessNewOrderWithMatchRecorded`, through the dispatcher, #442) |
| **State Machine** | `StatesTest.cpp`, `StateMachineTest.cpp`, both through `StateMachineHelper` | `StateMachineBench.cpp` |
| **Order Book & Matching** | `OrderBookTest.cpp`, `OrderMatcherTest.cpp` | `OrderMatchingBench.cpp` |
| **Queues** | `IncomingQueuesTest.cpp`, `OutgoingQueuesTest.cpp`, `QueuesManagerTest.cpp` | `EventProcessingBench.cpp` |
| **Processor** | `ProcessorTest.cpp`, `DeferedEventsTest.cpp`, `IntegrationTest.cpp` | `ProcessorBench.cpp` |
| **Transactions** | `TransactionMgrTest.cpp`, `TransactionScopeTest.cpp`, `TrOperationsTest.cpp`, `NLinkTreeTest.cpp` | - |
| **Concurrency** | `TaskManagerTest.cpp` | `ProcessorBench.cpp` (`BM_TaskManagerThroughput`, `BM_TaskManagerTrades`) |
| **Storage** | `LMDBStorageTest.cpp`, `OrderStorageTest.cpp`, `WideDataStorageTest.cpp`; `FileStorageTest.cpp` for the legacy file storage | - |
| **Low-Latency** | `CacheAlignedAtomicTest.cpp`, `CpuAffinityHugePagesTest.cpp`, `NumaAllocatorTest.cpp`, `TransactionScopePoolTest.cpp` | `TransactionScopePoolBench.cpp`, `NumaAllocatorBench.cpp`, `OrderParamsLayoutBench.cpp` |
| **Server (`app/`)** | `ClientMessageParserTest.cpp`, `JsonSerializerTest.cpp`, `WsSessionTest.cpp`, `WsServerLimitsTest.cpp`, `WsAuthTest.cpp`, `ServerArgsTest.cpp`, `ServerDataTest.cpp`, `SeedDataArgsTest.cpp`; the ctest scripts `ServerThatCannotStartExits` and `SeedDataIfEmptySeedsOnlyADirectoryWithoutInstruments`, and `SeedDataRunsCleanly`; with `BUILD_FUZZ=ON`, `test/fuzz/` | `SessionManagerBench.cpp` |
| **PostgreSQL** | `PGEnumStringsTest.cpp`, `PGRequestBuilderTest.cpp`; with `BUILD_PG=ON`, `PGWriteBehindTest.cpp`, whose integration cases need `PG_CONNECTION_STRING` | - |
| **CI runner** | the ctest script `JobStartedHookDecides` (`JobStartedHookTest.sh`): the job-started hook's verdicts, a fork's pull request and anything Dependabot sent refused (#143, #168); the ctest script `CiTestedTreeDecides` (`CiTestedTreeTest.sh`): when the push of a merge may skip the jobs its pull request's run passed (#314) | - |
| **Other** | `DocConsistencyTest.cpp`, `EventBenchmarkTest.cpp`, `FiltersTest.cpp`, `SubscriptionTest.cpp`, `IdTGeneratorTest.cpp`, `LoggerTest.cpp`; the ctest script `CpuBusyDecides` (`CpuBusyTest.sh`): the CPUs a pinned benchmark comparison watches, and the busiest of them (#406); the ctest script `BenchmarkBaselineKeepsItsConfig` (`BenchmarkBaselineTest.sh`): a saved baseline keeps a copy of the `config/` its binary reads, a comparison hands it that copy, and a benchmark that fails in the first round stops a comparison there (#415), and each `commit:` line, the `.info`'s too, names the commit of the tree its binary was built from, or says that tree is unknown or gone (#448); the ctest script `BenchmarkBusyStartIsInconclusive` (`BenchmarkBusyStartTest.sh`): a comparison in which a pinned run started with a watched CPU busy is inconclusive, naming the run (#422) | - |

### 9.2 State Machine Tests

- **`StatesTest.cpp`** drives the Rcvd_New state one event at a time: the transitions out of it, the refusals a new order or a replacement can meet (a duplicate ClOrdID, an invalid side, an empty or unknown id), and what a reject keeps of the order.
- **`StateMachineTest.cpp`** runs orders through the real `OrderState` machine: a full New, PartFill, Filled lifecycle; expiry, done for day, suspension and rejection; cancels and replaces through NoCnlReplace, GoingCancel and GoingReplace; trade corrections; and orders whose states stay independent.
- **`ProcessorTest.cpp`** covers what the processor adds around the machine, such as the booking mark (#83), restored orders (#165, #207) and market orders (#209, #211).

### 9.3 Test Utilities

| Utility | Location | Purpose |
|---------|----------|---------|
| `TestTransactionContext` | `TestAux.h/cpp` | Mock ACID::Scope for transaction testing |
| `DummyOrderSaver` | `TestAux.h` | No-op order persistence |
| `check()` | `TestAux.h/cpp` | Assertion with error messages |
| `uniqueTestPath()` | `TestAux.h` | A per-test-case path, so tests can run in parallel (#10) |
| `SingletonFixture`, `OrderStorageFixture`, ... | `TestFixtures.h` | Create and destroy the singletons per test |
| `OrderStateWrapper` | `StateMachineHelper.h/cpp` | State machine event processing |
| `TestInQueueObserver` | Various test files | Queue event capture |
| `TestOrderFunctor` | `OrderBookTest.cpp`, `mocks/MockOrderBook.h` | Mock order matching |
| `TestFileStorageObserver` | `FileStorageTest.cpp` | File storage callbacks |
| `TestDataStorageRestore` | `StorageRecordDispatcherTest.cpp` | Data restoration verification |

---

## 10. Appendix: File Reference

Every `.h`, `.cpp`, `.py` and `.sh` file under `src/`, `test/` and `bench/` is named here, and `DocConsistencyTest.FileReferenceNamesEveryFile` fails naming any that isn't (#261). A new file needs its line.

### 10.1 Source Files

| Category | Files |
|----------|-------|
| **Core Pipeline** | `Processor.h/cpp`, `IncomingQueues.h/cpp`, `OutgoingQueues.h/cpp`, `QueuesManager.h/cpp`, `ExpiryScheduler.h/cpp` |
| **State Machine** | `StateMachine.h/cpp`, `StateMachineDef.h`, `OrderStateMachineImpl.h/cpp`, `OrderStates.h/cpp`, `OrderStateEvents.h` |
| **Order Matching** | `OrderMatcher.h/cpp`, `OrderBookImpl.h/cpp` |
| **Transactions** | `TransactionDef.h`, `TransactionMgr.h/cpp`, `TransactionScope.h/cpp`, `TransactionScopePool.h`, `TrOperations.h/cpp`, `NLinkedTree.h/cpp` |
| **Storage** | `FileStorage.h/cpp`, `FileStorageDef.h`, `OrderStorage.h/cpp`, `StorageRecordDispatcher.h/cpp`, `LMDBStorage.h/cpp` |
| **Data Models** | `DataModelDef.h/cpp`, `TypesDef.h`, `QueuesDef.h`, `EventDef.h`, `TasksDef.h` |
| **Codecs** | `OrderCodec.h/cpp`, `InstrumentCodec.h/cpp`, `AccountCodec.h/cpp`, `ClearingCodec.h/cpp`, `RawDataCodec.h/cpp`, `StringTCodec.h/cpp`, `CodecChecks.h` |
| **Concurrency** | `TaskManager.h/cpp`, `AllocateCache.h/cpp` |
| **Low-Latency** | `TransactionScopePool.h`, `CacheAlignedAtomic.h`, `CpuAffinity.h`, `HugePages.h`, `NumaAllocator.h` |
| **Subscriptions** | `SubscrManager.h/cpp`, `SubscriptionLayerImpl.h/cpp`, `SubscriptionLayerDef.h`, `SubscriptionDef.h`, `FilterImpl.h/cpp`, `EntryFilter.h/cpp`, `OrderFilter.h/cpp` |
| **Events** | `EventManager.h/cpp`, `DeferedEvents.h`, `CancelOrderDeferedEvent.cpp`, `ExecutionDeferedEvent.cpp`, `MatchOrderDeferedEvent.cpp`, `ReleaseStopDeferedEvent.cpp`, `ReplaceRequestDeferedEvent.cpp`, `StateChangeDeferedEvent.cpp` |
| **FX swaps** | `MatchSwapOrderDeferedEvent.cpp`, `SwapExecutionDeferedEvent.cpp` |
| **PostgreSQL** | `PGWriteBehind.h/cpp`, `PGRequestBuilder.h/cpp`, `PGWriteRequest.h`, `PGEnumStrings.h` (optional) |
| **Utilities** | `Logger.h/cpp`, `IdTGenerator.h/cpp`, `ExchUtils.h/cpp`, `Singleton.h`, `WideDataStorage.h/cpp`, `WideDataLazyRef.h` |

### 10.2 Test Files

| Category | Files |
|----------|-------|
| **Google Test** | `CacheAlignedAtomicTest.cpp`, `ClientMessageParserTest.cpp`, `CodecsTest.cpp`, `CpuAffinityHugePagesTest.cpp`, `DeferedEventsTest.cpp`, `DocConsistencyTest.cpp`, `EventBenchmarkTest.cpp`, `ExpirySchedulerTest.cpp`, `FileStorageTest.cpp`, `FiltersTest.cpp`, `IdTGeneratorTest.cpp`, `IncomingQueuesTest.cpp`, `IntegrationTest.cpp`, `JsonSerializerTest.cpp`, `LMDBStorageTest.cpp`, `LoggerTest.cpp`, `NLinkTreeTest.cpp`, `NumaAllocatorTest.cpp`, `OrderBookTest.cpp`, `OrderMatcherTest.cpp`, `OrderStorageTest.cpp`, `OutgoingQueuesTest.cpp`, `PGEnumStringsTest.cpp`, `PGRequestBuilderTest.cpp`, `PGSchemaTest.cpp`, `PGWriteBehindTest.cpp`, `ProcessorTest.cpp`, `QueuesManagerTest.cpp`, `SeedDataArgsTest.cpp`, `ServerArgsTest.cpp`, `ServerDataTest.cpp`, `StateMachineTest.cpp`, `StatesTest.cpp`, `StorageRecordDispatcherTest.cpp`, `SubscriptionTest.cpp`, `TaskManagerTest.cpp`, `TransactionMgrTest.cpp`, `TransactionScopePoolTest.cpp`, `TransactionScopeTest.cpp`, `TrOperationsTest.cpp`, `WideDataStorageTest.cpp`, `WsAuthTest.cpp`, `WsAuthorizationTest.cpp`, `WsCredentialsTest.cpp`, `WsServerLimitsTest.cpp`, `WsSessionTest.cpp` |
| **This branch's** | `EnumCouplingTest.cpp`, `FxSwapTest.cpp`, and with `BUILD_FIX=ON`: `FixCredentialsTest.cpp`, `FixEndToEndTest.cpp`, `FixFullPipelineTest.cpp`, `FixGatewayTest.cpp`, `FixRotatingLogTest.cpp`, `FixTlsTest.cpp` |
| **Utilities** | `TestAux.h/cpp`, `StateMachineHelper.h/cpp`, `TestFixtures.h`, `TestMain.cpp` |
| **Mock Objects** | `mocks/MockDefered.h`, `mocks/MockOrderBook.h`, `mocks/MockQueues.h`, `mocks/MockStorage.h`, `mocks/MockTasks.h`, `mocks/MockTransaction.h` |
| **ctest scripts** | `ServerStartupTest.sh`, `SeedDataIfEmptyTest.sh`, `JobStartedHookTest.sh`, `CiTestedTreeTest.sh`, `CpuBusyTest.sh`, `BenchmarkBaselineTest.sh`, `BenchmarkBusyStartTest.sh` |
| **FIX ctest scripts** (`BUILD_FIX=ON`) | `FixLogonTest.py`, `FixRefusalTest.py`, `FixResendTest.py`, `FixTlsTest.py`, and the client they share, `fix_client.py`; with `BUILD_BENCHMARKS=ON` too, `FixBenchConfigTest.sh` |
| **Fuzzing** | `fuzz/fuzz_client_message.cpp`, `fuzz/standalone_main.cpp`, `fuzz/corpus/` (`BUILD_FUZZ=ON`) |

### 10.3 Benchmark Files

| File | Purpose |
|------|---------|
| `EventProcessingBench.cpp` | Event queue throughput measurement |
| `OrderMatchingBench.cpp` | The order book: add, remove, find and top of book at the book's depth (#293); no matching |
| `StateMachineBench.cpp` | State transition performance |
| `TransactionScopePoolBench.cpp` | Lock-free object pool allocation |
| `NumaAllocatorBench.cpp` | NUMA-aware allocation performance |
| `OrderParamsLayoutBench.cpp` | Field layout optimization |
| `ProcessorBench.cpp` | The real path: new order, match, cancel and replace (#457) through a `Processor`, an IOC's remainder and an expiry (#426), a restatement (#451), the reject of an order `isValid()` refuses and of a market order with no market (#435), a match whose records are encoded as the server's dispatcher encodes them (#442), and `TaskManager` throughput, with orders that don't trade (#25) and with orders that do (#290), each reporting into a queue that keeps nothing, as production's keep none (#447) |
| `SessionManagerBench.cpp` | WebSocket broadcast and book-update fan-out (#159), and a fill's report pushed through `WsOutQueues` (#442) |
| `FixGatewayBench.cpp` | The FIX gateway (`BUILD_FIX=ON`): enum conversion, NewOrderSingle translation, the gateway's ExecutionReport builder, a report's way to a logged-on session, and a new order with its acknowledgement (#395), a session's cancels, replaces and status requests and the refusals it is sent (#423), the fan-out, swap and single-leg matching, and a FIX order's fill end to end |

---

## Document Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | January 2026 | Generated | Initial architecture document |
| 1.1 | January 2026 | Generated | Added ultra low-latency optimizations: TransactionScopePool, CacheAlignedAtomic, CpuAffinity, HugePages |
| 1.2 | February 2026 | Generated | Updated file counts (101 src, 45 test, 7 bench), added LMDBStorage, NumaAllocator, PG write-behind, expanded namespace listing, updated test/benchmark appendix |
| 1.3 | October 2026 | dudleylane, Claude | Corrected against the code (#171): state and transition counts (now checked by DocConsistencyTest), the new order flow, execution records, the test sections and the appendix's file lists |
| 1.4 | October 2026 | dudleylane, Claude | Stop orders (#223): the stop book, the matcher's trigger, the stop order flow |
| 1.5 | October 2026 | dudleylane, Claude | The stop's TRIGGERED report (#236) |
| 1.6 | October 2026 | dudleylane, Claude | A replace's stopPx (#238) |
| 1.7 | October 2026 | dudleylane, Claude | The last trade's price survives a restart (#237) |
| 1.8 | October 2026 | dudleylane, Claude | Time in force: IOC, FOK, DAY and GTD (#240) |
| 1.9 | October 2026 | dudleylane, Claude | Business days, day figures and restatements (#244) |

---

*This document was generated from codebase analysis of the OrderProcessor system.*
