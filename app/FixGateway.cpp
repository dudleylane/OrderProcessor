#ifdef BUILD_FIX

#include "FixGateway.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "IdTGenerator.h"
#include "Logger.h"

#include <vector>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>

using namespace COP;
using namespace COP::App;
using namespace COP::Store;

// =============================================================================
// Construction
// =============================================================================

FixGateway::FixGateway(Queues::InQueues* inQueues,
                       WideParamsDataStorage* wideData,
                       OrderDataStorage* orderStorage,
                       SourceIdT defaultClearingId)
    : inQueues_(inQueues)
    , wideData_(wideData)
    , orderStorage_(orderStorage)
    , defaultClearingId_(defaultClearingId)
{
}

std::string FixGateway::makeSourceString(const FIX::SessionID& sid) {
    return "FIX:" + sid.getSenderCompID().getString() +
           "->" + sid.getTargetCompID().getString();
}

// =============================================================================
// Application callbacks
// =============================================================================

void FixGateway::onCreate(const FIX::SessionID& sid) {
    aux::ExchLogger::instance()->note("FIX session created: " + sid.toString());
}

void FixGateway::onLogon(const FIX::SessionID& sid) {
    aux::ExchLogger::instance()->note("FIX logon: " + sid.toString());
    std::string source = makeSourceString(sid);
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, true);
    sessionMap_[source] = sid;
}

void FixGateway::onLogout(const FIX::SessionID& sid) {
    aux::ExchLogger::instance()->note("FIX logout: " + sid.toString());
    std::string source = makeSourceString(sid);
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, true);
    sessionMap_.erase(source);
}

void FixGateway::toAdmin(FIX::Message& /*msg*/, const FIX::SessionID& /*sid*/) {}
void FixGateway::toApp(FIX::Message& /*msg*/, const FIX::SessionID& /*sid*/) {}
void FixGateway::fromAdmin(const FIX::Message& /*msg*/, const FIX::SessionID& /*sid*/) {}

void FixGateway::fromApp(const FIX::Message& msg, const FIX::SessionID& sid) {
    crack(msg, sid);
}

// =============================================================================
// Inbound message handlers
// =============================================================================

void FixGateway::onMessage(const FIX44::NewOrderSingle& msg, const FIX::SessionID& sid) {
    FIX::ClOrdID clOrdId;       msg.get(clOrdId);
    FIX::Symbol symbol;         msg.get(symbol);
    FIX::Side side;             msg.get(side);
    FIX::OrdType ordType;       msg.get(ordType);
    FIX::OrderQty orderQty;     msg.get(orderQty);
    FIX::TransactTime transTime; msg.get(transTime);

    FIX::Price price;
    double priceVal = 0.0;
    if (msg.isSet(price)) {
        msg.get(price);
        priceVal = price.getValue();
    }

    FIX::TimeInForce tif;
    char tifVal = '0'; // default Day
    if (msg.isSet(tif)) {
        msg.get(tif);
        tifVal = tif.getValue();
    }

    FIX::Account account;
    std::string acctStr;
    if (msg.isSet(account)) {
        msg.get(account);
        acctStr = account.getString();
    }

    FIX::Currency currency;
    std::string ccyStr = "USD";
    if (msg.isSet(currency)) {
        msg.get(currency);
        ccyStr = currency.getString();
    }

    // Lookup instrument
    SourceIdT instrId = wideData_->findInstrumentBySymbol(symbol.getString());
    if (!instrId.isValid()) {
        aux::ExchLogger::instance()->error("FIX: Unknown instrument: " + symbol.getString());
        return;
    }

    // Lookup account (optional)
    SourceIdT acctId;
    if (!acctStr.empty()) {
        acctId = wideData_->findAccountByName(acctStr);
    }

    // Create clOrderId as RawDataEntry
    std::string clOrdStr = clOrdId.getString();
    auto* clOrdRaw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(),
                                      static_cast<u32>(clOrdStr.size()));
    SourceIdT clOrdSrcId = WideDataStorage::instance()->add(clOrdRaw);

    SourceIdT emptyId;

    SourceIdT clearingId = defaultClearingId_;

    auto* execList = new ExecutionsT();
    SourceIdT execListId = WideDataStorage::instance()->add(execList);

    std::string sourceStr = makeSourceString(sid);
    auto* srcStrPtr = new StringT(sourceStr);
    SourceIdT srcId = WideDataStorage::instance()->add(srcStrPtr);

    auto* destStr = new StringT("Internal");
    SourceIdT destId = WideDataStorage::instance()->add(destStr);

    auto* order = new OrderEntry(srcId, destId, clOrdSrcId, emptyId,
                                 instrId, acctId, clearingId, execListId);
    order->side_ = toSide(side.getValue());
    order->ordType_ = toOrdType(ordType.getValue());
    order->price_ = priceVal;
    order->orderQty_ = static_cast<QuantityT>(orderQty.getValue());
    order->leavesQty_ = order->orderQty_;
    order->tif_ = toTif(tifVal);
    order->currency_ = toCurrency(ccyStr);
    order->capacity_ = AGENCY_CAPACITY;
    order->settlType_ = _2_SETTLTYPE;
    order->status_ = RECEIVEDNEW_ORDSTATUS;
    order->creationTime_ = static_cast<DateTimeT>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    order->lastUpdateTime_ = order->creationTime_;

    // FX Swap: extract far-leg fields from user-defined tags (6500=farPrice, 6501=farSettlDate)
    if (FXSWAP_ORDERTYPE == order->ordType_) {
        if (msg.isSetField(6500))
            order->farPrice_ = std::stod(msg.getField(6500));
        if (msg.isSetField(6501))
            order->farSettlDate_ = static_cast<DateTimeT>(std::stoull(msg.getField(6501)));
        if (msg.isSetField(64))  // SettlDate (standard tag) → near leg
            order->settlDate_ = static_cast<DateTimeT>(std::stoull(msg.getField(64)));
    }

    Queues::OrderEvent evt(order);
    inQueues_->push(sourceStr, evt);
}

