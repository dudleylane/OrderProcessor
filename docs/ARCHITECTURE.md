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
- **State machine-based** order lifecycle management (14 states, 47+ transitions)
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
│                    (FIX Gateway, REST API, Market Data)                     │
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
│  │  • 14 States      │ │ • Price-   │ │ • Operation      │                  │
│  │  • 47+ Transitions│ │   Time     │ │   Collection     │                  │
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
│  ┌──────────────────────┐  ┌────────────────────────────────────────────┐   │
│  │   InterLockCache     │  │              NLinkedTree                   │   │
│  │  (Wait-Free Pool)    │  │       (Transaction Dependency)             │   │
│  └──────────────────────┘  └────────────────────────────────────────────┘   │
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
| `InterlockCacheTest.cpp` | `PopAndPushBasic` | Underlying cache mechanism |

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
├─────────────────────────────────────────────────────────────────┤
│ Data Structure:                                                  │
│   std::multimap<Price, OrderEntry*, PriceComparator>            │
│   • Buy side: Greater-than comparator (best price first)        │
│   • Sell side: Less-than comparator (best price first)          │
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
│  │   │    Market orders: always match              │         │  │
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
│  │   │ 5. Continue matching if qty remaining       │         │  │
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
| `OrderMatchingBench.cpp` | `BM_OrderMatching*` | Matching performance |

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
│   • Add to OrderDataStorage                                              │
│   • Transition: INITIAL → RCVD_NEW → NEW                                │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ TransactionScope collects operations:                                    │
│   • CreateExecReportTrOperation (NEW acknowledgment)                    │
│   • AddToOrderBookTrOperation                                           │
│   • EnqueueEventTrOperation (deferred match event)                      │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ OrderMatcher.match()                                                     │
│   • Find opposite side orders                                            │
│   • Check price compatibility                                            │
│   • Create ExecutionDeferedEvent if matched                             │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ TransactionMgr.enqueue(transaction)                                      │
│   • Add to NLinkedTree dependency graph                                 │
│   • Mark objects as "in transaction"                                    │
│   • Schedule execution                                                   │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ TaskManager executes transaction:                                        │
│   • OrderBook.add(order)                                                │
│   • OutgoingQueues.push(ExecReport)                                     │
│   • Process deferred execution events                                   │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ FileStorage.save()                                                       │
│   • Serialize via OrderCodec                                            │
│   • Write versioned record                                              │
│   • Update metadata                                                      │
└──────────────────────────────────────────────────────────────────────────┘
       │
       ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ OutgoingQueues → External System                                         │
│   • Execution Report (NEW)                                              │
│   • Execution Report (FILL) if matched                                  │
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
│ Processor.onEvent(OrderCancelEvent)                                      │
│   • Locate order in OrderDataStorage                                    │
│   • Validate cancel is allowed                                          │
└──────────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ OrderStateMachine.onCancelReceived()                                     │
│   • Transition: NEW/PARTFILL → GOING_CANCEL                             │
└──────────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ OrderStateMachine.onExecCancel()                                         │
│   • Transition: GOING_CANCEL → CNCL_REPLACED                            │
│   • Create RemoveFromOrderBookTrOperation                               │
│   • Create CreateExecReportTrOperation (CANCELED)                       │
└──────────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Transaction Execution:                                                   │
│   • OrderBook.remove(order)                                             │
│   • OutgoingQueues.push(ExecReport CANCELED)                            │
│   • FileStorage.update(order)                                           │
└──────────────────────────────────────────────────────────────────────────┘
```

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
│   • Execution records persisted                                         │
└──────────────────────────────────────────────────────────────────────────┘
```

---

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
| RCVD_NEW | onExternalOrderReject | REJECTED | Send REJECT |
| RCVD_NEW | onRplOrderReceived | PEND_REPLACE | Queue replace |
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
| NO_CNL_REPLACE | onReplaceReceived | GOING_REPLACE | Begin replace |
| GOING_CANCEL | onExecCancel | CNCL_REPLACED | Remove from book |
| GOING_CANCEL | onCancelRejected | NO_CNL_REPLACE | Cancel failed |
| GOING_REPLACE | onExecReplace | CNCL_REPLACED | Update order |
| GOING_REPLACE | onReplaceRejected | NO_CNL_REPLACE | Replace failed |

