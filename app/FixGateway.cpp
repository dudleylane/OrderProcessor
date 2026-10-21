#ifdef BUILD_FIX

#include "FixGateway.h"
#include "ExchUtils.h"
#include "WideDataStorage.h"
#include "OrderStorage.h"
#include "IdTGenerator.h"
#include "Logger.h"
#include "EnumStrings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include <quickfix/Exceptions.h>
#include <quickfix/FixValues.h>
#include <quickfix/FixFields.h>
#include <quickfix/fixt11/Reject.h>

using namespace COP;
using namespace COP::App;
using namespace COP::Store;

namespace
{
/// Exceptions contained in fromApp() since the process started. Each is logged with its number, so the
/// count stays visible in the log.
std::atomic<u64> containedExceptions{ 0 };

/// Longest prefix of an inbound message that goes into the log.
const size_t MAX_LOGGED_MESSAGE = 256;
/// Longest prefix of one of its fields that goes into the log
const size_t MAX_LOGGED_VALUE = 32;
/// OrdRejReason (103) value 0, "Broker / Exchange option" in FIX 5.0 SP2, as in 4.4. QuickFIX names it
/// OrdRejReason_BROKER_CREDIT, its FIX 4.2 meaning. The engine's reject reasons are free text, so every reject goes out
/// with this code and the reason itself in Text (58).
const int ORD_REJ_REASON_BROKER_OPTION = 0;

/// Why a cancel was refused, for Text (58). The words match App::cancelRejectReason(), which WebSocket clients get;
/// that lives in JsonSerializer.cpp, which the FIX benchmark does not build.
std::string cancelRejectText(const Queues::CancelRejectEvent &evnt)
{
    switch (evnt.reason_)
    {
    case Queues::CancelRejectEvent::UNKNOWN_ORDER:
        return "Cancel rejected: unknown order";
    case Queues::CancelRejectEvent::TOO_LATE:
        return "Cancel rejected: too late, the order is " + std::string(toJsonString(evnt.ordStatus_));
    case Queues::CancelRejectEvent::PENDING:
        return "Cancel rejected: a replace of the order is pending";
    default:
        return "Cancel rejected";
    }
}
/// A RawDataEntry's text; empty when it has none
std::string rawText(const RawDataEntry &raw)
{
    return ((nullptr != raw.data_) && (0 < raw.length_)) ? std::string(raw.data_, raw.length_) : std::string();
}
/// The session string an order came from, which prefixes the keys of its ClOrdIDs (#71); empty when it names none
std::string sourceOf(const OrderEntry &order)
{
    return (SourceIdT() != order.source_.getId()) ? order.source_.get() : std::string();
}
/// CxlRejReason (102) for the engine's reason, the same for a refused cancel (434=1) and a refused replace (434=2)
int cxlRejReasonFor(Queues::CancelRejectEvent::Reason reason)
{
    switch (reason)
    {
    case Queues::CancelRejectEvent::UNKNOWN_ORDER:
        return FIX::CxlRejReason_UNKNOWN_ORDER;
    case Queues::CancelRejectEvent::TOO_LATE:
        return FIX::CxlRejReason_TOO_LATE_TO_CANCEL;
    case Queues::CancelRejectEvent::PENDING:
        return FIX::CxlRejReason_ORDER_ALREADY_IN_PENDING_STATUS;
    default:
        return FIX::CxlRejReason_OTHER;
    }
}
/// Why an OrderQty was refused, in the terms of FixGateway::toQuantity() (#145)
std::string quantityRefusal()
{
    return "OrderQty (38) must be a whole number from 1 to " + std::to_string(std::numeric_limits<QuantityT>::max());
}
/// Why a price in field was refused, in the terms of FixGateway::isTradablePrice() (#145)
std::string priceRefusal(const std::string &field, OrderType ordType)
{
    return field + ((MARKET_ORDERTYPE == ordType) ? " must be a finite number" : " must be a finite number above zero");
}
} // namespace

// =============================================================================
// Construction
// =============================================================================

FixGateway::FixGateway(Queues::InQueues *inQueues, WideParamsDataStorage *wideData, OrderDataStorage *orderStorage,
                       SourceIdT defaultClearingId, SourceIdT defaultAccountId)
    : inQueues_(inQueues), wideData_(wideData), orderStorage_(orderStorage), defaultClearingId_(defaultClearingId),
      defaultAccountId_(defaultAccountId)
{
}

std::string FixGateway::makeSourceString(const FIX::SessionID &sid)
{
    return "FIX:" + sid.getSenderCompID().getString() + "->" + sid.getTargetCompID().getString();
}

std::string FixGateway::sessionClOrdId(const std::string &source, const std::string &clOrdId)
{
    return source + '|' + clOrdId;
}

std::string FixGateway::clientClOrdId(const std::string &stored, const std::string &source)
{
    // Only the exact prefix: a client's ClOrdID may itself contain '|'
    if (!source.empty() && (stored.size() > source.size()) && ('|' == stored[source.size()]) &&
        (0 == stored.compare(0, source.size(), source)))
    {
        return stored.substr(source.size() + 1);
    }
    return stored;
}

Queues::OrderRejectEvent FixGateway::clientView(const Queues::OrderRejectEvent &evnt, const std::string &source)
{
    Queues::OrderRejectEvent shown = evnt;
    shown.clOrderId_ = clientClOrdId(evnt.clOrderId_, source);
    shown.origClOrderId_ = clientClOrdId(evnt.origClOrderId_, source);
    if (!source.empty())
    {
        const std::string prefix = source + '|';
        for (size_t at = shown.reason_.find(prefix); std::string::npos != at; at = shown.reason_.find(prefix, at))
        {
            shown.reason_.erase(at, prefix.size());
        }
    }
    return shown;
}

OrderEntry *FixGateway::locateSessionOrder(const std::string &source, const std::string &clOrdId) const
{
    const std::string key = sessionClOrdId(source, clOrdId);
    RawDataEntry sessionKey(STRING_RAWDATATYPE, key.c_str(), static_cast<u32>(key.size()));
    if (OrderEntry *order = orderStorage_->locateByClOrderId(sessionKey))
    {
        return order;
    }
    RawDataEntry rawKey(STRING_RAWDATATYPE, clOrdId.c_str(), static_cast<u32>(clOrdId.size()));
    OrderEntry *order = orderStorage_->locateByClOrderId(rawKey);
    if (nullptr == order)
    {
        return nullptr;
    }
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(order->entryMutex_, false);
    return (source == sourceOf(*order)) ? order : nullptr;
}

// =============================================================================
// Application callbacks
// =============================================================================

void FixGateway::onCreate(const FIX::SessionID &sid)
{
    aux::ExchLogger::instance()->note("FIX session created: " + sid.toString());
    // Known from now on, logged on or not (#160). The map used to hold a session only while it was logged on, so a report
    // for its order was dropped while the client was away, and a ResendRequest couldn't get it back.
    std::string source = makeSourceString(sid);
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, true);
    sessionMap_[source] = sid;
}

void FixGateway::onLogon(const FIX::SessionID &sid)
{
    aux::ExchLogger::instance()->note("FIX logon: " + sid.toString());
    std::string source = makeSourceString(sid);
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, true);
    sessionMap_[source] = sid;
    loggedOn_.insert(source);
}

void FixGateway::onLogout(const FIX::SessionID &sid)
{
    aux::ExchLogger::instance()->note("FIX logout: " + sid.toString());
    std::string source = makeSourceString(sid);
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, true);
    loggedOn_.erase(source); // the session stays known: QuickFIX keeps what it is sent for the client's return (#160)
}

void FixGateway::setCredentials(const FixCredentials *credentials)
{
    if (nullptr != credentials)
    {
        for (const FixCounterparty &counterparty : credentials->counterparties())
        {
            for (const std::string &account : counterparty.accounts)
            {
                if (!wideData_->findAccountByName(account).isValid())
                {
                    throw std::runtime_error("the FIX credentials give " + counterparty.compId + " the account " +
                                             account + ", which the data directory doesn't have");
                }
            }
        }
    }
    credentials_ = credentials;
}

void FixGateway::toAdmin(FIX::Message & /*msg*/, const FIX::SessionID & /*sid*/) {}
void FixGateway::toApp(FIX::Message & /*msg*/, const FIX::SessionID & /*sid*/) {}