// =============================================================================
// NewOrderMultileg (35=AB) — standards-compliant FX Swap entry
//
// Expected structure for FX Swap:
//   - Root fields: ClOrdID, Side (near-leg side), OrdType=FXSwap('G'), OrderQty
//   - NoLegs=2 repeating group:
//       Leg 1 (near): LegSymbol, LegPrice (spot), LegSettlDate (T+2),
//                     LegSide matches root Side
//       Leg 2 (far):  LegSymbol, LegPrice (forward), LegSettlDate (T+N),
//                     LegSide opposite to root Side
// =============================================================================
void FixGateway::onMessage(const FIX44::NewOrderMultileg& msg, const FIX::SessionID& sid) {
    FIX::ClOrdID clOrdId;       msg.get(clOrdId);
    FIX::Side side;             msg.get(side);
    FIX::OrdType ordType;       msg.get(ordType);
    FIX::OrderQty orderQty;     msg.get(orderQty);

    // Only handle FX Swap multileg orders
    if (FXSWAP_ORDERTYPE != toOrdType(ordType.getValue())) {
        aux::ExchLogger::instance()->error(
            "FIX Multileg: only FX Swap (OrdType=G) supported, got: " +
            std::string(1, ordType.getValue()));
        sendBusinessReject(IdT(), "NewOrderMultileg: only FX Swap supported");
        return;
    }

    FIX::Symbol rootSymbol;
    std::string symStr;
    if (msg.isSet(rootSymbol)) {
        msg.get(rootSymbol);
        symStr = rootSymbol.getString();
    }

    // Extract legs — use groupCount() rather than isSet(NoLegs) because
    // addGroup() sets up the count implicitly and isSet() can throw on
    // partial/invalid count fields.
    size_t legCount = msg.groupCount(FIX::FIELD::NoLegs);
    if (legCount < 2) {
        aux::ExchLogger::instance()->error("FIX Multileg: FX Swap requires 2 legs, got " +
            std::to_string(legCount));
        sendBusinessReject(IdT(), "NewOrderMultileg: NoLegs < 2");
        return;
    }

    double nearPrice = 0.0, farPrice = 0.0;
    DateTimeT nearSettlDate = 0, farSettlDate = 0;
    std::string legSymbol;

    FIX44::NewOrderMultileg::NoLegs legGroup;
    for (int i = 1; i <= 2; ++i) {
        msg.getGroup(i, legGroup);

        FIX::LegSymbol lsym;
        if (legGroup.isSet(lsym)) {
            legGroup.get(lsym);
            if (legSymbol.empty()) legSymbol = lsym.getString();
        }

        FIX::LegPrice lpx;
        double lpxVal = 0.0;
        if (legGroup.isSet(lpx)) {
            legGroup.get(lpx);
            lpxVal = lpx.getValue();
        }

        FIX::LegSettlDate lsd;
        DateTimeT lsdVal = 0;
        if (legGroup.isSet(lsd)) {
            legGroup.get(lsd);
            lsdVal = static_cast<DateTimeT>(std::stoull(lsd.getString()));
        }

        FIX::LegSide lside;
        char legSideVal = '0';
        if (legGroup.isSet(lside)) {
            legGroup.get(lside);
            legSideVal = lside.getValue();
        }

        // Near leg matches root side; far leg is opposite
        if (legSideVal == side.getValue()) {
            nearPrice = lpxVal;
            nearSettlDate = lsdVal;
        } else {
            farPrice = lpxVal;
            farSettlDate = lsdVal;
        }
    }

    // Prefer root Symbol, fall back to LegSymbol
    if (symStr.empty()) symStr = legSymbol;

    SourceIdT instrId = wideData_->findInstrumentBySymbol(symStr);
    if (!instrId.isValid()) {
        aux::ExchLogger::instance()->error("FIX Multileg: Unknown instrument: " + symStr);
        sendBusinessReject(IdT(), "Unknown instrument: " + symStr);
        return;
    }

    FIX::Account account;
    std::string acctStr;
    if (msg.isSet(account)) {
        msg.get(account);
        acctStr = account.getString();
    }
    SourceIdT acctId;
    if (!acctStr.empty()) acctId = wideData_->findAccountByName(acctStr);

    FIX::Currency currency;
    std::string ccyStr = "USD";
    if (msg.isSet(currency)) {
        msg.get(currency);
        ccyStr = currency.getString();
    }

    FIX::TimeInForce tif;
    char tifVal = FIX::TimeInForce_GOOD_TILL_CANCEL;
    if (msg.isSet(tif)) { msg.get(tif); tifVal = tif.getValue(); }

    // Build OrderEntry — same pattern as NewOrderSingle
    std::string clOrdStr = clOrdId.getString();
    auto* clOrdRaw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(),
                                      static_cast<u32>(clOrdStr.size()));
    SourceIdT clOrdSrcId = WideDataStorage::instance()->add(clOrdRaw);

    SourceIdT emptyId;
    SourceIdT clearingId = defaultClearingId_;

    auto* execList = new ExecutionsT();
    SourceIdT execListId = WideDataStorage::instance()->add(execList);

    std::string sourceStr = makeSourceString(sid);
    auto* srcStrPtr = new StringT(sourceStr);
    SourceIdT srcId = WideDataStorage::instance()->add(srcStrPtr);

    auto* destStr = new StringT("Internal");
    SourceIdT destId = WideDataStorage::instance()->add(destStr);

    auto* order = new OrderEntry(srcId, destId, clOrdSrcId, emptyId,
                                 instrId, acctId, clearingId, execListId);
    order->side_ = toSide(side.getValue());
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->price_ = nearPrice;
    order->farPrice_ = farPrice;
    order->settlDate_ = nearSettlDate;
    order->farSettlDate_ = farSettlDate;
    order->orderQty_ = static_cast<QuantityT>(orderQty.getValue());
    order->leavesQty_ = order->orderQty_;
    order->tif_ = toTif(tifVal);
    order->currency_ = toCurrency(ccyStr);
    order->capacity_ = AGENCY_CAPACITY;
    order->settlType_ = _2_SETTLTYPE;
    order->status_ = RECEIVEDNEW_ORDSTATUS;
    order->creationTime_ = static_cast<DateTimeT>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    order->lastUpdateTime_ = order->creationTime_;

    Queues::OrderEvent evt(order);
    inQueues_->push(sourceStr, evt);
}