**Associated Test Cases:**

| Test File | Test Cases | Coverage |
|-----------|------------|----------|
| `StatesTest.cpp` | 47 individual state tests | All transitions |
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
│   │                    InterLockCache                                    │   │
│   │                                                                      │   │
│   │   ┌────┐   ┌────┐   ┌────┐   ┌────┐   ┌────┐                       │   │
│   │   │Obj1│──►│Obj2│──►│Obj3│──►│Obj4│──►│NULL│                       │   │
│   │   └────┘   └────┘   └────┘   └────┘   └────┘                       │   │
│   │      ▲                                   ▲                          │   │
│   │      │                                   │                          │   │
│   │   nextFree_                          nextNull_                      │   │
│   │   (atomic)                           (atomic)                       │   │
│   │                                                                      │   │
│   │   pop() : atomic_compare_exchange_strong                            │   │
│   │   push(): atomic_compare_exchange_strong                            │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
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
   │   • InterLockCache: Wait-free object allocation                      │
   │   • Atomic processor attachment                                       │
   │   • Atomic counter updates                                           │
   │   • NumaAllocator: NUMA-local memory binding via mbind()             │
   └───────────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Cases | Coverage |
|-----------|------------|----------|
| `InterlockCacheTest.cpp` | `PopAndPushBasic`, `ExceedCacheSize` | Lock-free cache |
| `InterlockCacheBench.cpp` | `BM_InterlockCache*` | Cache performance |
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
│        └─── EXECUTION_RECORDTYPE ───► (Execution handling) ► ExecutionsT    │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                      Codec Interface                                         │
│                                                                              │
│   template<typename T>                                                       │
│   class Codec {                                                              │
│       static size_t encode(const T& obj, char* buffer);                     │
│       static size_t decode(T& obj, const char* buffer);                     │
│       static size_t size(const T& obj);                                     │
│   };                                                                         │
│                                                                              │
│   Binary Format: Fixed fields + variable length data                        │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Associated Test Cases:**

| Test File | Test Cases | Coverage |
|-----------|------------|----------|
| `CodecsTest.cpp` | `InstrumentCodecFilled`, `OrderCodecFilled`, etc. | All codec types |
| `FileStorageTest.cpp` | `FileStorageTest.*` | File I/O operations |
| `StorageRecordDispatcherTest.cpp` | `StorageRecordDispatcherTest.*` | Record routing |
| `OrderStorageTest.cpp` | `OrderStorageTest.*` | Order storage operations |
| `WideDataStorageTest.cpp` | `WideDataStorageTest.*` | Reference data storage |
| `LMDBStorageTest.cpp` | `LMDBStorageTest.*` | LMDB key-value backend |

---

## 9. Test Coverage Matrix

### 9.1 Component Coverage Summary

| Component | Unit Tests | Integration Tests | Benchmarks | Total Test Lines |
|-----------|------------|-------------------|------------|------------------|
| **Codecs** | CodecsTest.cpp (340), testCodecs.cpp (404) | - | - | 744 |
| **State Machine** | testStates.cpp (1,380) | testStateMachine.cpp (3,709) | StateMachineBench.cpp | 5,089+ |
| **Order Book** | testOrderBook.cpp (347) | testIntegral.cpp (579) | OrderMatchingBench.cpp | 926+ |
| **Queues** | testIncomingQueues.cpp (279) | testIntegral.cpp | EventProcessingBench.cpp | 858+ |
| **Processor** | testProcessor.cpp (289) | testIntegral.cpp | EventProcessingBench.cpp | 868+ |
| **Transactions** | NLinkTreeTest.cpp (51), testNLinkTree.cpp (484) | testIntegral.cpp | - | 1,114 |
| **Storage** | testFileStorage.cpp (289), testStorageRecordDispatcher.cpp (559) | testIntegral.cpp | - | 1,427 |
| **Low-Latency** | CacheAlignedAtomicTest.cpp, CpuAffinityHugePagesTest.cpp, NumaAllocatorTest.cpp, TransactionScopePoolTest.cpp | - | TransactionScopePoolBench.cpp, NumaAllocatorBench.cpp, OrderParamsLayoutBench.cpp | - |
| **LMDB Storage** | LMDBStorageTest.cpp | - | - | - |
| **PostgreSQL** | PGEnumStringsTest.cpp, PGRequestBuilderTest.cpp, PGWriteBehindTest.cpp | - | - | - |
| **Concurrency** | InterlockCacheTest.cpp (93), testInterlockCache.cpp (153) | testTaskManager.cpp (238) | InterlockCacheBench.cpp | 484+ |