void FixGateway::fromAdmin(const FIX::Message &msg, const FIX::SessionID &sid)
{
    // QuickFIX calls this with a Logon before it accepts it, and refuses the Logon if this throws RejectLogon: it answers
    // with a Logout carrying the text, and disconnects (#147). Before #147 nothing was checked here.
    const FIX::Header &header = msg.getHeader();
    if (!header.isSetField(FIX::FIELD::MsgType) || (FIX::MsgType_Logon != header.getField(FIX::FIELD::MsgType)))
    {
        return;
    }
    // The session checks the counterparty's messages against the dictionary of the version its Logon names in
    // DefaultApplVerID (1137), and has FIX 5.0 SP2's alone: with any other it would check them against none
    // (#377). QuickFIX refuses a Logon without one itself.
    const std::string version =
        msg.isSetField(FIX::FIELD::DefaultApplVerID) ? msg.getField(FIX::FIELD::DefaultApplVerID) : "";
    if (FIX::ApplVerID_FIX50_SP2 != version)
    {
        aux::ExchLogger::instance()->warn("FIX logon refused for " + sid.toString() +
                                          ": its DefaultApplVerID (1137) is '" + version.substr(0, MAX_LOGGED_VALUE) +
                                          "', not 9, FIX 5.0 SP2 (#377)");
        throw FIX::RejectLogon("Logon refused: this acceptor speaks FIX 5.0 SP2 alone, so DefaultApplVerID (1137) must "
                               "be 9");
    }
    if (nullptr == credentials_)
    {
        return;
    }
    const FixCounterparty *counterparty = credentials_->find(sid.getTargetCompID().getString());
    const std::string username = msg.isSetField(FIX::FIELD::Username) ? msg.getField(FIX::FIELD::Username) : "";
    const std::string password = msg.isSetField(FIX::FIELD::Password) ? msg.getField(FIX::FIELD::Password) : "";
    std::string refusal;
    if (nullptr == counterparty)
    {
        refusal = "the credentials name no counterparty with its CompID";
    }
    else if (username.empty() || password.empty())
    {
        refusal = "it carries no Username (553) or no Password (554)";
    }
    else if (!FixCredentials::verify(*counterparty, username, password))
    {
        refusal = "the Username or Password is wrong";
    }
    if (!refusal.empty())
    {
        // The reason goes to the log only; the client learns no more than that it was refused
        aux::ExchLogger::instance()->warn("FIX logon refused for " + sid.toString() + ": " + refusal + " (#147)");
        throw FIX::RejectLogon("Logon refused: wrong username or password");
    }
}

void FixGateway::fromApp(const FIX::Message &msg, const FIX::SessionID &sid)
{
    // A message may name its own version in ApplVerID (1128), and the session checked it against that version's
    // dictionary: against none, for any version but FIX 5.0 SP2 (#377)
    FIX::ApplVerID applVerId;
    if (msg.getHeader().getFieldIfSet(applVerId) && (FIX::ApplVerID_FIX50_SP2 != applVerId.getValue()))
    {
        rejectApplVerId(msg, sid);
        return;
    }
    // QuickFIX calls this on the session's own thread and handles only FIX::Exception there: anything else
    // leaves the thread and terminates the server (#36). Contain it and reject the one message instead.
    try
    {
        dispatch(msg, sid);
    }
    catch (const FIX::FieldNotFound &ex)
    {
        // A field a handler needs that FIX 5.0 SP2 doesn't require, such as a cancel's OrigClOrdID (41). Answered as
        // QuickFIX answers it, but here: QuickFIX's BusinessMessageReject carries DefaultApplVerID (1137) on a FIXT
        // session, which that message doesn't have, so a counterparty that validates refused it (#377,
        // dudleylane/quickfix#173).
        rejectMessage(msg, sid, FIX::BusinessRejectReason_CONDITIONALLY_REQUIRED_FIELD_MISSING,
                      std::string(FIX::BusinessRejectReason_CONDITIONALLY_REQUIRED_FIELD_MISSING_TEXT) + " (" +
                          std::to_string(ex.field) + ")");
    }
    catch (const FIX::Exception &)
    {
        // QuickFIX answers these itself with the right session-level reject
        throw;
    }
    catch (const std::exception &ex)
    {
        rejectContained(msg, sid, ex.what());
    }
    catch (...)
    {
        rejectContained(msg, sid, "unknown exception");
    }
}

void FixGateway::rejectContained(const FIX::Message &msg, const FIX::SessionID &sid, const char *what)
{
    const u64 n = containedExceptions.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string text = msg.toString();
    std::replace(text.begin(), text.end(), '\x01', '|');
    aux::ExchLogger::instance()->error("FixGateway: contained exception #" + std::to_string(n) + " from " +
                                       sid.toString() + ": " + what +
                                       "; message: " + text.substr(0, MAX_LOGGED_MESSAGE));
    rejectMessage(msg, sid, FIX::BusinessRejectReason_OTHER, "Internal error: message not processed");
}

void FixGateway::rejectMessage(const FIX::Message &msg, const FIX::SessionID &sid, int reason, const std::string &text)
{
    try
    {
        FIX50SP2::BusinessMessageReject reject;
        FIX::MsgSeqNum seqNum;
        if (msg.getHeader().getFieldIfSet(seqNum))
        {
            reject.set(FIX::RefSeqNum(seqNum.getValue()));
        }
        FIX::MsgType msgType;
        if (msg.getHeader().getFieldIfSet(msgType))
        {
            reject.set(FIX::RefMsgType(msgType.getValue()));
        }
        FIX::ClOrdID clOrdId;
        if (msg.getFieldIfSet(clOrdId))
        {
            reject.set(FIX::BusinessRejectRefID(clOrdId.getValue()));
        }
        reject.set(FIX::BusinessRejectReason(reason));
        reject.set(FIX::Text(text));
        FIX::Session::sendToTarget(reject, sid);
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the reject: ") + ex.what());
    }
}

void FixGateway::rejectApplVerId(const FIX::Message &msg, const FIX::SessionID &sid)
{
    const std::string version = msg.getHeader().getField(FIX::FIELD::ApplVerID);
    aux::ExchLogger::instance()->warn("FIX: " + sid.toString() + " sent a message of ApplVerID (1128) '" +
                                      version.substr(0, MAX_LOGGED_VALUE) + "', not 9: refused (#377)");
    try
    {
        // A session-level Reject, as QuickFIX sends for a message its dictionary refuses, with FIXT.1.1's reason for
        // this one, which QuickFIX has no exception for
        FIX::MsgSeqNum seqNum;
        msg.getHeader().getField(seqNum);
        FIXT11::Reject reject(FIX::RefSeqNum(seqNum.getValue()));
        reject.set(FIX::RefTagID(FIX::FIELD::ApplVerID));
        reject.set(FIX::RefMsgType(msg.getHeader().getField(FIX::FIELD::MsgType)));
        reject.set(FIX::RefApplVerID(version));
        reject.set(FIX::SessionRejectReason(FIX::SessionRejectReason_INVALID_UNSUPPORTED_APPLICATION_VERSION));
        reject.set(FIX::Text("Unsupported Application Version: FIX 5.0 SP2, ApplVerID 9, alone"));
        FIX::Session::sendToTarget(reject, sid);
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the reject: ") + ex.what());
    }
}

void FixGateway::refuseOrder(const FIX::Message &msg, const FIX::SessionID &sid, int ordRejReason,
                             const std::string &text)
{
    try
    {
        // Its ExecID comes from the shared generator, as an engine reject's does (#81)
        FIX50SP2::ExecutionReport report =
            buildOrderRefusal(msg, ordRejReason, text, IdTGenerator::instance()->getId().id_);
        FIX::Session::sendToTarget(report, sid);
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the order refusal: ") + ex.what());
    }
}

void FixGateway::dispatch(const FIX::Message &msg, const FIX::SessionID &sid)
{
    // Build the handler's message type from the generic message rather than casting to it. The session hands
    // over a FIX::Message; FIX50SP2::MessageCracker::crack() takes a FIX50SP2::Message, so it received a temporary
    // copy and cast that to NewOrderSingle and the rest, types it is not: undefined behaviour on every inbound
    // message, which UBSan reports. Constructing the typed message costs the same single copy.
    FIX::MsgType msgType;
    msg.getHeader().getField(msgType);
    const std::string &type = msgType.getValue();
    if (FIX::MsgType_NewOrderSingle == type)
    {
        onMessage(FIX50SP2::NewOrderSingle(msg), sid);
    }
    else if (FIX::MsgType_NewOrderMultileg == type)
    {
        onMessage(FIX50SP2::NewOrderMultileg(msg), sid);
    }
    else if (FIX::MsgType_OrderCancelRequest == type)
    {
        onMessage(FIX50SP2::OrderCancelRequest(msg), sid);
    }
    else if (FIX::MsgType_OrderCancelReplaceRequest == type)
    {
        onMessage(FIX50SP2::OrderCancelReplaceRequest(msg), sid);
    }
    else
    {
        // Answered as QuickFIX answers the cracker's UnsupportedMessageType, but here, as fromApp() answers a missing
        // field: QuickFIX's BusinessMessageReject carries 1137, which a counterparty that validates refuses (#377,
        // dudleylane/quickfix#173)
        rejectMessage(msg, sid, FIX::BusinessRejectReason_UNSUPPORTED_MESSAGE_TYPE,
                      FIX::BusinessRejectReason_UNSUPPORTED_MESSAGE_TYPE_TEXT);
    }
}