void FixGateway::onMessage(const FIX44::OrderCancelRequest& msg, const FIX::SessionID& sid) {
    FIX::OrigClOrdID origClOrdId; msg.get(origClOrdId);

    // Look up by ClOrdID
    std::string origClOrdStr = origClOrdId.getString();
    RawDataEntry rawKey(STRING_RAWDATATYPE, origClOrdStr.c_str(),
                        static_cast<u32>(origClOrdStr.size()));
    OrderEntry* order = orderStorage_->locateByClOrderId(rawKey);
    if (!order) {
        aux::ExchLogger::instance()->error("FIX: Cancel - order not found: " + origClOrdStr);
        return;
    }

    std::string sourceStr = makeSourceString(sid);
    Queues::OrderCancelEvent evt(order->orderId_, "FIX cancel request");
    inQueues_->push(sourceStr, evt);
}

void FixGateway::onMessage(const FIX44::OrderCancelReplaceRequest& msg, const FIX::SessionID& sid) {
    FIX::OrigClOrdID origClOrdId; msg.get(origClOrdId);

    std::string origClOrdStr = origClOrdId.getString();
    RawDataEntry rawKey(STRING_RAWDATATYPE, origClOrdStr.c_str(),
                        static_cast<u32>(origClOrdStr.size()));
    OrderEntry* existing = orderStorage_->locateByClOrderId(rawKey);
    if (!existing) {
        aux::ExchLogger::instance()->error("FIX: Replace - order not found: " + origClOrdStr);
        return;
    }

    OrderEntry* replacement = existing->clone();

    FIX::Price price;
    if (msg.isSet(price)) {
        msg.get(price);
        replacement->price_ = price.getValue();
    }

    FIX::OrderQty orderQty;
    if (msg.isSet(orderQty)) {
        msg.get(orderQty);
        replacement->orderQty_ = static_cast<QuantityT>(orderQty.getValue());
        replacement->leavesQty_ = replacement->orderQty_;
    }

    FIX::TimeInForce tif;
    if (msg.isSet(tif)) {
        msg.get(tif);
        replacement->tif_ = toTif(tif.getValue());
    }

    std::string sourceStr = makeSourceString(sid);
    Queues::OrderReplaceEvent evt(existing->orderId_, replacement);
    inQueues_->push(sourceStr, evt);
}