### 9.2 Test File Details

#### Google Test Files (33 files, 463 tests)

| Category | Test Files |
|----------|------------|
| **Core** | `CodecsTest.cpp`, `IncomingQueuesTest.cpp`, `OutgoingQueuesTest.cpp`, `InterlockCacheTest.cpp`, `NLinkTreeTest.cpp`, `ProcessorTest.cpp`, `StateMachineTest.cpp`, `StatesTest.cpp`, `OrderBookTest.cpp`, `OrderMatcherTest.cpp`, `OrderStorageTest.cpp` |
| **Transactions** | `TransactionMgrTest.cpp`, `TransactionScopeTest.cpp`, `TransactionScopePoolTest.cpp`, `TrOperationsTest.cpp` |
| **Storage** | `FileStorageTest.cpp`, `StorageRecordDispatcherTest.cpp`, `WideDataStorageTest.cpp`, `LMDBStorageTest.cpp` |
| **Low-Latency** | `CacheAlignedAtomicTest.cpp`, `CpuAffinityHugePagesTest.cpp`, `NumaAllocatorTest.cpp` |
| **PostgreSQL** | `PGEnumStringsTest.cpp`, `PGRequestBuilderTest.cpp`, `PGWriteBehindTest.cpp` |
| **Other** | `DeferedEventsTest.cpp`, `EventBenchmarkTest.cpp`, `FiltersTest.cpp`, `IdTGeneratorTest.cpp`, `QueuesManagerTest.cpp`, `SubscriptionTest.cpp`, `TaskManagerTest.cpp`, `IntegrationTest.cpp` |

#### Legacy Tests (10 files, retained for reference)

| Test File | Coverage |
|-----------|----------|
| `testStateMachine.cpp` | Full state machine lifecycle |
| `testStates.cpp` | All 47 state transitions |
| `testOrderBook.cpp` | Order book operations |
| `testIncomingQueues.cpp` | Queue operations |
| `testProcessor.cpp` | Processor coordination |
| `testIntegral.cpp` | End-to-end integration |
| `testFileStorage.cpp` | File persistence |
| `testStorageRecordDispatcher.cpp` | Record dispatching |
| `testNLinkTree.cpp` | Dependency tree |
| `testCodecs.cpp` | All codecs |

### 9.3 State Machine Test Coverage