// =============================================================================
// Inbound message handlers
// =============================================================================

void FixGateway::onMessage(const FIX50SP2::NewOrderSingle &msg, const FIX::SessionID &sid)
{
    FIX::ClOrdID clOrdId;
    msg.get(clOrdId);
    FIX::Symbol symbol;
    msg.get(symbol);
    FIX::Side side;
    msg.get(side);
    FIX::OrdType ordType;
    msg.get(ordType);
    FIX::OrderQty orderQty;
    msg.get(orderQty);
    FIX::TransactTime transTime;
    msg.get(transTime);

    FIX::Price price;
    double priceVal = 0.0;
    if (msg.isSet(price))
    {
        msg.get(price);
        priceVal = price.getValue();
    }

    FIX::TimeInForce tif;
    char tifVal = '0'; // default Day
    if (msg.isSet(tif))
    {
        msg.get(tif);
        tifVal = tif.getValue();
    }

    FIX::Account account;
    std::string acctStr;
    if (msg.isSet(account))
    {
        msg.get(account);
        acctStr = account.getString();
    }

    FIX::Currency currency;
    std::string ccyStr = "USD";
    if (msg.isSet(currency))
    {
        msg.get(currency);
        ccyStr = currency.getString();
    }

    // Lookup instrument
    SourceIdT instrId = wideData_->findInstrumentBySymbol(symbol.getString());
    if (!instrId.isValid())
    {
        // Answered, as every other refusal is: the client used to wait for a report that never came (#189)
        refuseOrder(msg, sid, FIX::OrdRejReason_UNKNOWN_SYMBOL, "Order refused: unknown symbol " + symbol.getString());
        return;
    }

    // The session's own accounts (#147): one it may trade, its first if the order names none
    if (!applySessionAccounts(msg, sid, &acctStr))
    {
        return;
    }
    // Look up the account; an order that names none goes to the server's default account (#34)
    SourceIdT acctId = defaultAccountId_;
    if (!acctStr.empty())
    {
        acctId = wideData_->findAccountByName(acctStr);
    }
    else if (!acctId.isValid())
    {
        rejectMessage(msg, sid, FIX::BusinessRejectReason_CONDITIONALLY_REQUIRED_FIELD_MISSING,
                      "Order refused: it names no account, and the server has no default account");
        return;
    }

    // Every order clears through the server's clearing firm (#34)
    if (!defaultClearingId_.isValid())
    {
        rejectMessage(msg, sid, FIX::BusinessRejectReason_APPLICATION_NOT_AVAILABLE,
                      "Order refused: the server has no clearing firm");
        return;
    }

    // The quantity and prices are checked before they are narrowed or anything is stored (#145). OrdRejReason 13 is
    // FIX's code for an incorrect quantity; a price has no code of its own, so Text says what is wrong with it.
    const std::optional<QuantityT> qty = toQuantity(orderQty.getValue());
    if (!qty)
    {
        refuseOrder(msg, sid, FIX::OrdRejReason_INCORRECT_QUANTITY, "Order refused: " + quantityRefusal());
        return;
    }
    const OrderType ordTypeVal = toOrdType(ordType.getValue());
    if (msg.isSet(price) && !isTradablePrice(priceVal, ordTypeVal))
    {
        refuseOrder(msg, sid, ORD_REJ_REASON_BROKER_OPTION, "Order refused: " + priceRefusal("Price (44)", ordTypeVal));
        return;
    }
    // A stop order's stop price, StopPx (99) (#223): the engine holds a stop order until a trade reaches it, and refuses
    // one without a stop price above 0, so the gateway reads it, and refuses a stop order without one first, as it does a
    // Price (44) no order trades at (#145)
    double stopPxVal = 0.0;
    if ((STOP_ORDERTYPE == ordTypeVal) || (STOPLIMIT_ORDERTYPE == ordTypeVal))
    {
        FIX::StopPx stopPx;
        if (!msg.isSet(stopPx))
        {
            refuseOrder(msg, sid, ORD_REJ_REASON_BROKER_OPTION, "Order refused: a stop order needs StopPx (99)");
            return;
        }
        msg.get(stopPx);
        stopPxVal = stopPx.getValue();
        if (!isTradablePrice(stopPxVal, ordTypeVal))
        {
            refuseOrder(msg, sid, ORD_REJ_REASON_BROKER_OPTION,
                        "Order refused: " + priceRefusal("StopPx (99)", ordTypeVal));
            return;
        }
    }
    double farPrice = 0.0;
    if ((FXSWAP_ORDERTYPE == ordTypeVal) && msg.isSetField(6500))
    {
        farPrice = std::stod(msg.getField(6500));
        if (!isTradablePrice(farPrice, ordTypeVal))
        {
            refuseOrder(msg, sid, ORD_REJ_REASON_BROKER_OPTION,
                        "Order refused: " + priceRefusal("the far leg's price (6500)", ordTypeVal));
            return;
        }
    }

    // A swap's settlement dates, read before anything is stored (#190)
    DateTimeT settlDate = 0;
    DateTimeT farSettlDate = 0;
    if ((FXSWAP_ORDERTYPE == ordTypeVal) &&
        (!readSettlDate(msg, msg, sid, FIX::FIELD::SettlDate, "SettlDate (64)", &settlDate) ||
         !readSettlDate(msg, msg, sid, 6501, "the far leg's SettlDate (6501)", &farSettlDate)))
    {
        return;
    }

    // A GTD order's ExpireTime (126) (#240), read before anything is stored, as the prices and dates are
    DateTimeT expireTimeVal = 0;
    if (const std::optional<std::string> refusal =
            expiryRefusal(msg, toTif(tifVal), aux::currentDateTime(), &expireTimeVal))
    {
        refuseOrder(msg, sid, ORD_REJ_REASON_BROKER_OPTION, "Order refused: " + *refusal);
        return;
    }

    // The ClOrdID is stored under the session's key, since FIX scopes it to the client that sent it (#71)
    std::string sourceStr = makeSourceString(sid);
    std::string clOrdStr = sessionClOrdId(sourceStr, clOrdId.getString());
    auto *clOrdRaw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(), static_cast<u32>(clOrdStr.size()));
    // The order's strings go in its own record (#192): adding each wrote a record, a commit of its own
    SourceIdT clOrdSrcId = WideDataStorage::instance()->addUnsaved(clOrdRaw);

    SourceIdT emptyId;

    SourceIdT clearingId = defaultClearingId_;

    auto *execList = new ExecutionsT();
    SourceIdT execListId = WideDataStorage::instance()->add(execList);

    auto *srcStrPtr = new StringT(sourceStr);
    SourceIdT srcId = WideDataStorage::instance()->addUnsaved(srcStrPtr);

    auto *destStr = new StringT("Internal");
    SourceIdT destId = WideDataStorage::instance()->addUnsaved(destStr);

    auto *order = new OrderEntry(srcId, destId, clOrdSrcId, emptyId, instrId, acctId, clearingId, execListId);
    order->side_ = toSide(side.getValue());
    order->ordType_ = ordTypeVal;
    order->price_ = priceVal;
    order->stopPx_ = stopPxVal;
    order->orderQty_ = *qty;
    order->leavesQty_ = order->orderQty_;
    order->tif_ = toTif(tifVal);
    order->expireTime_ = expireTimeVal;
    order->currency_ = toCurrency(ccyStr);
    order->capacity_ = AGENCY_CAPACITY;
    order->settlType_ = _2_SETTLTYPE;
    order->status_ = RECEIVEDNEW_ORDSTATUS;
    order->creationTime_ = static_cast<DateTimeT>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    order->lastUpdateTime_ = order->creationTime_;

    // FX Swap: extract far-leg fields from user-defined tags (6500=farPrice, 6501=farSettlDate)
    if (FXSWAP_ORDERTYPE == order->ordType_)
    {
        if (msg.isSetField(6500))
        {
            order->farPrice_ = farPrice; // read and checked above (#145)
        }
        order->farSettlDate_ = farSettlDate; // 6501, read and checked above (#190)
        order->settlDate_ = settlDate;       // SettlDate (64), the near leg's
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
void FixGateway::onMessage(const FIX50SP2::NewOrderMultileg &msg, const FIX::SessionID &sid)
{
    FIX::ClOrdID clOrdId;
    msg.get(clOrdId);
    FIX::Side side;
    msg.get(side);
    FIX::OrdType ordType;
    msg.get(ordType);
    FIX::OrderQty orderQty;
    msg.get(orderQty);

    // Only handle FX Swap multileg orders
    if (FXSWAP_ORDERTYPE != toOrdType(ordType.getValue()))
    {
        // To its sender alone: these three refusals went to every session logged on, naming no order (#189)
        refuseOrder(msg, sid, FIX::OrdRejReason_UNSUPPORTED_ORDER_CHARACTERISTIC,
                    "Order refused: a NewOrderMultileg must be an FX swap, OrdType (40) G");
        return;
    }

    FIX::Symbol rootSymbol;
    std::string symStr;
    if (msg.isSet(rootSymbol))
    {
        msg.get(rootSymbol);
        symStr = rootSymbol.getString();
    }

    // Extract legs — use groupCount() rather than isSet(NoLegs) because
    // addGroup() sets up the count implicitly and isSet() can throw on
    // partial/invalid count fields.
    size_t legCount = msg.groupCount(FIX::FIELD::NoLegs);
    if (legCount < 2)
    {
        refuseOrder(msg, sid, FIX::OrdRejReason_UNSUPPORTED_ORDER_CHARACTERISTIC,
                    "Order refused: an FX swap needs 2 legs, not " + std::to_string(legCount));
        return;
    }

    double nearPrice = 0.0, farPrice = 0.0;
    DateTimeT nearSettlDate = 0, farSettlDate = 0;
    std::string legSymbol;

    FIX50SP2::NewOrderMultileg::NoLegs legGroup;
    for (int i = 1; i <= 2; ++i)
    {
        msg.getGroup(i, legGroup);

        FIX::LegSymbol lsym;
        if (legGroup.isSet(lsym))
        {
            legGroup.get(lsym);
            if (legSymbol.empty())
            {
                legSymbol = lsym.getString();
            }
        }

        FIX::LegPrice lpx;
        double lpxVal = 0.0;
        if (legGroup.isSet(lpx))
        {
            legGroup.get(lpx);
            lpxVal = lpx.getValue();
            // Checked before anything is stored, as the quantity is below (#145)
            if (!isTradablePrice(lpxVal, FXSWAP_ORDERTYPE))
            {
                refuseOrder(msg, sid, ORD_REJ_REASON_BROKER_OPTION,
                            "Order refused: " + priceRefusal("LegPrice (566)", FXSWAP_ORDERTYPE));
                return;
            }
        }

        DateTimeT lsdVal = 0;
        if (!readSettlDate(legGroup, msg, sid, FIX::FIELD::LegSettlDate, "LegSettlDate (588)", &lsdVal))
        {
            return;
        }

        FIX::LegSide lside;
        char legSideVal = '0';
        if (legGroup.isSet(lside))
        {
            legGroup.get(lside);
            legSideVal = lside.getValue();
        }

        // Near leg matches root side; far leg is opposite
        if (legSideVal == side.getValue())
        {
            nearPrice = lpxVal;
            nearSettlDate = lsdVal;
        }
        else
        {
            farPrice = lpxVal;
            farSettlDate = lsdVal;
        }
    }

    // Prefer root Symbol, fall back to LegSymbol
    if (symStr.empty())
    {
        symStr = legSymbol;
    }

    SourceIdT instrId = wideData_->findInstrumentBySymbol(symStr);
    if (!instrId.isValid())
    {
        refuseOrder(msg, sid, FIX::OrdRejReason_UNKNOWN_SYMBOL, "Order refused: unknown symbol " + symStr);
        return;
    }

    FIX::Account account;
    std::string acctStr;
    if (msg.isSet(account))
    {
        msg.get(account);
        acctStr = account.getString();
    }
    // As for NewOrderSingle: the session's own accounts (#147), else the server's default, and its clearing firm (#34)
    if (!applySessionAccounts(msg, sid, &acctStr))
    {
        return;
    }
    SourceIdT acctId = defaultAccountId_;
    if (!acctStr.empty())
    {
        acctId = wideData_->findAccountByName(acctStr);
    }
    else if (!acctId.isValid())
    {
        rejectMessage(msg, sid, FIX::BusinessRejectReason_CONDITIONALLY_REQUIRED_FIELD_MISSING,
                      "Order refused: it names no account, and the server has no default account");
        return;
    }
    if (!defaultClearingId_.isValid())
    {
        rejectMessage(msg, sid, FIX::BusinessRejectReason_APPLICATION_NOT_AVAILABLE,
                      "Order refused: the server has no clearing firm");
        return;
    }
    // As for NewOrderSingle, before the quantity is narrowed or anything is stored (#145)
    const std::optional<QuantityT> qty = toQuantity(orderQty.getValue());
    if (!qty)
    {
        refuseOrder(msg, sid, FIX::OrdRejReason_INCORRECT_QUANTITY, "Order refused: " + quantityRefusal());
        return;
    }

    FIX::Currency currency;
    std::string ccyStr = "USD";
    if (msg.isSet(currency))
    {
        msg.get(currency);
        ccyStr = currency.getString();
    }

    FIX::TimeInForce tif;
    char tifVal = FIX::TimeInForce_GOOD_TILL_CANCEL;
    if (msg.isSet(tif))
    {
        msg.get(tif);
        tifVal = tif.getValue();
    }

    // As for NewOrderSingle: a GTD swap's ExpireTime (126) (#240)
    DateTimeT expireTimeVal = 0;
    if (const std::optional<std::string> refusal =
            expiryRefusal(msg, toTif(tifVal), aux::currentDateTime(), &expireTimeVal))
    {
        refuseOrder(msg, sid, ORD_REJ_REASON_BROKER_OPTION, "Order refused: " + *refusal);
        return;
    }

    // Build OrderEntry — same pattern as NewOrderSingle, with the ClOrdID under the session's key (#71)
    std::string sourceStr = makeSourceString(sid);
    std::string clOrdStr = sessionClOrdId(sourceStr, clOrdId.getString());
    auto *clOrdRaw = new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(), static_cast<u32>(clOrdStr.size()));
    // The order's strings go in its own record (#192): adding each wrote a record, a commit of its own
    SourceIdT clOrdSrcId = WideDataStorage::instance()->addUnsaved(clOrdRaw);

    SourceIdT emptyId;
    SourceIdT clearingId = defaultClearingId_;

    auto *execList = new ExecutionsT();
    SourceIdT execListId = WideDataStorage::instance()->add(execList);

    auto *srcStrPtr = new StringT(sourceStr);
    SourceIdT srcId = WideDataStorage::instance()->addUnsaved(srcStrPtr);

    auto *destStr = new StringT("Internal");
    SourceIdT destId = WideDataStorage::instance()->addUnsaved(destStr);

    auto *order = new OrderEntry(srcId, destId, clOrdSrcId, emptyId, instrId, acctId, clearingId, execListId);
    order->side_ = toSide(side.getValue());
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->price_ = nearPrice;
    order->farPrice_ = farPrice;
    order->settlDate_ = nearSettlDate;
    order->farSettlDate_ = farSettlDate;
    order->orderQty_ = *qty;
    order->leavesQty_ = order->orderQty_;
    order->tif_ = toTif(tifVal);
    order->expireTime_ = expireTimeVal;
    order->currency_ = toCurrency(ccyStr);
    order->capacity_ = AGENCY_CAPACITY;
    order->settlType_ = _2_SETTLTYPE;
    order->status_ = RECEIVEDNEW_ORDSTATUS;
    order->creationTime_ = static_cast<DateTimeT>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    order->lastUpdateTime_ = order->creationTime_;

    Queues::OrderEvent evt(order);
    inQueues_->push(sourceStr, evt);
}