// =============================================================================
// Outbound
// =============================================================================

bool FixGateway::hasFixSession(const std::string& source) const {
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
    return sessionMap_.find(source) != sessionMap_.end();
}

void FixGateway::sendExecutionReport(const ExecutionEntry* exec, const OrderEntry& order) {
    std::string source = order.source_.get();

    FIX::SessionID sid;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
        auto it = sessionMap_.find(source);
        if (it == sessionMap_.end()) return; // not a FIX-sourced order
        sid = it->second;
    }

    FIX44::ExecutionReport report(
        FIX::OrderID(std::to_string(order.orderId_.id_)),
        FIX::ExecID(std::to_string(exec->execId_.id_)),
        FIX::ExecType(fromExecType(exec->type_)),
        FIX::OrdStatus(fromOrdStatus(exec->orderStatus_)),
        FIX::Side(fromSide(order.side_)),
        FIX::LeavesQty(order.leavesQty_),
        FIX::CumQty(order.cumQty_),
        FIX::AvgPx(order.avgPx_));

    const auto& clOrd = order.clOrderId_.get();
    if (clOrd.data_ && clOrd.length_ > 0)
        report.set(FIX::ClOrdID(std::string(clOrd.data_, clOrd.length_)));

    report.set(FIX::Symbol(order.instrument_.get().symbol_));
    report.set(FIX::TransactTime(FIX::UtcTimeStamp()));

    if (exec->type_ == TRADE_EXECTYPE) {
        auto* trade = static_cast<const TradeExecEntry*>(exec);
        report.set(FIX::LastQty(trade->lastQty_));
        report.set(FIX::LastPx(trade->lastPx_));
    }

    // FX Swap leg: populate NoLegs group with leg-specific fields, set
    // TradeLinkID so clients can correlate near+far reports for the same swap.
    if (exec->execLegType_ != SINGLE_LEG) {
        FIX44::ExecutionReport::NoLegs legGroup;

        legGroup.set(FIX::LegSymbol(order.instrument_.get().symbol_));

        // Near leg trades at root side; far leg trades at opposite side
        char legFixSide;
        DateTimeT legSettl;
        if (exec->execLegType_ == NEAR_LEG) {
            legFixSide = fromSide(order.side_);
            legSettl = order.settlDate_;
        } else { // FAR_LEG
            legFixSide = (order.side_ == BUY_SIDE) ? FIX::Side_SELL : FIX::Side_BUY;
            legSettl = order.farSettlDate_;
        }
        legGroup.set(FIX::LegSide(legFixSide));

        if (exec->type_ == TRADE_EXECTYPE) {
            auto* trade = static_cast<const TradeExecEntry*>(exec);
            legGroup.set(FIX::LegLastPx(trade->lastPx_));
            // LegLastQty (tag 1418) has no typed class in QuickFIX's FIX 4.4 headers,
            // use untyped setField instead
            legGroup.setField(1418, std::to_string(trade->lastQty_));
        }

        // LegSettlDate is LocalMktDate (YYYYMMDD string in FIX)
        // We store DateTimeT (ms epoch); pass as string for simplicity
        if (legSettl > 0) {
            legGroup.set(FIX::LegSettlDate(std::to_string(legSettl)));
        }

        report.addGroup(legGroup);

        // Correlation: "<orderId>-<transactTime>" pairs near+far from same match.
        // Clients join on TradeLinkID (tag 820) to reconstruct the swap fill.
        // TradeLinkID has no typed class in FIX 4.4 headers; use untyped setField.
        report.setField(820,
            std::to_string(order.orderId_.id_) + "-" +
            std::to_string(exec->transactTime_));
    }

    FIX::Session::sendToTarget(report, sid);
}