| State | Transitions Tested | Test Functions |
|-------|-------------------|----------------|
| **RCVD_NEW** | 6 | `testRcvdNew2New_onOrderReceived`, `testRcvdNew2Rejected_onRecvOrderRejected`, `testRcvdNew2PendReplace_onRplOrderReceived`, `testRcvdNew2Rejected_onRecvRplOrderRejected`, `testRcvdNew2Rejected_onExternalOrderReject`, `testRcvdNew2New_onExternalOrder` |
| **PEND_NEW** | 3 | `testPendNew2New_onOrderAccepted`, `testPendNew2Rejected_onOrderRejected`, `testPendNew2Expired_onExpired` |
| **PEND_REPLACE** | 3 | `testPendRepl2New_onReplace`, `testPendRepl2Rejected_onRplOrderRejected`, `testPendRepl2Expired_onRplOrderExpired` |
| **NEW** | 6 | `testNew2PartFill_onTradeExecution`, `testNew2Filled_onTradeExecution`, `testNew2Rejected_onOrderRejected`, `testNew2Expired_onExpired`, `testNew2DoneForDay_onFinished`, `testNew2Suspended_onSuspended` |
| **PARTFILL** | 7 | `testPartFill2PartFill_onTradeExecution`, `testPartFill2Filled_onTradeExecution`, `testPartFill2PartFill_onTradeCrctCncl`, `testPartFill2New_onTradeCrctCncl`, `testPartFill2Expired_onExpired`, `testPartFill2DoneForDay_onFinished`, `testPartFill2Suspended_onSuspended` |
| **FILLED** | 5 | `testFilled2PartFill_onTradeCrctCncl`, `testFilled2New_onTradeCrctCncl`, `testFilled2Expired_onExpired`, `testFilled2DoneForDay_onFinished`, `testFilled2Suspended_onSuspended` |
| **EXPIRED** | 1 | `testExpired2Expired_onTradeCrctCncl` |
| **DONEFORDAY** | 4 | `testDoneForDay2PartFill_onNewDay`, `testDoneForDay2New_onNewDay`, `testDoneForDay2DoneForDay_onTradeCrctCncl`, `testDoneForDay2Suspended_onSuspended` |
| **SUSPENDED** | 5 | `testSuspended2PartFill_onContinue`, `testSuspended2New_onContinue`, `testSuspended2Expired_onExpired`, `testSuspended2DoneForDay_onFinished`, `testSuspended2Suspended_onTradeCrctCncl` |
| **NO_CNL_REPLACE** | 2 | `testNoCnlReplace2GoingCancel_onCancelReceived`, `testNoCnlReplace2GoingReplace_onReplaceReceived` |
| **GOING_CANCEL** | 2 | `testGoingCancel2NoCnlReplace_onCancelRejected`, `testGoingCancel2CnclReplaced_onExecCancel` |
| **GOING_REPLACE** | 2 | `testGoingReplace2NoCnlReplace_onReplaceRejected`, `testGoingReplace2CnclReplaced_onExecReplace` |
| **CNCL_REPLACED** | 1 | `testCnclReplaced2CnclReplaced_onTradeCrctCncl` |

**Total: 47 state transition tests**

### 9.4 Test Utilities

| Utility | Location | Purpose |
|---------|----------|---------|
| `TestTransactionContext` | `TestAux.h/cpp` | Mock ACID::Scope for transaction testing |
| `DummyOrderSaver` | `TestAux.h/cpp` | No-op order persistence |
| `check()` | `TestAux.h/cpp` | Assertion with error messages |
| `OrderStateWrapper` | `StateMachineHelper.h/cpp` | State machine event processing |
| `TestInQueueObserver` | Various test files | Queue event capture |
| `TestOrderFunctor` | Various test files | Mock order matching |
| `TestFileStorageObserver` | `testFileStorage.cpp` | File storage callbacks |
| `TestDataStorageRestore` | `testStorageRecordDispatcher.cpp` | Data restoration verification |

---

## 10. Appendix: File Reference

### 10.1 Source Files (101 total: 54 .h, 47 .cpp)

| Category | Files |
|----------|-------|
| **Core Pipeline** | `Processor.h/cpp`, `IncomingQueues.h/cpp`, `OutgoingQueues.h/cpp`, `QueuesManager.h/cpp` |
| **State Machine** | `StateMachine.h/cpp`, `StateMachineDef.h`, `OrderStateMachineImpl.h/cpp`, `OrderStates.h/cpp`, `OrderStateEvents.h` |
| **Order Matching** | `OrderMatcher.h/cpp`, `OrderBookImpl.h/cpp` |
| **Transactions** | `TransactionDef.h`, `TransactionMgr.h/cpp`, `TransactionScope.h/cpp`, `TransactionScopePool.h`, `TrOperations.h/cpp`, `NLinkedTree.h/cpp` |
| **Storage** | `FileStorage.h/cpp`, `FileStorageDef.h`, `OrderStorage.h/cpp`, `StorageRecordDispatcher.h/cpp`, `LMDBStorage.h/cpp` |
| **Data Models** | `DataModelDef.h/cpp`, `TypesDef.h`, `QueuesDef.h`, `EventDef.h`, `TasksDef.h` |
| **Codecs** | `OrderCodec.h/cpp`, `InstrumentCodec.h/cpp`, `AccountCodec.h/cpp`, `ClearingCodec.h/cpp`, `RawDataCodec.h/cpp`, `StringTCodec.h/cpp` |
| **Concurrency** | `TaskManager.h/cpp`, `InterLockCache.h/cpp`, `AllocateCache.h/cpp` |
| **Low-Latency** | `TransactionScopePool.h`, `CacheAlignedAtomic.h`, `CpuAffinity.h`, `HugePages.h`, `NumaAllocator.h` |
| **Subscriptions** | `SubscrManager.h/cpp`, `SubscriptionLayerImpl.h/cpp`, `SubscriptionLayerDef.h`, `SubscriptionDef.h`, `FilterImpl.h/cpp`, `EntryFilter.h/cpp`, `OrderFilter.h/cpp` |
| **Events** | `EventManager.h/cpp`, `DeferedEvents.h`, `CancelOrderDeferedEvent.cpp`, `ExecutionDeferedEvent.cpp`, `MatchOrderDeferedEvent.cpp` |
| **PostgreSQL** | `PGWriteBehind.h/cpp`, `PGRequestBuilder.h/cpp`, `PGWriteRequest.h`, `PGEnumStrings.h` (optional) |
| **Utilities** | `Logger.h/cpp`, `IdTGenerator.h/cpp`, `ExchUtils.h/cpp`, `Singleton.h`, `WideDataStorage.h/cpp`, `WideDataLazyRef.h` |