void FixGateway::onMessage(const FIX50SP2::OrderCancelRequest &msg, const FIX::SessionID &sid)
{
    FIX::OrigClOrdID origClOrdId;
    msg.get(origClOrdId);
    FIX::ClOrdID clOrdId;
    msg.get(clOrdId);

    // Only the session's own orders: another session's is answered as unknown, without saying that it exists (#71)
    const std::string sourceStr = makeSourceString(sid);
    std::string origClOrdStr = origClOrdId.getString();
    OrderEntry *order = locateSessionOrder(sourceStr, origClOrdStr);
    if (!order)
    {
        aux::ExchLogger::instance()->error("FIX: Cancel - order not found: " + origClOrdStr);
        // Answered here rather than by the engine, which only rejects an unknown order id (#57): here the request's
        // own ClOrdID is known, so the reject can quote it (#73)
        Queues::CancelRejectEvent unknown; // UNKNOWN_ORDER, with no order status
        try
        {
            FIX50SP2::OrderCancelReject reject = buildCancelReject(unknown, "NONE", clOrdId.getString(), origClOrdStr);
            FIX::Session::sendToTarget(reject, sid);
        }
        catch (const std::exception &ex)
        {
            aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the cancel reject: ") +
                                               ex.what());
        }
        return;
    }

    Queues::OrderCancelEvent evt(order->orderId_, "FIX cancel request");
    // The engine hands it back on the cancel's acknowledgement and on a reject, for their ClOrdID (11) (#98)
    evt.requestClOrdId_ = clOrdId.getString();
    inQueues_->push(sourceStr, evt);
}