void FixGateway::sendCancelReject(const IdT& orderId, const std::string& clOrdId) {
    // Find the session for this order — need to look up the order first
    OrderEntry* order = orderStorage_->locateByOrderId(orderId);
    if (!order) return;

    std::string source = order->source_.get();
    FIX::SessionID sid;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
        auto it = sessionMap_.find(source);
        if (it == sessionMap_.end()) return;
        sid = it->second;
    }

    FIX44::OrderCancelReject reject(
        FIX::OrderID(std::to_string(orderId.id_)),
        FIX::ClOrdID(clOrdId),
        FIX::OrigClOrdID(clOrdId),
        FIX::OrdStatus(fromOrdStatus(order->status_)),
        FIX::CxlRejResponseTo(FIX::CxlRejResponseTo_ORDER_CANCEL_REQUEST));

    FIX::Session::sendToTarget(reject, sid);
}

void FixGateway::sendBusinessReject(const IdT& refOrderId, const std::string& reason) {
    // Determine which FIX session to notify. If refOrderId is valid,
    // route to the originating session; otherwise, broadcast to all sessions.
    std::vector<FIX::SessionID> targets;

    if (refOrderId.isValid()) {
        OrderEntry* order = orderStorage_->locateByOrderId(refOrderId);
        if (order) {
            std::string source = order->source_.get();
            oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
            auto it = sessionMap_.find(source);
            if (it != sessionMap_.end()) targets.push_back(it->second);
        }
    } else {
        // No order reference — send to every live FIX session
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
        for (const auto& kv : sessionMap_) targets.push_back(kv.second);
    }

    for (const auto& sid : targets) {
        FIX44::BusinessMessageReject reject;
        reject.set(FIX::RefMsgType("D"));  // NewOrderSingle/NewOrderMultileg
        reject.set(FIX::BusinessRejectReason(FIX::BusinessRejectReason_OTHER));
        reject.set(FIX::Text(reason));
        if (refOrderId.isValid()) {
            reject.set(FIX::BusinessRejectRefID(std::to_string(refOrderId.id_)));
        }
        FIX::Session::sendToTarget(reject, sid);
    }
}

// =============================================================================
// Enum conversions
// =============================================================================

Side FixGateway::toSide(char fixSide) {
    switch (fixSide) {
        case FIX::Side_BUY:        return BUY_SIDE;
        case FIX::Side_SELL:       return SELL_SIDE;
        case FIX::Side_SELL_SHORT: return SELL_SHORT_SIDE;
        case '6':                  return CROSS_SIDE; // FIX Cross
        default:                   return INVALID_SIDE;
    }
}

OrderType FixGateway::toOrdType(char fixOrdType) {
    switch (fixOrdType) {
        case FIX::OrdType_MARKET:           return MARKET_ORDERTYPE;
        case FIX::OrdType_LIMIT:            return LIMIT_ORDERTYPE;
        case FIX::OrdType_STOP:             return STOP_ORDERTYPE;
        case FIX::OrdType_STOP_LIMIT:       return STOPLIMIT_ORDERTYPE;
        case FIX::OrdType_FOREX_SWAP:       return FXSWAP_ORDERTYPE;
        default:                            return INVALID_ORDERTYPE;
    }
}