### 10.2 Test Files (45 total)

| Category | Files |
|----------|-------|
| **Google Test (33)** | `CacheAlignedAtomicTest.cpp`, `CodecsTest.cpp`, `CpuAffinityHugePagesTest.cpp`, `DeferedEventsTest.cpp`, `EventBenchmarkTest.cpp`, `FileStorageTest.cpp`, `FiltersTest.cpp`, `IdTGeneratorTest.cpp`, `IncomingQueuesTest.cpp`, `IntegrationTest.cpp`, `InterlockCacheTest.cpp`, `LMDBStorageTest.cpp`, `NLinkTreeTest.cpp`, `NumaAllocatorTest.cpp`, `OrderBookTest.cpp`, `OrderMatcherTest.cpp`, `OrderStorageTest.cpp`, `OutgoingQueuesTest.cpp`, `PGEnumStringsTest.cpp`, `PGRequestBuilderTest.cpp`, `PGWriteBehindTest.cpp`, `ProcessorTest.cpp`, `QueuesManagerTest.cpp`, `StateMachineTest.cpp`, `StatesTest.cpp`, `StorageRecordDispatcherTest.cpp`, `SubscriptionTest.cpp`, `TaskManagerTest.cpp`, `TransactionMgrTest.cpp`, `TransactionScopePoolTest.cpp`, `TransactionScopeTest.cpp`, `TrOperationsTest.cpp`, `WideDataStorageTest.cpp` |
| **Utilities** | `TestAux.h/cpp`, `StateMachineHelper.h/cpp`, `TestFixtures.h`, `TestMain.cpp` |
| **Mock Objects** | `mocks/MockDefered.h`, `mocks/MockOrderBook.h`, `mocks/MockQueues.h`, `mocks/MockStorage.h`, `mocks/MockTasks.h`, `mocks/MockTransaction.h` |

### 10.3 Benchmark Files (7 total)

| File | Purpose |
|------|---------|
| `EventProcessingBench.cpp` | Event queue throughput measurement |
| `OrderMatchingBench.cpp` | Order matching performance |
| `StateMachineBench.cpp` | State transition performance |
| `InterlockCacheBench.cpp` | Lock-free cache performance |
| `TransactionScopePoolBench.cpp` | Lock-free object pool allocation |
| `NumaAllocatorBench.cpp` | NUMA-aware allocation performance |
| `OrderParamsLayoutBench.cpp` | Field layout optimization |

---

## Document Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | January 2026 | Generated | Initial architecture document |
| 1.1 | January 2026 | Generated | Added ultra low-latency optimizations: TransactionScopePool, CacheAlignedAtomic, CpuAffinity, HugePages |
| 1.2 | February 2026 | Generated | Updated file counts (101 src, 45 test, 7 bench), added LMDBStorage, NumaAllocator, PG write-behind, expanded namespace listing, updated test/benchmark appendix |

---

*This document was generated from codebase analysis of the OrderProcessor system.*
