# FIX Gateway & FX Swap Test Suite

Test coverage for the QuickFIX/C++ FIX gateway integration and FX Swap order support. All tests require `BUILD_FIX=ON` (except FX Swap tests which are always built).

**Total: 40 tests across 7 test suites**

```bash
# Run all FIX/FX tests
ctest -R 'Fix|FxSwap|MultiOut'

# Run just the full pipeline test
ctest -R FixFullPipelineTest

# Run with verbose output
ctest --output-on-failure -R 'Fix|FxSwap|MultiOut'
```

---

## 1. FixFullPipelineTest — Complete Component Coverage

**File:** `test/FixFullPipelineTest.cpp`
**Requires:** `BUILD_FIX=ON`

A single test that submits two FIX orders (sell + buy limit) through the entire system and asserts on artifacts from **every component**. No mocks — all real components.

| Test | Description |
|------|-------------|
| `EveryComponentTouched` | Full sell→buy→match→fill cycle verifying all 15 components |

**Component verification matrix:**

| Component | What is asserted | Proves |
|-----------|-----------------|--------|
| **FixGateway** | `source_ == "FIX:PIPELINE_CLIENT->ORDER_PROCESSOR"` | FIX message translated to OrderEntry |
| **IncomingQueues** | Orders appear in OrderStorage after push | Events queued and dispatched |
| **TaskManager** | Orders reach FILLED status | Work dispatched to Processor pool |
| **Processor** | State transitions complete | Event handling + deferred event loop |
| **StateMachine** | `status_ == FILLED_ORDSTATUS` | Rcvd_New → New → Filled transitions |
| **OrderStorage** | `locateByOrderId()` and `locateByClOrderId()` both work | Dual-index persistence |
| **WideDataStorage** | `instrument_.get().symbol_ == "GBPUSD"` | Reference data resolution |
| **OrderBookImpl** | `getTop() == invalid` (empty after fill) | Orders added then removed |
| **OrderMatcher** | `cumQty_ == 500, leavesQty_ == 0` | Price match + trade execution |
| **TransactionScopePool** | `cacheMisses() == 0` | Arena allocation, no heap fallback |
| **TransactionMgr** | Orders reach terminal state | Transaction dependency tracking + execution |
| **DeferedEvents** | Both orders filled from single match | ExecutionDeferedEvent chain completed |
| **TrOperations** | `executions_` contains TRADE_EXECTYPE entry | CreateTradeExecReportTrOperation ran |
| **MultiOutQueues** | `recordingOut.count == countingOut.count` | Fan-out to both delegates |
| **OutQueues** | `tradeReports >= 2` | Execution reports delivered to both sides |
| **IdTGenerator** | `orderId_.isValid()` and unique per order | Unique ID assignment |
| **OrderCodec** | `decode(encode(order))` preserves all fields | Serialization round-trip |

---

## 2. FixEndToEndTest — FIX Order Lifecycle

**File:** `test/FixEndToEndTest.cpp`
**Requires:** `BUILD_FIX=ON`

Integration tests using real IncomingQueues, Processor, TaskManager, and OrderBook. Orders enter via `FixGateway::onMessage()` and flow through the complete pipeline.

| Test | Description |
|------|-------------|
| `LimitOrder_Accepted` | FIX limit order reaches NEW status with correct fields |
| `TwoOrders_MatchAndFill` | Sell + buy at same price → both FILLED, cumQty=100 |
| `PartialFill` | Sell 50 + buy 100 → sell FILLED, buy PARTFILL with leavesQty=50 |
| `CancelOrder_ViaFix_Queued` | FIX OrderCancelRequest translates and queues correctly |
| `SourceStringPreserved` | Order source tracks back to `"FIX:TRADER_A->ORDER_PROCESSOR"` |
| `MarketOrder_FillsImmediately` | Market order enters pipeline with correct type/side/qty |

---

## 3. FixEnumTest — FIX↔COP Enum Conversion

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

Pure unit tests for static enum conversion functions. No I/O, no singletons.

| Test | Description |
|------|-------------|
| `SideConversion` | FIX Side chars (1,2,5) → COP Side enums |
| `SideRoundTrip` | COP Side → FIX Side char and back |
| `OrdTypeConversion` | FIX OrdType (1,2,3,4) → COP OrderType |
| `TifConversion` | FIX TimeInForce (0,1,2,3,4,7) → COP TimeInForce |
| `CurrencyConversion` | FIX currency strings → COP Currency (8 currencies + invalid) |
| `OrdStatusConversion` | COP OrderStatus → FIX OrdStatus chars (11 statuses) |
| `ExecTypeConversion` | COP ExecType → FIX ExecType chars (12 types) |

---

## 4. FixGatewayInboundTest — FIX Message Translation

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

Tests FIX message parsing and OrderEntry construction using `MockInQueues` to capture pushed events.