TimeInForce FixGateway::toTif(char fixTif) {
    switch (fixTif) {
        case FIX::TimeInForce_DAY:                     return DAY_TIF;
        case FIX::TimeInForce_GOOD_TILL_CANCEL:        return GTC_TIF;
        case FIX::TimeInForce_IMMEDIATE_OR_CANCEL:     return IOC_TIF;
        case FIX::TimeInForce_FILL_OR_KILL:            return FOK_TIF;
        case FIX::TimeInForce_GOOD_TILL_DATE:          return GTD_TIF;
        case FIX::TimeInForce_AT_THE_OPENING:          return OPG_TIF;
        case FIX::TimeInForce_AT_THE_CLOSE:            return ATCLOSE_TIF;
        default:                                       return INVALID_TIF;
    }
}

Currency FixGateway::toCurrency(const std::string& fixCcy) {
    if (fixCcy == "USD") return USD_CURRENCY;
    if (fixCcy == "EUR") return EUR_CURRENCY;
    if (fixCcy == "GBP") return GBP_CURRENCY;
    if (fixCcy == "JPY") return JPY_CURRENCY;
    if (fixCcy == "CHF") return CHF_CURRENCY;
    if (fixCcy == "AUD") return AUD_CURRENCY;
    if (fixCcy == "CAD") return CAD_CURRENCY;
    if (fixCcy == "NZD") return NZD_CURRENCY;
    return INVALID_CURRENCY;
}

char FixGateway::fromOrdStatus(OrderStatus s) {
    switch (s) {
        case NEW_ORDSTATUS:            return FIX::OrdStatus_NEW;
        case PARTFILL_ORDSTATUS:       return FIX::OrdStatus_PARTIALLY_FILLED;
        case FILLED_ORDSTATUS:         return FIX::OrdStatus_FILLED;
        case CANCELED_ORDSTATUS:       return FIX::OrdStatus_CANCELED;
        case REJECTED_ORDSTATUS:       return FIX::OrdStatus_REJECTED;
        case REPLACED_ORDSTATUS:       return FIX::OrdStatus_REPLACED;
        case PENDINGNEW_ORDSTATUS:     return FIX::OrdStatus_PENDING_NEW;
        case PENDINGREPLACE_ORDSTATUS: return FIX::OrdStatus_PENDING_REPLACE;
        case EXPIRED_ORDSTATUS:        return FIX::OrdStatus_EXPIRED;
        case SUSPENDED_ORDSTATUS:      return FIX::OrdStatus_SUSPENDED;
        case DFD_ORDSTATUS:            return FIX::OrdStatus_DONE_FOR_DAY;
        default:                       return FIX::OrdStatus_NEW;
    }
}

char FixGateway::fromExecType(ExecType t) {
    switch (t) {
        case NEW_EXECTYPE:          return FIX::ExecType_NEW;
        case TRADE_EXECTYPE:        return FIX::ExecType_TRADE;
        case CANCEL_EXECTYPE:       return FIX::ExecType_CANCELED;
        case REJECT_EXECTYPE:       return FIX::ExecType_REJECTED;
        case REPLACE_EXECTYPE:      return FIX::ExecType_REPLACED;
        case EXPIRED_EXECTYPE:      return FIX::ExecType_EXPIRED;
        case SUSPENDED_EXECTYPE:    return FIX::ExecType_SUSPENDED;
        case PEND_CANCEL_EXECTYPE:  return FIX::ExecType_PENDING_CANCEL;
        case PEND_REPLACE_EXECTYPE: return FIX::ExecType_PENDING_REPLACE;
        case DFD_EXECTYPE:          return FIX::ExecType_DONE_FOR_DAY;
        case STATUS_EXECTYPE:       return FIX::ExecType_ORDER_STATUS;
        case RESTATED_EXECTYPE:     return FIX::ExecType_RESTATED;
        default:                    return FIX::ExecType_NEW;
    }
}

char FixGateway::fromSide(Side s) {
    switch (s) {
        case BUY_SIDE:        return FIX::Side_BUY;
        case SELL_SIDE:       return FIX::Side_SELL;
        case SELL_SHORT_SIDE: return FIX::Side_SELL_SHORT;
        case CROSS_SIDE:      return FIX::Side_CROSS;
        default:              return FIX::Side_BUY;
    }
}

#endif // BUILD_FIX