void FixGateway::onMessage(const FIX50SP2::OrderCancelReplaceRequest &msg, const FIX::SessionID &sid)
{
    FIX::OrigClOrdID origClOrdId;
    msg.get(origClOrdId);
    FIX::ClOrdID clOrdId;
    msg.get(clOrdId);

    // Only the session's own orders, as for a cancel (#71)
    const std::string sourceStr = makeSourceString(sid);
    std::string origClOrdStr = origClOrdId.getString();
    OrderEntry *existing = locateSessionOrder(sourceStr, origClOrdStr);
    if (!existing)
    {
        aux::ExchLogger::instance()->error("FIX: Replace - order not found: " + origClOrdStr);
        // Answered here, as a cancel of an unknown order is (#73), quoting the request's own ClOrdIDs
        try
        {
            FIX50SP2::OrderCancelReject reject = buildUnknownReplaceReject(clOrdId.getString(), origClOrdStr);
            FIX::Session::sendToTarget(reject, sid);
        }
        catch (const std::exception &ex)
        {
            aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the replace reject: ") +
                                               ex.what());
        }
        return;
    }

    // The replacement is the original with the request's changes, a fresh id and the request's ClOrdID (11), naming the
    // original (#74). The engine decides it, and carries over what the original has filled.
    std::unique_ptr<OrderEntry> replacement;
    std::string origOrderId;
    char origStatus = FIX::OrdStatus_NEW;
    {
        // read lock: a transaction worker may be updating the original
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(existing->entryMutex_, false);
        replacement.reset(existing->clone());
        origOrderId = std::to_string(existing->orderId_.id_);
        origStatus = fromOrdStatus(existing->status_);
    }

    // A request the gateway refuses itself gets an OrderCancelReject (102=99) that says why, quoting its ClOrdIDs
    auto refuse = [&](const std::string &reason)
    {
        Queues::OrderRejectEvent refusal;
        refusal.replacement_ = true;
        refusal.clOrderId_ = clOrdId.getString();
        refusal.origClOrderId_ = origClOrdStr;
        refusal.refusal_ = Queues::CancelRejectEvent::OTHER;
        refusal.reason_ = reason;
        try
        {
            FIX50SP2::OrderCancelReject reject = buildReplaceReject(refusal, origOrderId, origStatus);
            FIX::Session::sendToTarget(reject, sid);
        }
        catch (const std::exception &ex)
        {
            aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the replace reject: ") +
                                               ex.what());
        }
    };

    // Side and Symbol name the order, and FIX requires them to match it; carried over, so that the engine refuses a
    // replace that changes either, rather than keeping the original's without a word
    FIX::Side side;
    if (msg.isSet(side))
    {
        msg.get(side);
        replacement->side_ = toSide(side.getValue());
    }
    FIX::Symbol symbol;
    if (msg.isSet(symbol))
    {
        msg.get(symbol);
        const SourceIdT instrument = wideData_->findInstrumentBySymbol(symbol.getString());
        if (!instrument.isValid())
        {
            // Refused here: the engine's refusal can only describe an instrument that exists
            refuse("Replace refused: unknown instrument " + symbol.getString());
            return;
        }
        replacement->instrument_ = instrument;
    }

    // The price and quantity are checked before they are narrowed, and before the request's ClOrdID is stored (#145)
    FIX::Price price;
    if (msg.isSet(price))
    {
        msg.get(price);
        if (!isTradablePrice(price.getValue(), replacement->ordType_))
        {
            refuse("Replace refused: " + priceRefusal("Price (44)", replacement->ordType_));
            return;
        }
        replacement->price_ = price.getValue();
    }

    // StopPx (99) moves a stop order's stop (#238), checked as a new stop order's is (#223). The type is the original's,
    // which a replace doesn't change. Whether the stop was already reached is the engine's to decide: a release can run
    // before the replace does.
    FIX::StopPx stopPx;
    if (msg.isSet(stopPx))
    {
        msg.get(stopPx);
        if ((STOP_ORDERTYPE != replacement->ordType_) && (STOPLIMIT_ORDERTYPE != replacement->ordType_))
        {
            refuse("Replace refused: StopPx (99) is only for a stop or stop limit order");
            return;
        }
        if (!isTradablePrice(stopPx.getValue(), replacement->ordType_))
        {
            refuse("Replace refused: " + priceRefusal("StopPx (99)", replacement->ordType_));
            return;
        }
        replacement->stopPx_ = stopPx.getValue();
    }

    FIX::OrderQty orderQty;
    if (msg.isSet(orderQty))
    {
        msg.get(orderQty);
        const std::optional<QuantityT> qty = toQuantity(orderQty.getValue());
        if (!qty)
        {
            refuse("Replace refused: " + quantityRefusal());
            return;
        }
        replacement->orderQty_ = *qty;
    }

    // A GTD replacement expires at its ExpireTime (126), the original's or a new one ahead of it, and only a GTD order
    // has one (#240), as for replace_order. The time in force is the request's, if it gives one.
    TimeInForce tifAfter = replacement->tif_;
    {
        FIX::TimeInForce requested;
        if (msg.isSet(requested))
        {
            msg.get(requested);
            tifAfter = toTif(requested.getValue());
        }
    }
    DateTimeT expireTimeVal = replacement->expireTime_;
    std::optional<std::string> expiryProblem;
    if ((GTD_TIF == tifAfter) && !msg.isSetField(FIX::FIELD::ExpireTime))
    {
        if (GTD_TIF != replacement->tif_)
        {
            expiryProblem = "a GTD order needs ExpireTime (126)"; // the original had none to keep
        }
    }
    else
    {
        expiryProblem = expiryRefusal(msg, tifAfter, aux::currentDateTime(), &expireTimeVal);
    }
    if (expiryProblem)
    {
        refuse("Replace refused: " + *expiryProblem);
        return;
    }
    replacement->expireTime_ = (GTD_TIF == tifAfter) ? expireTimeVal : 0;

    replacement->orderId_ = IdT();
    replacement->origClOrderId_ = replacement->clOrderId_;
    const std::string clOrdStr = sessionClOrdId(sourceStr, clOrdId.getString());
    // In the replacement's own record, as WsSession's replace does (#192)
    replacement->clOrderId_ = WideDataStorage::instance()->addUnsaved(
        new RawDataEntry(STRING_RAWDATATYPE, clOrdStr.c_str(), static_cast<u32>(clOrdStr.size())));

    FIX::TimeInForce tif;
    if (msg.isSet(tif))
    {
        msg.get(tif);
        replacement->tif_ = toTif(tif.getValue());
    }
    replacement->status_ = RECEIVEDNEW_ORDSTATUS;
    replacement->lastUpdateTime_ = static_cast<DateTimeT>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    // The queue owns the replacement once pushed (#74)
    Queues::OrderReplaceEvent evt(existing->orderId_, replacement.release());
    inQueues_->push(sourceStr, evt);
}

// =============================================================================
// Outbound
// =============================================================================

bool FixGateway::readSettlDate(const FIX::FieldMap &fields, const FIX::Message &msg, const FIX::SessionID &sid, int tag,
                               const char *name, DateTimeT *date)
{
    if (!fields.isSetField(tag))
    {
        return true;
    }
    const std::string &text = fields.getField(tag);
    const std::optional<DateTimeT> parsed = fromLocalMktDate(text);
    if (!parsed)
    {
        // The refusal quotes what came, cut short: it is the client's own text
        refuseOrder(msg, sid, ORD_REJ_REASON_BROKER_OPTION,
                    "Order refused: " + std::string(name) + " must be a date, YYYYMMDD, not '" + text.substr(0, 32) +
                        "'");
        return false;
    }
    *date = *parsed;
    return true;
}

bool FixGateway::applySessionAccounts(const FIX::Message &msg, const FIX::SessionID &sid, std::string *account)
{
    if (nullptr == credentials_)
    {
        return true;
    }
    const FixCounterparty *counterparty = credentials_->find(sid.getTargetCompID().getString());
    if (nullptr == counterparty)
    {
        // Such a session can't log on; refuse rather than trade the server's default account
        refuseOrder(msg, sid, FIX::OrdRejReason_UNKNOWN_ACCOUNT, "Order refused: this session may trade no account");
        return false;
    }
    if (account->empty())
    {
        *account = counterparty->accounts.front();
        return true;
    }
    if (!FixCredentials::mayTrade(*counterparty, *account))
    {
        refuseOrder(msg, sid, FIX::OrdRejReason_UNKNOWN_ACCOUNT,
                    "Order refused: this session may not trade the account " + *account);
        return false;
    }
    return true;
}

bool FixGateway::hasFixSession(const std::string &source) const
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
    return sessionMap_.find(source) != sessionMap_.end();
}

bool FixGateway::reportsOverFix(const ExecutionEntry *exec)
{
    return (REPLACE_EXECTYPE != exec->type_) || (REPLACED_ORDSTATUS != exec->orderStatus_);
}

