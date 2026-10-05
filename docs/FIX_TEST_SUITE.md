# FIX Gateway & FX Swap Test Suite

Test coverage for the QuickFIX/C++ FIX gateway integration and FX Swap order support. All tests require `BUILD_FIX=ON` (except FX Swap tests which are always built).

This page lists the tests by suite and says what each one checks. It keeps no counts: `ctest -N` gives them for the build you have, and a number written here goes stale with the next test.

```bash
# Run all FIX/FX tests
ctest -R 'Fix|FxSwap|MultiOut'

# List them, with the count
ctest -N -R 'Fix|FxSwap|MultiOut'

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
| `EveryComponentTouched` | Full sell→buy→match→fill cycle, checking every component in the matrix below |

**Component verification matrix:**

| Component | What is asserted | Proves |
|-----------|-----------------|--------|
| **FixGateway** | `source_ == "FIX:PIPELINE_CLIENT->ORDER_PROCESSOR"` | FIX message translated to OrderEntry |
| **IncomingQueues** | Orders appear in OrderStorage after push | Events queued and dispatched |
| **TaskManager** | Orders reach FILLED status | Work dispatched to Processor pool |
| **Processor** | State transitions complete | Event handling + deferred event loop |
| **StateMachine** | `status_ == FILLED_ORDSTATUS` | Rcvd_New → New → Filled transitions |
| **OrderStorage** | `locateByOrderId()` and `locateByClOrderId()` (the session's key, #71) both work | Dual-index persistence |
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
| `CancelOrder_ViaFix_Completes` | FIX OrderCancelRequest → order CANCELED with leavesQty 0, one CANCEL report to the session, no reject (#73) |
| `CancelOfAFilledOrder_ViaFix_IsRefusedToTheSender` | Cancel of a filled order → refused (too late) to the sending session, with the order's status (#73) |
| `ReplaceOrder_ViaFix_Completes` | FIX OrderCancelReplaceRequest → original REPLACED, replacement NEW under the request's ClOrdID, one REPLACE report each |
| `ReplaceOfAFilledOrder_ViaFix_IsRefusedToTheSender` | Replace of a filled order → refused (too late) to the sending session, with the request's ClOrdIDs |
| `SourceStringPreserved` | Order source tracks back to `"FIX:TRADER_A->ORDER_PROCESSOR"` |
| `MarketOrder_FillsImmediately` | Market order enters pipeline with correct type/side/qty |
| `TwoSessions_CanUseTheSameClOrdId` | Two sessions each place ClOrdID `SAME-1` → both NEW, and each session's cancel finds its own order (#71) |
| `AnotherSessionsCancelAndReplace_ViaFix_DoNotReachTheEngine` | A second session's cancel and replace naming the first session's ClOrdID → the order is untouched, and the engine sees neither (#71) |

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
| `OrdStatusConversion` | COP OrderStatus → FIX OrdStatus chars (11 statuses; REPLACED → 4, since FIX 4.4 has no 39=5) |
| `ExecTypeConversion` | COP ExecType → FIX ExecType chars (12 types) |

---

## 4. FixGatewayInboundTest — FIX Message Translation and Reports

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

FIX message translation, `fromApp()` dispatch, and the ExecutionReports the gateway builds. `MockInQueues` captures what reaches the engine.

| Test | Description |
|------|-------------|
| `NewOrderSingle_PushesToQueue` | Limit order: correct side, ordType, price, qty, tif, status |
| `NewOrderSingle_MarketOrder` | Market order: ordType=MARKET, side=SELL, qty=50 |
| `NewOrderSingle_UnknownSymbol_NoPush` | Unknown symbol → no event pushed, error logged |
| `NewOrderSingle_WithoutAccount_GetsTheServerDefaults` | No Account (1) → the server's default account and clearing firm (#34) |
| `NewOrderSingle_NamingAnAccount_KeepsIt` | An Account (1) the server knows → kept |
| `NewOrderSingle_RefusedWhenTheServerHasNoClearingFirm` | The server has no clearing firm → refused at the gateway, nothing pushed (#34) |
| `NewOrderSingle_WithoutAccount_RefusedWhenTheServerHasNoDefaultAccount` | No Account (1), and the server has no default account → refused at the gateway, nothing pushed (#34) |
| `NewOrderMultileg_FxSwap_WithoutAccount_GetsTheServerDefaults` | The same defaults for an FX swap |
| `NewOrderMultileg_FxSwap_RefusedWhenTheServerHasNoClearingFirm` | The same refusal for an FX swap |
| `CancelRequest_PushesToQueue` | OrderCancelRequest for the session's own order → OrderCancelEvent with its orderId and the request's ClOrdID (#98) |
| `ReplaceRequest_PushesToQueue` | OrderCancelReplaceRequest → OrderReplaceEvent: a fresh id, ClOrdID from tag 11 under the session's key, OrigClOrdID set, new price/qty |
| `ReplaceRequest_CarriesTheRequestsSideAndSymbol` | Side and Symbol come from the request, so the engine refuses a change |
| `ReplaceRequest_UnknownSymbol_IsNotQueued` | Unknown Symbol → refused by the gateway (102=99), nothing pushed |
| `ReplaceRequest_UnknownOrigClOrdId_IsNotQueued` | Unknown OrigClOrdID → answered by the gateway (102=1), nothing pushed |
| `NewOrderSingle_StoresTheClOrdIdUnderItsSession` | The order's ClOrdID is stored as `FIX:<SenderCompID>-><TargetCompID>\|<ClOrdID>` (#71) |
| `NewOrderMultileg_StoresTheClOrdIdUnderItsSession` | The same for an FX swap (#71) |
| `CancelRequest_ForAnotherSessionsOrder_IsNotQueued` | Cancel naming another session's ClOrdID → answered as unknown by the gateway, nothing pushed (#71) |
| `ReplaceRequest_ForAnotherSessionsOrder_IsNotQueued` | Replace naming another session's ClOrdID → likewise (#71) |
| `CancelRequest_ForItsOwnOrderStoredUnderTheClOrdIdAlone_IsQueued` | The session's own order stored before #71, under its ClOrdID alone → still cancelled |
| `CancelRequest_ForAnotherSourcesOrderStoredUnderTheClOrdIdAlone_IsNotQueued` | A WebSocket order's ClOrdID, or another session's from before #71 → not found (#71) |
| `NewOrderMultileg_FxSwap_PushesToQueue` | 2-leg multileg → FXSWAP OrderEntry with near/far prices+dates |
| `NewOrderMultileg_NonSwapOrdType_Rejected` | Non-FXSWAP OrdType in multileg → rejected, no push |
| `NewOrderMultileg_TooFewLegs_Rejected` | Single leg multileg → rejected, no push |
| `SessionMapPopulatedOnLogon` | `onLogon()` adds session, `onLogout()` removes it |
| `MakeSourceString_Format` | Source string format: `"FIX:SENDER->TARGET"` |
| `FromApp_RoutesAGenericNewOrderSingle` | `fromApp()` given a plain `FIX::Message` builds the typed message, without the cracker's bad cast that UBSan reported |
| `FromApp_RoutesAGenericCancelRequest` | The same for a cancel request |
| `FromApp_UnsupportedTypeStillReachesQuickFix` | A type the gateway doesn't handle still throws UnsupportedMessageType, which QuickFIX answers |
| `FromApp_HandlerExceptionIsContained` | A handler's exception stays in `fromApp()` and is answered with a BusinessMessageReject, rather than ending the server (#36) |
| `ExecutionReport_RejectCarriesOrdRejReasonAndText` | Reject → 150=8, 39=8, OrdRejReason 103=0, the reason in Text (58) |
| `ExecutionReport_AcknowledgementCarriesNoRejectFields` | A new order's acknowledgement → 150=0, 39=0, no 103 |
| `ExecutionReport_CancelAckNamesTheCancelRequest` | A cancel's acknowledgement → 11 the request's ClOrdID, 41 the order's; a cancel no request asked for keeps the order's in 11 (#98) |
| `ExecutionReport_TradeCarriesLastQtyAndLastPx` | Trade → 150=F, LastQty (32), LastPx (31) |
| `ExecutionReport_CorrectionIsTradeCorrect` | Correction → 150=G, not NEW |
| `ExecutionReport_ReplaceNamesTheReplacedOrder` | Replacement's REPLACE report → 150=5, its status, OrigClOrdID (41) |
| `ExecutionReport_CarriesTheClientsOwnClOrdIds` | ClOrdIDs stored under the session's key go out as the client sent them, in 11 and 41 (#71) |
| `ExecutionReport_OnlyTheReplacementsReplaceReportGoesOverFix` | The replaced order's own REPLACE report is not sent over FIX |

---

## 5. FixOrderRejectTest — Orders Refused Without Being Stored

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

Pure unit tests for the messages that answer a new order or a replacement the engine refused without storing it (#67, #74).

| Test | Description |
|------|-------------|
| `DuplicateNewOrderGetsAnExecutionReportReject` | A new order whose ClOrdID is in use → 150=8, 39=8, OrdRejReason 6, the reason in Text (58), OrderID NONE |
| `TheSenderSeesItsOwnClOrdIdsInAnOrderReject` | The engine's event names ClOrdIDs under the session's key → the client gets its own, in 11, 41 and Text (58) (#71) |
| `AnOrderRejectsExecIdIsTheIdItIsGiven` | The ExecID comes from the shared id generator, so it doesn't repeat after a restart (#81) |
| `OrderRefusedForAnotherReasonUsesTheBrokerOptionCode` | Refused for another reason → OrdRejReason 0 |
| `DuplicateReplacementGetsAnOrderCancelReject` | A replacement whose ClOrdID is in use → 35=9, 434=2, 102=6, and the order it was to replace |
| `RefusedReplaceSaysWhyAsARefusedCancelDoes` | The engine's reason maps to CxlRejReason as for a cancel; a ClOrdID in use still says 6 (#74) |
| `ReplaceOfAnUnknownOrderIsRejectedAsUnknown` | The gateway's own answer to an unknown OrigClOrdID → 102=1, OrderID NONE, 39=8, and the request's ClOrdIDs (#74) |

---

## 6. FixCancelRejectTest — Refused Cancels

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

Pure unit tests for the OrderCancelReject (35=9, 434=1) that refuses a cancel request (#73).

| Test | Description |
|------|-------------|
| `CancelOfAFilledOrderIsTooLate` | Too late → 102=0, the order's status (39=2), why in Text (58) |
| `AnEngineRejectNamesTheCancelRequestAndTheOrder` | 11 the request's ClOrdID, 41 the order's; an event without the request's falls back to the order's in 11 (#98) |
| `CancelOfAnUnknownOrderIsRejectedAsUnknown` | The gateway's own answer to an unknown OrigClOrdID → 102=1, 39=8 |
| `PendingAndOtherReasonsMapToTheirCodes` | A replace pending → 102=3; any other reason → 102=99 |

---

## 7. FixClOrdIdKeyTest — ClOrdIDs per Session

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

A pure unit test for the key a FIX order's ClOrdID is stored under (#71).

| Test | Description |
|------|-------------|
| `TheClientsClOrdIdComesBackFromItsSessionsKey` | `sessionClOrdId()` and `clientClOrdId()` round-trip, also for a ClOrdID containing `\|`; a key without the session's prefix comes back whole |

---

## 8. MultiOutQueuesTest — Outbound Fan-Out

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

Tests the MultiOutQueues adapter that delegates to multiple OutQueues implementations.

| Test | Description |
|------|-------------|
| `FansOutToAllDelegates` | ExecReportEvent reaches both delegates |
| `FansOutCancelReject` | CancelRejectEvent reaches both delegates |
| `FansOutOrderReject` | OrderRejectEvent reaches both delegates (#67) |
| `FansOutBusinessReject` | BusinessRejectEvent reaches both delegates |
| `EmptyDelegatesNoCrash` | push() with no delegates doesn't crash |

---

## 9. FxSwapTest — FX Swap Order Matching

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
│ FixFullPipelineTest                                          │
│   All real components, no mocks                              │
│   Verifies artifact from every component                     │
├─────────────────────────────────────────────────────────────┤
│ FixEndToEndTest                                              │
│   Real pipeline (InQueues → Processor → OutQueues)           │
│   CapturingOutQueues instead of WsOutQueues                  │
├─────────────────────────────────────────────────────────────┤
│ FixGatewayInboundTest                  FxSwapTest            │
│   MockInQueues captures pushed events  Real OrderBook+Matcher│
│   FIX→internal, reports the gateway    Tests swap matching   │
│   builds                                                     │
├─────────────────────────────────────────────────────────────┤
│ FixEnumTest  FixOrderRejectTest  FixCancelRejectTest         │
│ FixClOrdIdKeyTest  MultiOutQueuesTest                        │
│   Pure unit, no I/O; MockOutQueues as MultiOut's delegates   │
└─────────────────────────────────────────────────────────────┘
```

---

## Running Tests

```bash
# Build with FIX support
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_FIX=ON
cmake --build build -j$(nproc)

# All tests; ctest -N lists and counts them
cd build && ctest --output-on-failure

# Only FIX/FX Swap tests
ctest -R 'Fix|FxSwap|MultiOut'

# Only the full pipeline coverage test
ctest -R FixFullPipelineTest

# Only unit tests (fast, no async)
ctest -R 'FixEnum|FixOrderReject|FixCancelReject|FixClOrdIdKey|MultiOut'

# Only end-to-end tests (slower, uses TaskManager)
ctest -R 'FixEndToEnd|FixFullPipeline'

# Build WITHOUT FIX (verifies no regression)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_FIX=OFF
cmake --build build -j$(nproc) && cd build && ctest --output-on-failure
```
