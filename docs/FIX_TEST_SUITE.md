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
| `OrdStatusConversion` | COP OrderStatus → FIX OrdStatus chars (11 statuses; REPLACED → 4, since 39=5 has been out of use since FIX 4.4) |
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
| `NewOrderSingle_OrderQtyTheEngineCannotHold_IsNotQueued` | OrderQty (38) of -5, 4294967296, 0.5 or 0, none a whole number QuantityT holds → refused before anything is stored, nothing pushed (#145) |
| `NewOrderSingle_LargestOrderQtyTheEngineHolds_IsQueued` | OrderQty 4294967295, the largest QuantityT → queued, its orderQty and leavesQty (#145) |
| `NewOrderSingle_LimitPriceNoOrderTradesAt_IsNotQueued` | A limit order's Price (44) of 0, of -1.25, or of 310 digits either way, which QuickFIX reads as infinity → refused, nothing pushed (#145) |
| `NewOrderSingle_MarketOrderWithPriceZero_IsQueued` | A market order's Price of 0 → queued: it trades at no price of its own (#145) |
| `NewOrderSingle_MarketOrderWithAPriceBeyondDoubleRange_IsNotQueued` | A market order's 310-digit Price → refused, nothing pushed (#145) |
| `NewOrderSingle_FxSwapFarPriceNoOrderTradesAt_IsNotQueued` | A swap's far price (6500) no order trades at → refused, nothing pushed (#145) |
| `NewOrderSingle_UnknownSymbol_NoPush` | Unknown symbol → no event pushed; refused to the sender, 103=1 (#189) |
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
| `ReplaceRequest_OrderQtyTheEngineCannotHold_IsNotQueued` | A replace's OrderQty QuantityT can't hold → refused with an OrderCancelReject, as an unknown symbol is; nothing pushed (#145) |
| `ReplaceRequest_PriceNoOrderTradesAt_IsNotQueued` | A replace's Price no order trades at → refused likewise, nothing pushed (#145) |
| `ReplaceRequest_StopPxMovesAStopOrdersStop` | A stop limit's replace with StopPx (99) → the replacement's new stop; one without StopPx keeps the original's (#238) |
| `ReplaceRequest_StopPxForAnOrderThatIsntAStopOrder_IsNotQueued` | StopPx on a limit order's replace → refused by the gateway (102=99), nothing pushed (#238) |
| `ReplaceRequest_StopPxNoOrderTradesAt_IsNotQueued` | A replace's StopPx no order trades at → refused by the gateway (102=99), nothing pushed (#238) |
| `ReplaceRequest_ExpireTimeFollowsReplaceOrdersRules` | A GTD replacement keeps the original's ExpireTime or takes the request's; a replace to GTC drops it (#240) |
| `ReplaceRequest_AnExpireTimeTheRulesRefuse_IsNotQueued` | To GTD without ExpireTime, with one that has passed, or ExpireTime for a Day order → refused (102=99), nothing pushed (#240) |
| `NewOrderSingle_StoresTheClOrdIdUnderItsSession` | The order's ClOrdID is stored as `FIX:<SenderCompID>-><TargetCompID>\|<ClOrdID>` (#71) |
| `NewOrderMultileg_StoresTheClOrdIdUnderItsSession` | The same for an FX swap (#71) |
| `CancelRequest_ForAnotherSessionsOrder_IsNotQueued` | Cancel naming another session's ClOrdID → answered as unknown by the gateway, nothing pushed (#71) |
| `ReplaceRequest_ForAnotherSessionsOrder_IsNotQueued` | Replace naming another session's ClOrdID → likewise (#71) |
| `CancelRequest_ForItsOwnOrderStoredUnderTheClOrdIdAlone_IsQueued` | The session's own order stored before #71, under its ClOrdID alone → still cancelled |
| `CancelRequest_ForAnotherSourcesOrderStoredUnderTheClOrdIdAlone_IsNotQueued` | A WebSocket order's ClOrdID, or another session's from before #71 → not found (#71) |
| `NewOrderMultileg_FxSwap_PushesToQueue` | 2-leg multileg → FXSWAP OrderEntry with near/far prices, and LegSettlDates as midnight UTC of their days (#198) |
| `ASwapsSettlementDatesAreReadAsDates` | A NewOrderSingle swap's SettlDate (64) and 6501, LocalMktDates, become midnight UTC of their days (#198) |
| `ASwapWithADateThatIsntOneIsNotQueued` | `-5`, a non-number, 21 digits or a day that doesn't exist, in 64, 6501 or a leg's 588 → refused before anything is stored, nothing pushed (#190) |
| `NewOrderSingle_StopOrdersCarryTheirStopPx` | A STOP's and a STOP_LIMIT's StopPx (99) → the order's stop price; a STOP_LIMIT's Price (44) is its limit (#223) |
| `NewOrderSingle_StopOrderWithoutAStopPxNoOrderTradesAt_IsNotQueued` | A stop order without StopPx, or with one no order trades at → refused before anything is stored, nothing pushed (#223) |
| `NewOrderSingle_AGtdOrderCarriesItsExpireTime` | TimeInForce GTD (59=6) with ExpireTime (126) → the order's expiry (#240) |
| `NewOrderSingle_AnExpireTimeTheRulesRefuse_IsNotQueued` | A GTD order without ExpireTime, with ExpireDate (432) alone or with one that has passed, and ExpireTime on a Day order → refused, nothing pushed (#240) |
| `NewOrderMultileg_AGtdSwapCarriesItsExpireTime` | The same for an FX swap (#240) |
| `ExecutionReport_ASwapsLegsCarryTheirSideDateAndPrice` | Each leg's report carries its side, its LegSettlDate as `YYYYMMDD` (#198), its price, and a TradeLinkID (#153): the fill's link, the same on both legs even when their transact times differ (#199) |
| `NewOrderMultileg_NonSwapOrdType_Rejected` | Non-FXSWAP OrdType in multileg → no push; refused to the sender, 103=11 (#189) |
| `NewOrderMultileg_TooFewLegs_Rejected` | Single leg multileg → no push; refused to the sender, 103=11 (#189) |
| `NewOrderMultileg_OrderQtyTheEngineCannotHold_IsNotQueued` | The same quantities on an FX swap → refused, nothing pushed (#145) |
| `NewOrderMultileg_LegPriceNoOrderTradesAt_IsNotQueued` | Either leg's LegPrice (566) no order trades at → refused, nothing pushed (#145) |
| `SessionMapPopulatedOnLogon` | `onLogon()` makes the session known |
| `ALogonNeedsItsCounterpartysUsernameAndPassword` | With credentials, `fromAdmin()` accepts a Logon only with its counterparty's Username and Password, and from a CompID the credentials name; it refuses with `RejectLogon` and a text that doesn't say which was wrong. Other admin messages aren't checked (#147) |
| `WithoutCredentialsEveryLogonIsAccepted` | The gateway without credentials, as tests run it, checks only a Logon's version (FixtSessionTest); the server won't run FIX that way (#147) |
| `ASessionTradesOnlyItsCounterpartysAccounts` | An order, single or FX swap, naming one of the counterparty's accounts is queued; one naming no account trades its first; one naming another account is refused before the engine sees it (#147) |
| `CredentialsGivingAnAccountTheServerDoesntHaveAreRefused` | `setCredentials()` throws for an account the data directory doesn't have (#147) |
| `ASessionStaysKnownWhileItsClientIsAway` | A session is known from `onCreate()`, before any logon, and still after `onLogout()`, so a report for its order reaches QuickFIX, which keeps it for a ResendRequest (#160) |
| `MakeSourceString_Format` | Source string format: `"FIX:SENDER->TARGET"` |
| `FromApp_RoutesAGenericNewOrderSingle` | `fromApp()` given a plain `FIX::Message` builds the typed message, without the cracker's bad cast that UBSan reported |
| `FromApp_RoutesAGenericCancelRequest` | The same for a cancel request |
| `FromApp_UnsupportedTypeIsAnsweredByTheGateway` | A type the gateway doesn't handle is answered with the gateway's own BusinessMessageReject, not thrown for QuickFIX to answer (#377) |
| `FromApp_HandlerExceptionIsContained` | A handler's exception stays in `fromApp()` and is answered with a BusinessMessageReject, rather than ending the server (#36) |
| `NewOrdersAndAReplaceWriteNoStringRecords` (FixGatewayStringsTest) | A NewOrderSingle, an FX swap's NewOrderMultileg and a replace write no string records: their ClOrdIDs, source and destination go in the order's own record (#192) |
| `ExecutionReport_RejectCarriesOrdRejReasonAndText` | Reject → 150=8, 39=8, OrdRejReason 103=0, the reason in Text (58) |
| `ExecutionReport_AcknowledgementCarriesNoRejectFields` | A new order's acknowledgement → 150=0, 39=0, no 103 |
| `ExecutionReport_CancelAckNamesTheCancelRequest` | A cancel's acknowledgement → 11 the request's ClOrdID, 41 the order's; a cancel no request asked for keeps the order's in 11 (#98) |
| `ExecutionReport_TradeCarriesLastQtyAndLastPx` | Trade → 150=F, LastQty (32), LastPx (31) |
| `ExecutionReport_CarriesTheOrdersAvgPx` | The order's AvgPx (6), which FIX 5.0 SP2 doesn't require and FIX 4.4 did (#377) |
| `ExecutionReport_TradeCarriesItsTradeDate` | Trade → TradeDate (75), the business day it counts in, `YYYYMMDD`; a trade in no day has none (#258) |
| `ExecutionReport_CorrectionIsTradeCorrect` | Correction → 150=G, not NEW |
| `ExecutionReport_AStopsReleaseIsTriggeredAndNew` | A stop's release → 150=L, 39=0, its ClOrdID, no 103 or LastQty (#236) |
| `ExecutionReport_AGtdOrdersReportCarriesItsExpireTime` | A GTD order's report → ExpireTime (126) to the millisecond; a Day order's has none (#240) |
| `ExecutionReport_ARestatementIsExecTypeDForAGtRenewal` | A business day's restatement → 150=D, 378=1 (GT renewal / restatement), the order's status; no other report carries 378 (#244) |
| `ExecutionReport_AGtOrderCarriesItsDayFiguresOnceItTradedOnAnEarlierDay` | A GT order with fills before today → DayOrderQty (424), DayCumQty (425), DayAvgPx (426); one whose fills are all today's, or a DAY order, carries none (#244) |
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
| `DuplicateNewOrderGetsAnExecutionReportReject` | A new order whose ClOrdID is in use → 150=8, 39=8, OrdRejReason 6, the reason in Text (58), OrderID NONE, AvgPx (6) 0 |
| `TheSenderSeesItsOwnClOrdIdsInAnOrderReject` | The engine's event names ClOrdIDs under the session's key → the client gets its own, in 11, 41 and Text (58) (#71) |
| `AnOrderRejectsExecIdIsTheIdItIsGiven` | The ExecID comes from the shared id generator, so it doesn't repeat after a restart (#81) |
| `OrderRefusedForAnotherReasonUsesTheBrokerOptionCode` | Refused for another reason → OrdRejReason 0 |
| `DuplicateReplacementGetsAnOrderCancelReject` | A replacement whose ClOrdID is in use → 35=9, 434=2, 102=6, and the order it was to replace |
| `RefusedReplaceSaysWhyAsARefusedCancelDoes` | The engine's reason maps to CxlRejReason as for a cancel; a ClOrdID in use still says 6 (#74) |
| `ReplaceOfAnUnknownOrderIsRejectedAsUnknown` | The gateway's own answer to an unknown OrigClOrdID → 102=1, OrderID NONE, 39=8, and the request's ClOrdIDs (#74) |
| `AnOrderTheGatewayRefusesGetsAnExecutionReportRejectQuotingItsQuantityAsSent` | An order the gateway refuses before the engine sees it → 150=8, 39=8, OrderID NONE, its OrderQty (38) as the client sent it, OrdRejReason 13 (incorrect quantity) and why in Text (58) (#145) |

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

## 10. FixRotatingLogTest — Bounded FIX Logs

**File:** `test/FixRotatingLogTest.cpp`
**Requires:** `BUILD_FIX=ON`

The QuickFIX log the gateway uses since #160, whose files rotate where QuickFIX's own FileLog grew without limit.

| Test | Description |
|------|-------------|
| `ASessionsFilesAreBounded` | 4 MiB of messages, at 1 MiB a file and two backups, leave three files of at most 1 MiB each |
| `LinesAreWrittenAsFileLogWritesThem` | `<UTC timestamp> : <text>`, messages to one file and events to the other, with braces and SOH separators kept |
| `TheLogOutsideASessionIsNamedGlobal` | As QuickFIX's FileLogFactory names it |
| `LimitsThatMakeNoSenseAreRefused` | `FileLogMaxMB` or `FileLogBackups` that can't be used → refused |

---

## 11. FixClientRecoversMissedReports — A Real Server, Two Sessions

**File:** `test/FixResendTest.py`, a ctest
**Requires:** `BUILD_FIX=ON`, Python 3

Runs `orderProcessorServer` with this branch's `config/fix.cfg` and two FIX clients on raw sockets. CLIENT_A rests a buy and drops its connection; CLIENT_B's sell fills it while A is away; A logs on again, with its sequence numbers continuing, and its ResendRequest must get the fill back, with PossDupFlag (43=Y). Before #160 the gateway forgot a session at logout and dropped the report, and `ResetOnLogon=Y` started both sides from 1.

## 12. FixCredentialsTest — The Credentials File and Its Hashes

**File:** `test/FixCredentialsTest.cpp`
**Requires:** `BUILD_FIX=ON`

| Test | Description |
|------|-------------|
| `HashesWithPbkdf2HmacSha256` | RFC 7914's PBKDF2-HMAC-SHA256 vectors, as `hashPassword()` stores them |
| `EachHashHasItsOwnSalt` | Two hashes of one password differ; the iteration count is read back |
| `ALogonNeedsTheUsernameAndThePassword` | `verify()` takes only the counterparty's Username and Password, and no stored password that isn't a hash |
| `AFileNamesEachCounterpartyItsUserAndItsAccounts` | Lines with spaces or tabs, CR LF endings, comments and blank lines; accounts in order |
| `ALineThatCantBeUsedIsRefusedWithItsNumber` | A wrong field count, a CompID or an account given twice, an empty account, a hash in another form: refused with the file and line |
| `AFileItCantReadIsRefused` | A missing file is refused, naming it |

---

## 13. FixLogonNeedsCredentials — A Real Server's Logons

**File:** `test/FixLogonTest.py`, a ctest
**Requires:** `BUILD_FIX=ON`, Python 3

Runs `orderProcessorServer` with this branch's `config/fix.cfg` and a credentials file, and checks, on the wire:
- the server won't start FIX without `--fix-credentials` (exit 2), nor with an account the data directory lacks, nor with a BeginString or DefaultApplVerID other than FIXT.1.1's and FIX 5.0 SP2's (exit 1, #377);
- a Logon with a wrong password, none, another username, or DefaultApplVerID 7 (FIX 5.0, not SP2) gets a Logout and a closed connection, and one from a CompID no line names, or of FIX 4.4, is dropped; the log says why, and has no password in it;
- with its password the counterparty logs on, an order on one of its accounts is acknowledged, and one on another account is refused with 150=8 and 103=15;
- `--hash-fix-password` makes a hash Python's own PBKDF2 agrees with, with 600000 iterations.

`test/fix_client.py` holds the raw-socket FIX client and server harness it shares with `FixResendTest.py`.

---

## 14. FixLocalMktDateTest — Settlement Dates

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

| Test | Description |
|------|-------------|
| `ADateIsMidnightUtcOfThatDay` | `YYYYMMDD` in, milliseconds since the epoch at midnight UTC out, a leap day included; back to `YYYYMMDD`, from any millisecond of the day; a date past 9999-12-31 is written as that day (#198) |
| `AnythingElseIsNotADate` | `-5`, `1000`, 7 or 9 digits, letters, a sign or space, 21 digits, 2023-02-29, month 13 or 0, day 32, and 1969-12-31 → not dates (#190) |

---

## 15. FixRefusalsReachOnlyTheirSender — A Real Server's Refusals

**File:** `test/FixRefusalTest.py`, a ctest
**Requires:** `BUILD_FIX=ON`, Python 3

Two counterparties log on. CLIENT_A sends a NewOrderSingle for an unknown symbol, and NewOrderMultilegs that aren't an FX swap, have one leg, or name an unknown symbol. Each must come back to CLIENT_A alone, as an ExecutionReport with 150=8, 39=8 and OrdRejReason 1 or 11, and CLIENT_B must get nothing. Before #189 the first got no answer, and the others went to every session logged on as BusinessMessageRejects naming no order.

---

## 16. FixTlsTest — The Acceptor's TLS Settings

**File:** `test/FixTlsTest.cpp`
**Requires:** `BUILD_FIX=ON`

| Test | Description |
|------|-------------|
| `TheFilesAndTheProtocolsGoIntoDefault` | `addFixTls()` puts the certificate and key files in `[DEFAULT]`, with TLS 1.2 and 1.3 only, where QuickFIX's default would allow SSLv3, TLS 1.0 and TLS 1.1; a counterparty's session made after carries them (#228) |
| `TheSettingsOwnProtocolsAreKept` | An `SSLProtocol` that `config/fix.cfg` sets is kept (#228) |
| `AFileItCantReadIsRefusedByName` | A certificate or key it can't read, or none, is refused, naming the file (#228) |

---

## 17. FixServesTlsOnly — A Real Server Over TLS

**File:** `test/FixTlsTest.py`, a ctest
**Requires:** `BUILD_FIX=ON`, Python 3, the `openssl` command

Runs `orderProcessorServer` with a certificate made for the run, and checks, on the wire:
- `--fix-tls-cert` without `--fix-tls-key` exits 2, and so do both without `--fix-cfg`; a key that isn't there exits 1, naming it;
- a client that checks the certificate logs on over TLS 1.2 or 1.3, and its order is acknowledged;
- a plain TCP client gets no answer, and the server closes the connection;
- with the settings' own `SSLProtocol` at TLS 1.3 only, a TLS 1.2 client fails the handshake and a TLS 1.3 one logs on;
- without the flags FIX runs over plain TCP, and the server warns so, once (#228).

---

## 18. FixSessionTimeTest — The Session's Day

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

| Test | Description |
|------|-------------|
| `ASessionsDayEndsAt1700NewYork` | `config/fix.cfg`'s StartTime and EndTime, read as QuickFIX's SessionFactory reads them with UseLocalTime, in the zone `useBusinessDayLocalTime()` gives the process: a session rolls at 17:00 New York, 21:00 UTC in daylight time and 22:00 UTC in standard time, and not at New York's midnight or 00:00 UTC (#240) |

---

## 19. FixQuantityTest and FixPriceTest — What the Gateway Takes

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

The checks the gateway makes on a quantity and a price before the engine sees them (#145).

| Test | Description |
|------|-------------|
| `OrderQtyIsAWholeNumberTheEngineCanHold` (FixQuantityTest) | `toQuantity()`: 1, 100 and 4294967295 → those; -5, 4294967296, 0.5, 0, -0, 4294967295.5, 1e300, both infinities and NaN → none |
| `APriceIsFiniteAndAboveZeroUnlessTheOrderIsAMarketOrder` (FixPriceTest) | `isTradablePrice()`: a limit order's or a swap's price is finite and above 0; a market order's may be 0, but not infinite |

---

## 20. FixtSessionTest — FIXT.1.1 Sessions, with FIX 5.0 SP2

**File:** `test/FixGatewayTest.cpp`
**Requires:** `BUILD_FIX=ON`

The gateway speaks FIX 5.0 SP2 over FIXT.1.1 (#377). Each test hands CLIENT_A's messages to a real QuickFIX session that `config/fix.cfg` makes, as the acceptor makes one, with the gateway as its application, and reads what the session sends as a counterparty does: parsed with the session's dictionaries, this tree's `config/spec`, and validated as QuickFIX validates by default. "Taken" means such a counterparty accepts every message sent. Each test loads FIX 5.0 SP2's dictionary, most of a second.

| Test | Description |
|------|-------------|
| `TheSettingsMakeAFixtSessionWithFix50Sp2` | `config/fix.cfg`'s session is FIXT.1.1's, DefaultApplVerID 9, with `FIX50SP2.xml`, whose ExecType has L; no other version has a dictionary |
| `ALogonIsAnsweredWithFix50Sp2AsTheDefault` | A Logon gets a Logon, 8=FIXT.1.1 and 1137=9 |
| `ALogonOfAnotherVersionIsRefused` | A Logon with DefaultApplVerID 7, 6 or `FIX.5.0SP2` (the name, where the value belongs) gets a Logout saying 1137 must be 9, and the connection closed: the session would check that version's messages against no dictionary |
| `AMessageOfAnotherVersionIsRefused` | A message with ApplVerID (1128) 7 gets a Reject (35=3) naming 1128, SessionRejectReason 18, and isn't queued; the session goes on |
| `AMessageOfFix50Sp2OrOfNoVersionIsTaken` | A message with 1128=9, and one without 1128, are queued |
| `AValueFix50Sp2DoesntHaveIsRefused` | OrdType Z gets a Reject naming 40: the session checks against FIX 5.0 SP2's dictionary |
| `AMessageTypeTheGatewayDoesntTakeIsRefusedReadably` | News (35=B) gets the gateway's BusinessMessageReject, 380=3; QuickFIX's own carried 1137, which a counterparty that validates refuses |
| `ARequestWithoutAFieldTheGatewayNeedsIsRefusedReadably` | A cancel without OrigClOrdID (41) and an order without OrderQty (38), which FIX 5.0 SP2 doesn't require, get a BusinessMessageReject, 380=5, naming the field |
| `TheGatewaysOwnRefusalsAreTaken` | The gateway's refusal of an unknown symbol (35=8), and of a cancel and a replace of an unknown order (35=9) |
| `TheEnginesRefusalsAreTaken` | The engine's refusals as the gateway reports them: a new order and a replacement whose ClOrdID is in use, a cancel too late, a business reject |
| `EveryKindOfExecutionReportIsTaken` | Every ExecType the gateway sends, a stop's release (150=L, #236) among them, and a trade, a correction, a cancel's acknowledgement and a replacement's report |
| `ASwapsTradeReportIsRefusedForItsTradeLinkIdAlone` | A swap leg's trade report is refused for TradeLinkID (820), which neither FIX 4.4's ExecutionReport nor FIX 5.0 SP2's has (#378), and for nothing else: its LegLastQty (1418) is in the NoLegs group, where FIX 5.0 SP2 has it |

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