void FixGateway::sendExecutionReport(const ExecutionEntry *exec, const OrderEntry &order,
                                     const std::string &requestClOrdId)
{
    if (!reportsOverFix(exec))
    {
        return; // the replacement's report acknowledges the replace (#74)
    }

    std::string source = order.source_.get();

    FIX::SessionID sid;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
        auto it = sessionMap_.find(source);
        if (it == sessionMap_.end())
        {
            return; // not a FIX-sourced order
        }
        sid = it->second;
    }

    FIX50SP2::ExecutionReport report = buildExecutionReport(exec, order, requestClOrdId);
    FIX::Session::sendToTarget(report, sid);
}

FIX50SP2::ExecutionReport FixGateway::buildExecutionReport(const ExecutionEntry *exec, const OrderEntry &order,
                                                           const std::string &requestClOrdId)
{
    FIX50SP2::ExecutionReport report(
        FIX::OrderID(std::to_string(order.orderId_.id_)), FIX::ExecID(std::to_string(exec->execId_.id_)),
        FIX::ExecType(fromExecType(exec->type_)), FIX::OrdStatus(fromOrdStatus(exec->orderStatus_)),
        FIX::Side(fromSide(order.side_)), FIX::LeavesQty(order.leavesQty_), FIX::CumQty(order.cumQty_));
    // Optional since FIX 5.0, so no longer the constructor's, but every report of the Order State Changes has it
    report.set(FIX::AvgPx(order.avgPx_));

    // The client's own ClOrdIDs, without the session's key they are stored under (#71)
    const std::string source = sourceOf(order);
    const std::string clOrd = clientClOrdId(rawText(order.clOrderId_.get()), source);
    if ((CANCEL_EXECTYPE == exec->type_) && !requestClOrdId.empty())
    {
        // A cancel's acknowledgement names the request it answers, and the order it cancelled (#98)
        report.set(FIX::ClOrdID(requestClOrdId));
        if (!clOrd.empty())
        {
            report.set(FIX::OrigClOrdID(clOrd));
        }
    }
    else if (!clOrd.empty())
    {
        report.set(FIX::ClOrdID(clOrd));
    }
    // A replace is acknowledged with the replacement's report, which names the order it replaced in OrigClOrdID (41)
    if ((REPLACE_EXECTYPE == exec->type_) && (SourceIdT() != order.origClOrderId_.getId()))
    {
        const std::string origClOrd = clientClOrdId(rawText(order.origClOrderId_.get()), source);
        if (!origClOrd.empty())
        {
            report.set(FIX::OrigClOrdID(origClOrd));
        }
    }

    report.set(FIX::Symbol(order.instrument_.get().symbol_));
    report.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    // A restatement at a business day's start (#244): FIX's GT renewal / restatement, with no corporate action
    if (RESTATED_EXECTYPE == exec->type_)
    {
        report.set(FIX::ExecRestatementReason(FIX::ExecRestatementReason_GT_RENEWAL));
    }
    // A GT order's figures for the business day, once it has traded on an earlier one, as FIX defines them (#244)
    if ((DAY_TIF != order.tif_) && (IOC_TIF != order.tif_) && (FOK_TIF != order.tif_) &&
        (order.dayCumQty_ < order.cumQty_))
    {
        report.set(FIX::DayOrderQty(order.dayOrderQty_));
        report.set(FIX::DayCumQty(order.dayCumQty_));
        report.set(FIX::DayAvgPx(order.dayAvgPx_));
    }
    // A GTD order's expiry (#240)
    if ((GTD_TIF == order.tif_) && (0 != order.expireTime_))
    {
        report.set(FIX::ExpireTime(toUtcTimeStamp(order.expireTime_), 3));
    }

    // Extra fields follow the report's class, as in the JSON serializer; reports are full copies since #56
    if (auto *trade = dynamic_cast<const TradeExecEntry *>(exec))
    {
        report.set(FIX::LastQty(trade->lastQty_));
        report.set(FIX::LastPx(trade->lastPx_));
        // TradeDate (75), a LocalMktDate: the business day the trade counts in, which the engine keeps as YYYYMMDD
        // (#258). A trade in no day has none.
        if (0 != trade->tradeDate_)
        {
            report.set(FIX::TradeDate(std::to_string(trade->tradeDate_)));
        }
    }
    // A reject says why: OrdRejReason (103) and the engine's reason in Text (58)
    if (auto *reject = dynamic_cast<const RejectExecEntry *>(exec))
    {
        if (REJECT_EXECTYPE == exec->type_)
        {
            report.set(FIX::OrdRejReason(ORD_REJ_REASON_BROKER_OPTION));
        }
        if (!reject->rejectReason_.empty())
        {
            report.set(FIX::Text(reject->rejectReason_));
        }
    }

    // FX Swap leg: populate NoLegs group with leg-specific fields, set
    // TradeLinkID so clients can correlate near+far reports for the same swap.
    if (exec->execLegType_ != SINGLE_LEG)
    {
        FIX50SP2::ExecutionReport::NoLegs legGroup;

        legGroup.set(FIX::LegSymbol(order.instrument_.get().symbol_));

        // Near leg trades at root side; far leg trades at opposite side
        char legFixSide;
        DateTimeT legSettl;
        if (exec->execLegType_ == NEAR_LEG)
        {
            legFixSide = fromSide(order.side_);
            legSettl = order.settlDate_;
        }
        else
        { // FAR_LEG
            legFixSide = (order.side_ == BUY_SIDE) ? FIX::Side_SELL : FIX::Side_BUY;
            legSettl = order.farSettlDate_;
        }
        legGroup.set(FIX::LegSide(legFixSide));

        if (auto *trade = dynamic_cast<const TradeExecEntry *>(exec))
        {
            legGroup.set(FIX::LegLastPx(trade->lastPx_));
            // LegLastQty (1418), in FIX 5.0 SP2's leg group: FIX 4.4 had no such field, so a client checking a report
            // against its dictionary refused it there
            legGroup.set(FIX::LegLastQty(trade->lastQty_));
        }

        // A LocalMktDate, YYYYMMDD: it used to be the engine's milliseconds, which no FIX client could read (#198)
        if (legSettl > 0)
        {
            legGroup.set(FIX::LegSettlDate(toLocalMktDate(legSettl)));
        }

        report.addGroup(legGroup);

        // TradeLinkID (820): the same on a side's near and far reports, so a client pairs them (#199). It was
        // "<orderId>-<transactTime>", and each report stamps its own time. FIX 5.0 SP2's ExecutionReport doesn't have
        // it, as 4.4's didn't, so a client checking a swap's report against its dictionary refuses it (#378).
        report.setField(820, std::to_string(exec->tradeLink_));
    }

    return report;
}

void FixGateway::sendOrderReject(const Queues::OrderRejectEvent &evnt, const std::string &source)
{
    FIX::SessionID sid;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
        auto it = sessionMap_.find(source);
        if (it == sessionMap_.end())
        {
            return; // not sent through a FIX session
        }
        sid = it->second;
    }

    try
    {
        // The engine's event names the ClOrdIDs as they are stored; the client gets its own (#71)
        const Queues::OrderRejectEvent shown = clientView(evnt, source);
        if (evnt.replacement_)
        {
            // The reject describes the order it was to replace: its OrderID, and its status as the engine saw it when
            // it refused (#74), or as it is now; Rejected when there is no such order
            std::string origOrderId = "NONE";
            char origStatus = FIX::OrdStatus_REJECTED;
            RawDataEntry rawKey(STRING_RAWDATATYPE, evnt.origClOrderId_.c_str(),
                                static_cast<u32>(evnt.origClOrderId_.size()));
            if (OrderEntry *orig = orderStorage_->locateByClOrderId(rawKey))
            {
                oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(orig->entryMutex_, false);
                origOrderId = std::to_string(orig->orderId_.id_);
                origStatus = fromOrdStatus(orig->status_);
            }
            if (INVALID_ORDSTATUS != evnt.origStatus_)
            {
                origStatus = fromOrdStatus(evnt.origStatus_);
            }
            FIX50SP2::OrderCancelReject reject = buildReplaceReject(shown, origOrderId, origStatus);
            FIX::Session::sendToTarget(reject, sid);
        }
        else
        {
            // The order was never stored, so its reject has no execution of its own. Its ExecID still comes from the
            // shared id generator, whose saved limit keeps it from repeating after a restart, as execution ids don't
            // (#81); a counter of the gateway's own started again at R1 in every run.
            FIX50SP2::ExecutionReport report = buildOrderReject(shown, IdTGenerator::instance()->getId().id_);
            FIX::Session::sendToTarget(report, sid);
        }
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the order reject: ") + ex.what());
    }
}