| Test | Description |
|------|-------------|
| `NewOrderSingle_PushesToQueue` | Limit order: correct side, ordType, price, qty, tif, status |
| `NewOrderSingle_MarketOrder` | Market order: ordType=MARKET, side=SELL, qty=50 |
| `NewOrderSingle_UnknownSymbol_NoPush` | Unknown symbol → no event pushed, error logged |
| `CancelRequest_PushesToQueue` | OrderCancelRequest → OrderCancelEvent with correct orderId |
| `ReplaceRequest_PushesToQueue` | OrderCancelReplaceRequest → OrderReplaceEvent with new price/qty |
| `NewOrderMultileg_FxSwap_PushesToQueue` | 2-leg multileg → FXSWAP OrderEntry with near/far prices+dates |
| `NewOrderMultileg_NonSwapOrdType_Rejected` | Non-FXSWAP OrdType in multileg → rejected, no push |
| `NewOrderMultileg_TooFewLegs_Rejected` | Single leg multileg → rejected, no push |
| `SessionMapPopulatedOnLogon` | `onLogon()` adds session, `onLogout()` removes it |
| `MakeSourceString_Format` | Source string format: `"FIX:SENDER->TARGET"` |

---

## 5. MultiOutQueuesTest — Outbound Fan-Out

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

Tests the MultiOutQueues adapter that delegates to multiple OutQueues implementations.

| Test | Description |
|------|-------------|
| `FansOutToAllDelegates` | ExecReportEvent reaches both delegates |
| `FansOutCancelReject` | CancelRejectEvent reaches both delegates |
| `FansOutBusinessReject` | BusinessRejectEvent reaches both delegates |
| `EmptyDelegatesNoCrash` | push() with no delegates doesn't crash |

---

## 6. FxSwapTest — FX Swap Order Matching

**File:** `test/FxSwapTest.cpp`
**Requires:** Always built (no BUILD_FIX dependency)

Tests for FX Swap data model, matching, validation, and codec support.

| Test | Description |
|------|-------------|
| `SwapValidation_FarSettlDateMustBeAfterNear` | farSettlDate <= settlDate → rejected |
| `SwapValidation_FarPriceMustBePositive` | farPrice == 0 → rejected |
| `SwapValidation_ValidSwapOrder` | Valid swap fields → isValid() passes |
| `SwapNoMatch_RestsOnBook` | Swap order with no contra → rests (no auto-cancel) |
| `SwapDoesNotMatchLimitOrder` | Swap order does not match regular LIMIT order |
| `SwapMatchExact` | Two contra swaps match → 1 SwapExecutionDeferedEvent |
| `SwapMatchPartial_ChainsMatchEvent` | Partial qty → SwapExec + MatchSwapOrderDeferedEvent |
| `SwapSettlDateMismatch_NoMatch` | Different farSettlDate → no match |
| `SwapFarPriceIncompatible_NoMatch` | Incompatible far price → no match |
| `SwapCodecRoundTrip` | Encode/decode preserves farPrice + farSettlDate (v1) |
| `LegacyCodecCompat_V0DefaultsSwapFields` | v0 orders decode with swap defaults (0.0, 0) |
| `NonSwapOrderUnchanged_LimitMatch` | Regular LIMIT matching still works (regression) |

---

## Test Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ FixFullPipelineTest (1 test)                                 │
│   All real components, no mocks                              │
│   Verifies artifact from every component                     │
├─────────────────────────────────────────────────────────────┤
│ FixEndToEndTest (6 tests)                                    │
│   Real pipeline (InQueues → Processor → OutQueues)           │
│   CapturingOutQueues instead of WsOutQueues                  │
├─────────────────────────────────────────────────────────────┤
│ FixGatewayInboundTest (7 tests)        FxSwapTest (12 tests) │
│   MockInQueues captures pushed events  Real OrderBook+Matcher│
│   Tests FIX→internal translation       Tests swap matching   │
├─────────────────────────────────────────────────────────────┤
│ FixEnumTest (7 tests)    MultiOutQueuesTest (4 tests)        │
│   Pure unit, no I/O      MockOutQueues as delegates          │
└─────────────────────────────────────────────────────────────┘
```

---

## Running Tests

```bash
# Build with FIX support
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_FIX=ON
cmake --build build -j$(nproc)

# All tests (504 total)
cd build && ctest --output-on-failure

# Only FIX/FX Swap tests (37 total)
ctest -R 'Fix|FxSwap|MultiOut'

# Only the full pipeline coverage test
ctest -R FixFullPipelineTest

# Only unit tests (fast, no async)
ctest -R 'FixEnum|MultiOut'

# Only end-to-end tests (slower, uses TaskManager)
ctest -R 'FixEndToEnd|FixFullPipeline'

# Build WITHOUT FIX (verifies no regression)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_FIX=OFF
cmake --build build -j$(nproc) && cd build && ctest --output-on-failure
```