FIX50SP2::ExecutionReport FixGateway::buildOrderReject(const Queues::OrderRejectEvent &evnt, u64 execId)
{
    // The order was never stored, so it has no OrderID; its reject still needs an ExecID of its own
    FIX50SP2::ExecutionReport report(FIX::OrderID("NONE"), FIX::ExecID("R" + std::to_string(execId)),
                                     FIX::ExecType(FIX::ExecType_REJECTED), FIX::OrdStatus(FIX::OrdStatus_REJECTED),
                                     FIX::Side(fromSide(evnt.side_)), FIX::LeavesQty(0), FIX::CumQty(0));
    report.set(FIX::AvgPx(0)); // as buildExecutionReport() sends it
    if (!evnt.clOrderId_.empty())
    {
        report.set(FIX::ClOrdID(evnt.clOrderId_));
    }
    if (!evnt.symbol_.empty())
    {
        report.set(FIX::Symbol(evnt.symbol_));
    }
    report.set(FIX::OrderQty(evnt.orderQty_));
    report.set(
        FIX::OrdRejReason(evnt.duplicateClOrderId_ ? FIX::OrdRejReason_DUPLICATE_ORDER : ORD_REJ_REASON_BROKER_OPTION));
    report.set(FIX::Text(evnt.reason_));
    report.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    return report;
}

FIX50SP2::ExecutionReport FixGateway::buildOrderRefusal(const FIX::Message &order, int ordRejReason,
                                                        const std::string &text, u64 execId)
{
    Queues::OrderRejectEvent refusal;
    FIX::ClOrdID clOrdId;
    if (order.getFieldIfSet(clOrdId))
    {
        refusal.clOrderId_ = clOrdId.getValue();
    }
    FIX::Symbol symbol;
    if (order.getFieldIfSet(symbol))
    {
        refusal.symbol_ = symbol.getValue();
    }
    refusal.reason_ = text;
    FIX50SP2::ExecutionReport report = buildOrderReject(refusal, execId);
    report.set(FIX::OrdRejReason(ordRejReason));
    // Side and OrderQty as they were sent: the engine's own types may not hold them
    FIX::Side side;
    if (order.getFieldIfSet(side))
    {
        report.set(side);
    }
    if (order.isSetField(FIX::FIELD::OrderQty))
    {
        report.setField(FIX::FIELD::OrderQty, order.getField(FIX::FIELD::OrderQty));
    }
    return report;
}

FIX50SP2::OrderCancelReject FixGateway::buildReplaceReject(const Queues::OrderRejectEvent &evnt,
                                                           const std::string &origOrderId, char origStatus)
{
    FIX50SP2::OrderCancelReject reject(FIX::OrderID(origOrderId), FIX::ClOrdID(evnt.clOrderId_),
                                       FIX::OrdStatus(origStatus),
                                       FIX::CxlRejResponseTo(FIX::CxlRejResponseTo_ORDER_CANCEL_REPLACE_REQUEST));
    // Optional since FIX 5.0, and still sent: it names the order the request was for
    reject.set(FIX::OrigClOrdID(evnt.origClOrderId_));
    reject.set(FIX::CxlRejReason(evnt.duplicateClOrderId_ ? FIX::CxlRejReason_DUPLICATE_CL_ORD_ID
                                                          : cxlRejReasonFor(evnt.refusal_)));
    reject.set(FIX::Text(evnt.reason_));
    return reject;
}

FIX50SP2::OrderCancelReject FixGateway::buildUnknownReplaceReject(const std::string &clOrdId,
                                                                  const std::string &origClOrdId)
{
    Queues::OrderRejectEvent unknown;
    unknown.replacement_ = true;
    unknown.clOrderId_ = clOrdId;
    unknown.origClOrderId_ = origClOrdId;
    unknown.refusal_ = Queues::CancelRejectEvent::UNKNOWN_ORDER;
    unknown.reason_ = "Replace refused: unknown order";
    return buildReplaceReject(unknown, "NONE", FIX::OrdStatus_REJECTED);
}

FIX50SP2::OrderCancelReject FixGateway::buildCancelReject(const Queues::CancelRejectEvent &evnt,
                                                          const std::string &orderId, const std::string &clOrdId,
                                                          const std::string &origClOrdId)
{
    const char status =
        (INVALID_ORDSTATUS == evnt.ordStatus_) ? FIX::OrdStatus_REJECTED : fromOrdStatus(evnt.ordStatus_);
    FIX50SP2::OrderCancelReject reject(FIX::OrderID(orderId), FIX::ClOrdID(clOrdId), FIX::OrdStatus(status),
                                       FIX::CxlRejResponseTo(FIX::CxlRejResponseTo_ORDER_CANCEL_REQUEST));
    reject.set(FIX::OrigClOrdID(origClOrdId)); // optional since FIX 5.0, as for a replace
    reject.set(FIX::CxlRejReason(cxlRejReasonFor(evnt.reason_)));
    reject.set(FIX::Text(cancelRejectText(evnt)));
    return reject;
}

FIX50SP2::OrderCancelReject FixGateway::buildEngineCancelReject(const Queues::CancelRejectEvent &evnt,
                                                                const std::string &orderId,
                                                                const std::string &orderClOrdId)
{
    const std::string &clOrdId = evnt.requestClOrdId_.empty() ? orderClOrdId : evnt.requestClOrdId_;
    return buildCancelReject(evnt, orderId, clOrdId, orderClOrdId);
}

void FixGateway::sendCancelReject(const Queues::CancelRejectEvent &evnt, const std::string &target)
{
    // To the session that sent the cancel, which may not be the one that placed the order
    FIX::SessionID sid;
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
        auto it = sessionMap_.find(target);
        if (it == sessionMap_.end())
        {
            return; // not sent through a FIX session
        }
        sid = it->second;
    }

    std::string orderId = "NONE";
    std::string clOrdId = "NONE";
    if (OrderEntry *order = orderStorage_->locateByOrderId(evnt.id_))
    {
        oneapi::tbb::spin_rw_mutex::scoped_lock ordLock(order->entryMutex_, false);
        orderId = std::to_string(order->orderId_.id_);
        // the client's own ClOrdID, without the session's key it is stored under (#71)
        const std::string client = clientClOrdId(rawText(order->clOrderId_.get()), sourceOf(*order));
        if (!client.empty())
        {
            clOrdId = client;
        }
    }
    try
    {
        FIX50SP2::OrderCancelReject reject = buildEngineCancelReject(evnt, orderId, clOrdId);
        FIX::Session::sendToTarget(reject, sid);
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the cancel reject: ") + ex.what());
    }
}

void FixGateway::sendBusinessReject(const IdT &refOrderId, const std::string &reason)
{
    // To the session of the order it names, and to nobody else. One that named no order went to every session logged on,
    // so each got a refusal of an order it never sent (#189). The gateway answers its own refusals with refuseOrder().
    if (!refOrderId.isValid())
    {
        aux::ExchLogger::instance()->warn("FixGateway: a business reject that names no order is not sent: " + reason);
        return;
    }
    const OrderEntry *order = orderStorage_->locateByOrderId(refOrderId);
    if (nullptr == order)
    {
        return;
    }
    FIX::SessionID target;
    {
        const std::string source = order->source_.get();
        oneapi::tbb::spin_rw_mutex::scoped_lock lock(sessionMapLock_, false);
        const auto it = sessionMap_.find(source);
        if (sessionMap_.end() == it)
        {
            return; // not a FIX session's order
        }
        target = it->second;
    }
    FIX50SP2::BusinessMessageReject reject;
    reject.set(FIX::RefMsgType("D")); // NewOrderSingle/NewOrderMultileg
    reject.set(FIX::BusinessRejectReason(FIX::BusinessRejectReason_OTHER));
    reject.set(FIX::Text(reason));
    reject.set(FIX::BusinessRejectRefID(std::to_string(refOrderId.id_)));
    FIX::Session::sendToTarget(reject, target);
}

// =============================================================================
// Enum conversions
// =============================================================================

Side FixGateway::toSide(char fixSide)
{
    switch (fixSide)
    {
    case FIX::Side_BUY:
        return BUY_SIDE;
    case FIX::Side_SELL:
        return SELL_SIDE;
    case FIX::Side_SELL_SHORT:
        return SELL_SHORT_SIDE;
    case '6':
        return CROSS_SIDE; // FIX Cross
    default:
        return INVALID_SIDE;
    }
}

OrderType FixGateway::toOrdType(char fixOrdType)
{
    switch (fixOrdType)
    {
    case FIX::OrdType_MARKET:
        return MARKET_ORDERTYPE;
    case FIX::OrdType_LIMIT:
        return LIMIT_ORDERTYPE;
    case FIX::OrdType_STOP:
        return STOP_ORDERTYPE;
    case FIX::OrdType_STOP_LIMIT:
        return STOPLIMIT_ORDERTYPE;
    case FIX::OrdType_FOREX_SWAP:
        return FXSWAP_ORDERTYPE;
    default:
        return INVALID_ORDERTYPE;
    }
}

TimeInForce FixGateway::toTif(char fixTif)
{
    switch (fixTif)
    {
    case FIX::TimeInForce_DAY:
        return DAY_TIF;
    case FIX::TimeInForce_GOOD_TILL_CANCEL:
        return GTC_TIF;
    case FIX::TimeInForce_IMMEDIATE_OR_CANCEL:
        return IOC_TIF;
    case FIX::TimeInForce_FILL_OR_KILL:
        return FOK_TIF;
    case FIX::TimeInForce_GOOD_TILL_DATE:
        return GTD_TIF;
    case FIX::TimeInForce_AT_THE_OPENING:
        return OPG_TIF;
    case FIX::TimeInForce_AT_THE_CLOSE:
        return ATCLOSE_TIF;
    default:
        return INVALID_TIF;
    }
}

Currency FixGateway::toCurrency(const std::string &fixCcy)
{
    if (fixCcy == "USD")
    {
        return USD_CURRENCY;
    }
    if (fixCcy == "EUR")
    {
        return EUR_CURRENCY;
    }
    if (fixCcy == "GBP")
    {
        return GBP_CURRENCY;
    }
    if (fixCcy == "JPY")
    {
        return JPY_CURRENCY;
    }
    if (fixCcy == "CHF")
    {
        return CHF_CURRENCY;
    }
    if (fixCcy == "AUD")
    {
        return AUD_CURRENCY;
    }
    if (fixCcy == "CAD")
    {
        return CAD_CURRENCY;
    }
    if (fixCcy == "NZD")
    {
        return NZD_CURRENCY;
    }
    return INVALID_CURRENCY;
}

std::optional<QuantityT> FixGateway::toQuantity(double orderQty)
{
    // Checked before the cast, which is undefined behaviour for a value outside QuantityT's range
    if (std::isfinite(orderQty) && (1.0 <= orderQty) &&
        (orderQty <= static_cast<double>(std::numeric_limits<QuantityT>::max())) && (std::trunc(orderQty) == orderQty))
    {
        return static_cast<QuantityT>(orderQty);
    }
    return std::nullopt;
}

std::optional<DateTimeT> FixGateway::fromLocalMktDate(std::string_view text)
{
    if ((8 != text.size()) || !std::all_of(text.begin(), text.end(),
                                           [](char c)
                                           {
                                               return (c >= '0') && (c <= '9');
                                           }))
    {
        return std::nullopt;
    }
    const auto number = [text](std::size_t at, std::size_t digits)
    {
        unsigned value = 0;
        for (std::size_t i = at; i < at + digits; ++i)
        {
            value = 10 * value + static_cast<unsigned>(text[i] - '0');
        }
        return value;
    };
    const std::chrono::year_month_day day{ std::chrono::year(static_cast<int>(number(0, 4))),
                                           std::chrono::month(number(4, 2)), std::chrono::day(number(6, 2)) };
    if (!day.ok() || (day.year() < std::chrono::year(1970)))
    {
        return std::nullopt;
    }
    const auto sinceEpoch = std::chrono::sys_days(day).time_since_epoch();
    return static_cast<DateTimeT>(std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count());
}

std::optional<std::string> FixGateway::expiryRefusal(const FIX::FieldMap &msg, TimeInForce tif, DateTimeT now,
                                                     DateTimeT *expireTime)
{
    assert(nullptr != expireTime);
    if (!msg.isSetField(FIX::FIELD::ExpireTime))
    {
        if (GTD_TIF != tif)
        {
            return std::nullopt;
        }
        return std::string(msg.isSetField(FIX::FIELD::ExpireDate)
                               ? "a GTD order needs ExpireTime (126); the server doesn't read ExpireDate (432)"
                               : "a GTD order needs ExpireTime (126)");
    }
    if (GTD_TIF != tif)
    {
        return std::string("ExpireTime (126) is only for a GTD order");
    }
    DateTimeT when = 0;
    try
    {
        const FIX::UtcTimeStamp value = FIX::UtcTimeStampConvertor::convert(msg.getField(FIX::FIELD::ExpireTime));
        const time_t seconds = value.getTimeT();
        when = (0 <= seconds)
                   ? (static_cast<DateTimeT>(seconds) * 1000 + static_cast<DateTimeT>(value.getMillisecond()))
                   : 0;
    }
    catch (const FIX::FieldConvertError &)
    {
        return std::string("ExpireTime (126) must be a UTCTimestamp, YYYYMMDD-HH:MM:SS");
    }
    if (when <= now)
    {
        return std::string("ExpireTime (126) has passed");
    }
    *expireTime = when;
    return std::nullopt;
}

FIX::UtcTimeStamp FixGateway::toUtcTimeStamp(DateTimeT when)
{
    return FIX::UtcTimeStamp(static_cast<time_t>(when / 1000), static_cast<int>(when % 1000));
}

std::string FixGateway::toLocalMktDate(DateTimeT date)
{
    // The last day a LocalMktDate can name, 9999-12-31, bounds a date no FIX order could have given
    constexpr DateTimeT LAST_DAY_MS = 253402214400000ULL;
    const std::chrono::sys_time<std::chrono::milliseconds> when{ std::chrono::milliseconds(
        static_cast<std::int64_t>(std::min(date, LAST_DAY_MS))) };
    const std::chrono::year_month_day day{ std::chrono::floor<std::chrono::days>(when) };
    char text[16];
    std::snprintf(text, sizeof(text), "%04d%02u%02u", static_cast<int>(day.year()), static_cast<unsigned>(day.month()),
                  static_cast<unsigned>(day.day()));
    return text;
}

bool FixGateway::isTradablePrice(double price, OrderType ordType)
{
    return std::isfinite(price) && ((MARKET_ORDERTYPE == ordType) || (0.0 < price));
}

char FixGateway::fromOrdStatus(OrderStatus s)
{
    switch (s)
    {
    case NEW_ORDSTATUS:
        return FIX::OrdStatus_NEW;
    case PARTFILL_ORDSTATUS:
        return FIX::OrdStatus_PARTIALLY_FILLED;
    case FILLED_ORDSTATUS:
        return FIX::OrdStatus_FILLED;
    case CANCELED_ORDSTATUS:
        return FIX::OrdStatus_CANCELED;
    case REJECTED_ORDSTATUS:
        return FIX::OrdStatus_REJECTED;
    case REPLACED_ORDSTATUS:
        // 39=5, Replaced, has been out of use since FIX 4.4, though QuickFIX's FIX 5.0 SP2 dictionary lists it: a
        // replace is told by ExecType 5 alone. A replaced order is closed, its quantity moved to the replacement, which
        // has its own acknowledgement (#74); a cancel or replace that names it later gets 102=0 and this.
        return FIX::OrdStatus_CANCELED;
    case PENDINGNEW_ORDSTATUS:
        return FIX::OrdStatus_PENDING_NEW;
    case PENDINGREPLACE_ORDSTATUS:
        return FIX::OrdStatus_PENDING_REPLACE;
    case EXPIRED_ORDSTATUS:
        return FIX::OrdStatus_EXPIRED;
    case SUSPENDED_ORDSTATUS:
        return FIX::OrdStatus_SUSPENDED;
    case DFD_ORDSTATUS:
        return FIX::OrdStatus_DONE_FOR_DAY;
    default:
        return FIX::OrdStatus_NEW;
    }
}

char FixGateway::fromExecType(ExecType t)
{
    switch (t)
    {
    case NEW_EXECTYPE:
        return FIX::ExecType_NEW;
    case TRADE_EXECTYPE:
        return FIX::ExecType_TRADE;
    case CANCEL_EXECTYPE:
        return FIX::ExecType_CANCELED;
    case REJECT_EXECTYPE:
        return FIX::ExecType_REJECTED;
    case CORRECT_EXECTYPE:
        return FIX::ExecType_TRADE_CORRECT;
    case REPLACE_EXECTYPE:
        return FIX::ExecType_REPLACED;
    case EXPIRED_EXECTYPE:
        return FIX::ExecType_EXPIRED;
    case SUSPENDED_EXECTYPE:
        return FIX::ExecType_SUSPENDED;
    case PEND_CANCEL_EXECTYPE:
        return FIX::ExecType_PENDING_CANCEL;
    case PEND_REPLACE_EXECTYPE:
        return FIX::ExecType_PENDING_REPLACE;
    case DFD_EXECTYPE:
        return FIX::ExecType_DONE_FOR_DAY;
    case STATUS_EXECTYPE:
        return FIX::ExecType_ORDER_STATUS;
    case RESTATED_EXECTYPE:
        return FIX::ExecType_RESTATED;
    case TRIGGERED_EXECTYPE: // a held stop released by a trade at its stop (#223, #236)
        return FIX::ExecType_TRIGGERED_OR_ACTIVATED_BY_SYSTEM;
    default:
        return FIX::ExecType_NEW;
    }
}

char FixGateway::fromSide(Side s)
{
    switch (s)
    {
    case BUY_SIDE:
        return FIX::Side_BUY;
    case SELL_SIDE:
        return FIX::Side_SELL;
    case SELL_SHORT_SIDE:
        return FIX::Side_SELL_SHORT;
    case CROSS_SIDE:
        return FIX::Side_CROSS;
    default:
        return FIX::Side_BUY;
    }
}

#endif // BUILD_FIX
