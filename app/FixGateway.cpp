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
#include <charconv>
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
/// Whether a report can be its order's first (#380): a new order's acknowledgement or its rejection, or a replacement's
/// report. The engine stores an order before it sends one of these, and sends every other report after one.
bool canBeFirstReport(ExecType type)
{
    return (NEW_EXECTYPE == type) || (REJECT_EXECTYPE == type) || (REPLACE_EXECTYPE == type);
}
/// The session string an order came from, which prefixes the keys of its ClOrdIDs (#71); empty when it names none
std::string sourceOf(const OrderEntry &order)
{
    return (SourceIdT() != order.source_.getId()) ? order.source_.get() : std::string();
}
/// How every FIX session's source string begins (FixGateway::makeSourceString()), so how a FIX order's source does
constexpr std::string_view FIX_SOURCE_PREFIX{ "FIX:" };
/// The order number an OrderID (37) names, as the gateway sends it (#58); nothing for anything else
std::optional<u64> toOrderNumber(const std::string &text)
{
    u64 number = 0;
    const char *end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, number);
    return ((std::errc() == error) && (end == at)) ? std::optional<u64>(number) : std::nullopt;
}
/// What a status answer reports of a stored order (#383): the fields a transaction worker changes, read together under
/// the order's lock. Its ClOrdIDs and instrument never change once it is stored.
struct OrderSeen
{
    u64 number = 0; // its OrderID (37)
    OrderStatus status = INVALID_ORDSTATUS;
    Side side = INVALID_SIDE;
    TimeInForce tif = INVALID_TIF;
    QuantityT orderQty = 0;
    QuantityT cumQty = 0;
    QuantityT leavesQty = 0;
    QuantityT dayOrderQty = 0;
    QuantityT dayCumQty = 0;
    PriceT avgPx = 0;
    PriceT dayAvgPx = 0;
    DateTimeT expireTime = 0;
};
OrderSeen seenOf(const OrderEntry &order)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(order.entryMutex_, false);
    return OrderSeen{ order.orderId_.id_, order.status_, order.side_,      order.tif_,
                      order.orderQty_,    order.cumQty_, order.leavesQty_, order.dayOrderQty_,
                      order.dayCumQty_,   order.avgPx_,  order.dayAvgPx_,  order.expireTime_ };
}
/// What an order had left to trade when a replace of it is pending (#383): a replace zeroes the original's LeavesQty
/// once the engine decides it, before the replacement is acknowledged
QuantityT leavesWhileReplacing(const OrderSeen &order)
{
    return (REPLACED_ORDSTATUS == order.status) ? order.orderQty - order.cumQty : order.leavesQty;
}
/// An ExecutionReport of ExecType Order Status (150=I), answering a status request (#383): ExecID 0, as FIX gives
/// every one, and the quantities, as any other report of the gateway's has them
FIX50SP2::ExecutionReport orderStatusReport(const std::string &orderId, char ordStatus, char side, QuantityT orderQty,
                                            QuantityT cumQty, QuantityT leavesQty, PriceT avgPx)
{
    FIX50SP2::ExecutionReport report(FIX::OrderID(orderId), FIX::ExecID("0"), FIX::ExecType(FIX::ExecType_ORDER_STATUS),
                                     FIX::OrdStatus(ordStatus), FIX::Side(side), FIX::LeavesQty(leavesQty),
                                     FIX::CumQty(cumQty));
    report.set(FIX::AvgPx(avgPx));
    report.setField(FIX::FIELD::OrderQty, std::to_string(orderQty));
    report.set(FIX::TransactTime(FIX::UtcTimeStamp::now()));
    return report;
}
/// The status answer for a stored order, as seen, in ordStatus with leavesQty, under the ClOrdIDs given (#383). The
/// order's fields as buildExecutionReport() writes them, and no leg group: a swap's status is its order's.
FIX50SP2::ExecutionReport storedOrderStatusReport(const OrderEntry &order, const OrderSeen &seen, char ordStatus,
                                                  QuantityT leavesQty, const std::string &clOrdId,
                                                  const std::string &origClOrdId)
{
    FIX50SP2::ExecutionReport report =
        orderStatusReport(std::to_string(seen.number), ordStatus, FixGateway::fromSide(seen.side), seen.orderQty,
                          seen.cumQty, leavesQty, seen.avgPx);
    if (!clOrdId.empty())
    {
        report.set(FIX::ClOrdID(clOrdId));
    }
    if (!origClOrdId.empty())
    {
        report.set(FIX::OrigClOrdID(origClOrdId));
    }
    report.set(FIX::Symbol(order.instrument_.get().symbol_));
    // A GT order's figures for the business day, once it has traded on an earlier one (#244), and a GTD order's expiry
    // (#240)
    if ((DAY_TIF != seen.tif) && (IOC_TIF != seen.tif) && (FOK_TIF != seen.tif) && (seen.dayCumQty < seen.cumQty))
    {
        report.set(FIX::DayOrderQty(seen.dayOrderQty));
        report.set(FIX::DayCumQty(seen.dayCumQty));
        report.set(FIX::DayAvgPx(seen.dayAvgPx));
    }
    if ((GTD_TIF == seen.tif) && (0 != seen.expireTime))
    {
        report.set(FIX::ExpireTime(FixGateway::toUtcTimeStamp(seen.expireTime), 3));
    }
    return report;
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
                       SourceIdT defaultClearingId, SourceIdT defaultAccountId, std::chrono::milliseconds answerWait)
    : inQueues_(inQueues), wideData_(wideData), orderStorage_(orderStorage), defaultClearingId_(defaultClearingId),
      defaultAccountId_(defaultAccountId), answerWait_(answerWait)
{
    // The replacements stored, which the server restored before it made the gateway: a status request that names an
    // order replaced before this run is answered with its replacement, as for one replaced since (#383)
    orderStorage_->forEachOrder(
        [this](const IdT &, const OrderEntry &order)
        {
            learnReplacement(order);
        });
}

std::string FixGateway::makeSourceString(const FIX::SessionID &sid)
{
    return std::string(FIX_SOURCE_PREFIX) + sid.getSenderCompID().getString() + "->" +
           sid.getTargetCompID().getString();
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

OrderEntry *FixGateway::locateReceivedOrder(const std::string &source, const std::string &clOrdId,
                                            std::optional<Unanswered> *pending) const
{
    // The order may be on its way: the session's thread hands an order to the engine, and an event worker stores it
    // later (#380). The engine stores an order before it reports it, and the gateway keeps its key from before it hands
    // the order over until the engine answers: so a key the gateway doesn't keep is of an order never sent, or one
    // stored. The order is looked for again after the key is read: answered since the first look, or stored and not yet
    // reported, it is found then. Each look at the store is made without answersLock_, which the engine's threads take
    // while they report.
    OrderEntry *order = locateSessionOrder(source, clOrdId);
    *pending = unansweredOf(sessionClOrdId(source, clOrdId));
    return (nullptr != order) ? order : locateSessionOrder(source, clOrdId);
}

OrderEntry *FixGateway::locateAnsweredOrder(const std::string &source, const std::string &clOrdId, bool *unanswered)
{
    *unanswered = false;
    if (OrderEntry *order = locateSessionOrder(source, clOrdId)) [[likely]]
    {
        return order;
    }
    // A request sent right after its order could find nothing and was answered as unknown, though the order was then
    // acknowledged and rested in the book (#380): one the engine hasn't answered is waited for
    std::optional<Unanswered> pending;
    OrderEntry *order = locateReceivedOrder(source, clOrdId, &pending);
    if ((nullptr != order) || !pending)
    {
        return order;
    }
    const std::string key = sessionClOrdId(source, clOrdId);
    {
        std::unique_lock<std::mutex> lock(answersLock_);
        ++waiting_;
        *unanswered = !answers_.wait_for(lock, answerWait_,
                                         [&]
                                         {
                                             return !unanswered_.contains(key);
                                         });
        --waiting_;
    }
    // Answered or not: stored and never reported, as when its booking's commit failed, it is found now
    return locateSessionOrder(source, clOrdId);
}

OrderEntry *FixGateway::locateSentOrder(const std::string &key, std::optional<Unanswered> *pending) const
{
    *pending = unansweredOf(key);
    RawDataEntry rawKey(STRING_RAWDATATYPE, key.c_str(), static_cast<u32>(key.size()));
    return orderStorage_->locateByClOrderId(rawKey);
}

template <typename Event>
void FixGateway::pushUnanswered(const std::string &source, const std::string &key, Unanswered record, const Event &evnt)
{
    // Before the push, so that the engine's answer, which comes after it, always finds the key: the queue's push and
    // pop order the insert before the report that reads unansweredCount_. The key is never there already: a new order
    // or a replace whose ClOrdID the session uses, stored or on its way, is answered before it gets here (#382, #403).
    {
        std::lock_guard<std::mutex> lock(answersLock_);
        unanswered_.try_emplace(key, std::move(record));
        unansweredCount_.store(unanswered_.size(), std::memory_order_release);
    }
    try
    {
        inQueues_->push(source, evnt);
    }
    catch (...)
    {
        answered(key); // the engine never got it, so nothing else would
        throw;
    }
}

void FixGateway::answered(std::string_view key)
{
    // Without the lock while nothing is unanswered: a report of an order the gateway sent always finds its key, since
    // the key went in before the order went to the engine (pushUnanswered())
    if (0 == unansweredCount_.load(std::memory_order_acquire))
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(answersLock_);
        const auto it = unanswered_.find(key);
        if (unanswered_.end() == it)
        {
            return;
        }
        unanswered_.erase(it);
        unansweredCount_.store(unanswered_.size(), std::memory_order_release);
    }
    answers_.notify_all();
}

std::optional<FixGateway::Unanswered> FixGateway::unansweredOf(std::string_view key) const
{
    // Without the lock while nothing is unanswered: a key the session's own thread put in, before it pushed the order,
    // is still there until the engine's answer takes it out, and the count says so
    if (0 == unansweredCount_.load(std::memory_order_acquire))
    {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(answersLock_);
    const auto it = unanswered_.find(key);
    return (unanswered_.end() == it) ? std::nullopt : std::optional<Unanswered>(it->second);
}

size_t FixGateway::waitingRequests() const
{
    std::lock_guard<std::mutex> lock(answersLock_);
    return waiting_;
}

void FixGateway::learnReplacement(const OrderEntry &replacement)
{
    // A replacement names the order it replaced by its stored ClOrdID (#74), and only a FIX order is ever asked for
    // by a FIX session. A WebSocket replace of a FIX order keeps the FIX source, so it is learned too.
    if ((SourceIdT() == replacement.origClOrderId_.getId()) || (SourceIdT() == replacement.clOrderId_.getId()) ||
        !sourceOf(replacement).starts_with(FIX_SOURCE_PREFIX))
    {
        return;
    }
    std::string replaced = rawText(replacement.origClOrderId_.get());
    std::string key = rawText(replacement.clOrderId_.get());
    if (replaced.empty() || key.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(replacementsLock_);
    replacements_.insert_or_assign(std::move(replaced), std::move(key));
}

OrderEntry *FixGateway::replacementOf(std::string_view key) const
{
    std::string replacementKey;
    {
        std::lock_guard<std::mutex> lock(replacementsLock_);
        const auto it = replacements_.find(key);
        if (replacements_.end() == it)
        {
            return nullptr;
        }
        replacementKey = it->second;
    }
    RawDataEntry rawKey(STRING_RAWDATATYPE, replacementKey.c_str(), static_cast<u32>(replacementKey.size()));
    return orderStorage_->locateByClOrderId(rawKey);
}

size_t FixGateway::knownReplacements() const
{
    std::lock_guard<std::mutex> lock(replacementsLock_);
    return replacements_.size();
}

void FixGateway::learnRefusal(const std::string &source, const std::string &clOrdId, const std::string &origClOrdId)
{
    if (clOrdId.empty() || origClOrdId.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(refusalsLock_);
    Refusals &refused = refusals_[source];
    // A ClOrdID refused again names the order its latest replace was for, and keeps its place among the oldest
    if (!refused.originals.insert_or_assign(clOrdId, origClOrdId).second)
    {
        return;
    }
    refused.oldestFirst.push_back(clOrdId);
    if (REFUSALS_KEPT < refused.oldestFirst.size())
    {
        refused.originals.erase(refused.oldestFirst.front());
        refused.oldestFirst.pop_front();
    }
}

void FixGateway::forgetRefusal(const std::string &source, const std::string &clOrdId)
{
    std::lock_guard<std::mutex> lock(refusalsLock_);
    const auto refused = refusals_.find(source);
    if ((refusals_.end() != refused) && (0 != refused->second.originals.erase(clOrdId)))
    {
        std::erase(refused->second.oldestFirst, clOrdId);
    }
}

OrderEntry *FixGateway::locateRefusedOriginal(const std::string &source, std::string *clOrdId) const
{
    std::string original;
    {
        std::lock_guard<std::mutex> lock(refusalsLock_);
        const auto refused = refusals_.find(source);
        if (refusals_.end() == refused)
        {
            return nullptr;
        }
        const auto it = refused->second.originals.find(*clOrdId);
        if (refused->second.originals.end() == it)
        {
            return nullptr;
        }
        original = it->second;
    }
    // One step reaches it: a replacement is decided against an order stored when it is, and one decided against a
    // refused replacement's original names that original itself
    OrderEntry *order = locateSessionOrder(source, original);
    if (nullptr != order)
    {
        aux::ExchLogger::instance()->note("FIX: " + *clOrdId + " is a refused replacement; decided against " +
                                          original + ", the order it was to replace (#398)");
        *clOrdId = std::move(original);
    }
    return order;
}

class FixGateway::AnsweredOnExit
{
public:
    AnsweredOnExit(FixGateway &gateway, std::string_view key) : gateway_(gateway), key_(key) {}
    ~AnsweredOnExit()
    {
        if (!key_.empty())
        {
            gateway_.answered(key_);
        }
    }
    AnsweredOnExit(const AnsweredOnExit &) = delete;
    AnsweredOnExit &operator=(const AnsweredOnExit &) = delete;

private:
    FixGateway &gateway_;
    std::string_view key_;
};

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
    // Its ClOrdID, if a refused replacement had it, names no order now (#398)
    FIX::ClOrdID clOrdId;
    if (msg.getFieldIfSet(clOrdId))
    {
        forgetRefusal(makeSourceString(sid), clOrdId.getValue());
    }
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
    else if (FIX::MsgType_OrderStatusRequest == type)
    {
        onMessage(FIX50SP2::OrderStatusRequest(msg), sid);
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

    // The ClOrdID is stored under the session's key, since FIX scopes it to the client that sent it (#71). One the
    // session has sent before is answered first, before anything else is checked (#382).
    const std::string sourceStr = makeSourceString(sid);
    const std::string clOrdStr = sessionClOrdId(sourceStr, clOrdId.getString());
    if (answerRepeatedOrder(msg, sid, sourceStr, clOrdStr, clOrdId.getString(), side.getValue()))
    {
        return;
    }

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

    // A cancel sent at once waits for it (#380), and a status request reports it Pending New (#383)
    Queues::OrderEvent evt(order);
    pushUnanswered(sourceStr, clOrdStr, Unanswered{ order->orderQty_, std::string() }, evt);
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

    // As for NewOrderSingle: the ClOrdID under the session's key (#71), and one the session has sent before answered
    // first (#382)
    const std::string sourceStr = makeSourceString(sid);
    const std::string clOrdStr = sessionClOrdId(sourceStr, clOrdId.getString());
    if (answerRepeatedOrder(msg, sid, sourceStr, clOrdStr, clOrdId.getString(), side.getValue()))
    {
        return;
    }

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
    pushUnanswered(sourceStr, clOrdStr, Unanswered{ order->orderQty_, std::string() }, evt); // as for NewOrderSingle
}

void FixGateway::onMessage(const FIX50SP2::OrderCancelRequest &msg, const FIX::SessionID &sid)
{
    FIX::OrigClOrdID origClOrdId;
    msg.get(origClOrdId);
    FIX::ClOrdID clOrdId;
    msg.get(clOrdId);

    // Only the session's own orders: another session's is answered as unknown, without saying that it exists (#71).
    // One the session sent just before is waited for (#380), and a refused replacement names the order it was to
    // replace (#398).
    const std::string sourceStr = makeSourceString(sid);
    std::string origClOrdStr = origClOrdId.getString();
    bool unanswered = false;
    OrderEntry *order = locateAnsweredOrder(sourceStr, origClOrdStr, &unanswered);
    if (!order && !unanswered)
    {
        order = locateRefusedOriginal(sourceStr, &origClOrdStr);
    }
    if (!order)
    {
        aux::ExchLogger::instance()->error("FIX: Cancel - order not found: " + origClOrdStr +
                                           (unanswered ? ", nor answered by the engine yet (#380)" : ""));
        // Answered here rather than by the engine, which only rejects an unknown order id (#57): here the request's
        // own ClOrdID is known, so the reject can quote it (#73)
        Queues::CancelRejectEvent unknown; // UNKNOWN_ORDER, with no order status
        try
        {
            FIX50SP2::OrderCancelReject reject =
                unanswered ? buildUnansweredOrderReject(FIX::CxlRejResponseTo_ORDER_CANCEL_REQUEST, clOrdId.getString(),
                                                        origClOrdStr)
                           : buildCancelReject(unknown, "NONE", clOrdId.getString(), origClOrdStr);
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

    // Only the session's own orders, as for a cancel (#71), waiting for one the session sent just before (#380). A
    // refused replacement names the order it was to replace, which the request is decided against, and its answers
    // name in OrigClOrdID (41), as EP284's D.2.c's (Z,X) (#398).
    const std::string sourceStr = makeSourceString(sid);
    std::string origClOrdStr = origClOrdId.getString();
    bool unanswered = false;
    OrderEntry *existing = locateAnsweredOrder(sourceStr, origClOrdStr, &unanswered);
    if (!existing && !unanswered)
    {
        existing = locateRefusedOriginal(sourceStr, &origClOrdStr);
    }
    if (!existing)
    {
        aux::ExchLogger::instance()->error("FIX: Replace - order not found: " + origClOrdStr +
                                           (unanswered ? ", nor answered by the engine yet (#380)" : ""));
        // Answered here, as a cancel of an unknown order is (#73), quoting the request's own ClOrdIDs
        try
        {
            FIX50SP2::OrderCancelReject reject =
                unanswered ? buildUnansweredOrderReject(FIX::CxlRejResponseTo_ORDER_CANCEL_REPLACE_REQUEST,
                                                        clOrdId.getString(), origClOrdStr)
                           : buildUnknownReplaceReject(clOrdId.getString(), origClOrdStr);
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

    // A request the gateway refuses itself gets an OrderCancelReject (102=99) that says why, quoting its ClOrdIDs; one
    // whose ClOrdID is in use, 102=6, as the engine's refusal of it does (#403). The replacement is remembered as
    // refused, as the engine's refusals are (#398), but for a ClOrdID in use, which names another order.
    auto refuse = [&](const std::string &reason, bool duplicate = false)
    {
        if (!duplicate)
        {
            learnRefusal(sourceStr, clOrdId.getString(), origClOrdStr);
        }
        Queues::OrderRejectEvent refusal;
        refusal.replacement_ = true;
        refusal.duplicateClOrderId_ = duplicate;
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
    // A ClOrdID the session already uses, for an order stored or still on its way, is refused here, as the engine
    // refuses it (#67) and the gateway a new order's (#382). Left to the engine, the replacement held the key in the
    // record until then: a status request for it was answered with the replace's original, and of two on their way
    // with the key, the later could be stored and the earlier refused (#403).
    std::optional<Unanswered> pending;
    if ((nullptr != locateSentOrder(clOrdStr, &pending)) || pending)
    {
        refuse("Replace refused: ClOrdID " + clOrdId.getString() + " is already in use", true);
        return;
    }
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

    // The queue owns the replacement once pushed (#74). A replace sent at once that names the replacement waits for it,
    // and is then decided against it, as in EP284's D.2.a (#380); a status request for it reports its original,
    // Pending Replace (#383).
    Unanswered record{ replacement->orderQty_, rawText(existing->clOrderId_.get()) };
    Queues::OrderReplaceEvent evt(existing->orderId_, replacement.release());
    pushUnanswered(sourceStr, clOrdStr, std::move(record), evt);
}

void FixGateway::onMessage(const FIX50SP2::OrderStatusRequest &msg, const FIX::SessionID &sid)
{
    FIX50SP2::ExecutionReport report = answerOrderStatus(msg, sid);
    try
    {
        FIX::Session::sendToTarget(report, sid);
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("FixGateway: could not send the order status: ") + ex.what());
    }
}

FIX50SP2::ExecutionReport FixGateway::answerOrderStatus(const FIX50SP2::OrderStatusRequest &msg,
                                                        const FIX::SessionID &sid)
{
    FIX::ClOrdID clOrdId;
    const bool byClOrdId = msg.getFieldIfSet(clOrdId);
    FIX::OrderID orderId;
    const bool byOrderId = msg.getFieldIfSet(orderId);
    if (!byClOrdId && !byOrderId)
    {
        // FIX 5.0 SP2 requires neither, and the gateway needs one: answered as fromApp() answers a missing field (380=5)
        throw FIX::FieldNotFound(FIX::FIELD::ClOrdID);
    }

    // The session's own orders alone, as for a cancel (#71): by its ClOrdID, as the client knows an order from the
    // start, or else by the OrderID the gateway gave it
    const std::string source = makeSourceString(sid);
    std::optional<Unanswered> pending;
    OrderEntry *order = nullptr;
    if (byClOrdId)
    {
        order = locateReceivedOrder(source, clOrdId.getString(), &pending);
    }
    else if (const std::optional<u64> number = toOrderNumber(orderId.getString()))
    {
        // An order number two orders have, as only a data directory from before #58 can hold, names neither
        order = orderStorage_->locateByOrderNumber(*number);
        if ((nullptr != order) && (source != sourceOf(*order)))
        {
            order = nullptr;
        }
        if (nullptr != order)
        {
            pending = unansweredOf(rawText(order->clOrderId_.get()));
        }
    }

    // The status of an order no one can name: OrdStatus Rejected, unknown order, OrderID NONE, the request's ClOrdID,
    // Side and Symbol, and no quantities (EP284's G.1.a)
    FIX::Side side;
    msg.get(side);
    const auto unknown = [&](const std::string &text)
    {
        FIX50SP2::ExecutionReport report =
            orderStatusReport("NONE", FIX::OrdStatus_REJECTED, side.getValue(), 0, 0, 0, 0);
        report.set(FIX::OrdRejReason(FIX::OrdRejReason_UNKNOWN_ORDER));
        report.set(FIX::Text(text));
        return report;
    };
    std::optional<FIX50SP2::ExecutionReport> answer = answerReceivedOrder(
        order, pending, source, byClOrdId ? clOrdId.getString() : std::string(), side.getValue(), true);
    // Named by both, they must name the same order: the OrderID (37) is that of the order the ClOrdID names, or the one
    // the answer reports for it along a replace, which the client has from the replace it sent or from the
    // acknowledgement of it. A new order the engine hasn't stored has none yet.
    if (answer && byClOrdId && byOrderId && (orderId.getString() != answer->getField(FIX::FIELD::OrderID)) &&
        ((nullptr == order) || (orderId.getString() != std::to_string(order->orderId_.id_))))
    {
        answer = unknown("Unknown order: OrderID (37) " + orderId.getString() + " is not the order of ClOrdID (11) " +
                         clOrdId.getString());
    }
    if (!answer)
    {
        answer = unknown("Unknown order");
    }
    // An order the request names but the store doesn't have goes by the request's ClOrdID and Symbol, and every answer
    // carries the request's OrdStatusReqID (790), which ties it to the request
    if (byClOrdId && !answer->isSetField(FIX::FIELD::ClOrdID))
    {
        answer->set(clOrdId);
    }
    FIX::Symbol symbol;
    if (!answer->isSetField(FIX::FIELD::Symbol) && msg.getFieldIfSet(symbol))
    {
        answer->set(symbol);
    }
    FIX::OrdStatusReqID reqId;
    if (msg.getFieldIfSet(reqId))
    {
        answer->set(reqId);
    }
    return *answer;
}

FIX50SP2::ExecutionReport FixGateway::answerStoredOrder(const OrderEntry &order, const std::string &source,
                                                        const std::string &requested,
                                                        const std::optional<Unanswered> &pending,
                                                        bool followReplacements) const
{
    const std::string key = rawText(order.clOrderId_.get());
    const std::string asked = requested.empty() ? clientClOrdId(key, source) : requested;
    if (pending && !pending->origKey.empty())
    {
        // A replacement the engine has stored and not yet acknowledged: still pending for the client, so its original,
        // Pending Replace, as for one not stored yet
        RawDataEntry rawKey(STRING_RAWDATATYPE, pending->origKey.c_str(), static_cast<u32>(pending->origKey.size()));
        if (const OrderEntry *original = orderStorage_->locateByClOrderId(rawKey))
        {
            const OrderSeen seen = seenOf(*original);
            return storedOrderStatusReport(*original, seen, FIX::OrdStatus_PENDING_REPLACE, leavesWhileReplacing(seen),
                                           asked, clientClOrdId(pending->origKey, source));
        }
    }
    const OrderSeen seen = seenOf(order);
    if (pending)
    {
        // Stored, and not yet acknowledged, nor durable until its booking commits: Pending New (G.1.c). Nothing has
        // traded yet: the order's first transaction sends its acknowledgement before it applies a fill.
        return storedOrderStatusReport(order, seen, FIX::OrdStatus_PENDING_NEW, seen.leavesQty, asked, std::string());
    }
    // A replaced order is answered with the order working in its place, naming the one asked for in OrigClOrdID (41),
    // as EP284's G.1.c answers a status request for X after Replace (Y,X): the replacements are followed, no more of
    // them than the gateway knows of
    const OrderEntry *described = &order;
    std::string describedKey = key;
    OrderSeen describedSeen = seen;
    for (size_t followed = 0; REPLACED_ORDSTATUS == describedSeen.status; ++followed)
    {
        const OrderEntry *next = (followed < knownReplacements()) ? replacementOf(describedKey) : nullptr;
        if (nullptr == next)
        {
            // Replaced, and its replacement's report isn't out yet: the replace is still pending for the client
            const std::string clOrd = clientClOrdId(describedKey, source);
            return storedOrderStatusReport(*described, describedSeen, FIX::OrdStatus_PENDING_REPLACE,
                                           leavesWhileReplacing(describedSeen), clOrd,
                                           (clOrd == asked) ? std::string() : asked);
        }
        if (!followReplacements)
        {
            break; // its own state, which its replacement's report gave the client
        }
        described = next;
        describedKey = rawText(next->clOrderId_.get());
        describedSeen = seenOf(*next);
    }
    const std::string clOrd = clientClOrdId(describedKey, source);
    return storedOrderStatusReport(*described, describedSeen, fromOrdStatus(describedSeen.status),
                                   describedSeen.leavesQty, clOrd, (clOrd == asked) ? std::string() : asked);
}

std::optional<FIX50SP2::ExecutionReport> FixGateway::answerReceivedOrder(const OrderEntry *order,
                                                                         const std::optional<Unanswered> &pending,
                                                                         const std::string &source,
                                                                         const std::string &clOrdId, char side,
                                                                         bool followReplacements) const
{
    if (nullptr != order)
    {
        return answerStoredOrder(*order, source, clOrdId, pending, followReplacements);
    }
    if (!pending)
    {
        return std::nullopt;
    }
    if (!pending->origKey.empty())
    {
        // A replacement the engine hasn't stored yet: the replace is pending, so its original is reported, under the
        // replacement's ClOrdID and naming the original's, as EP284's G.1.c acknowledges one (row 14)
        RawDataEntry rawKey(STRING_RAWDATATYPE, pending->origKey.c_str(), static_cast<u32>(pending->origKey.size()));
        if (const OrderEntry *original = orderStorage_->locateByClOrderId(rawKey))
        {
            const OrderSeen seen = seenOf(*original);
            return storedOrderStatusReport(*original, seen, FIX::OrdStatus_PENDING_REPLACE, leavesWhileReplacing(seen),
                                           clOrdId, clientClOrdId(pending->origKey, source));
        }
    }
    // Received, and not yet stored: Pending New, with the quantity it was sent with (G.1.c), and no OrderID yet
    return orderStatusReport("NONE", FIX::OrdStatus_PENDING_NEW, side, pending->orderQty, 0, pending->orderQty, 0);
}

bool FixGateway::answerRepeatedOrder(const FIX::Message &msg, const FIX::SessionID &sid, const std::string &source,
                                     const std::string &key, const std::string &clOrdId, char side)
{
    std::optional<Unanswered> pending;
    const OrderEntry *order = locateSentOrder(key, &pending);
    if ((nullptr == order) && !pending) [[likely]]
    {
        return false;
    }
    // EP284's F.1.b: sent again, PossResend (97) Y, it gets the order's state, as a status request for it does. F.1.a:
    // anything else is a duplicate, refused with the status of the order that has the ClOrdID: that order's own, since
    // the reject goes by its ClOrdID, not that of an order that replaced it. QuickFIX's session leaves 97 to the
    // application.
    FIX::PossResend possResend(false);
    msg.getHeader().getFieldIfSet(possResend);
    FIX50SP2::ExecutionReport answer =
        *answerReceivedOrder(order, pending, source, clOrdId, side, possResend.getValue());
    if (!possResend.getValue())
    {
        answer = buildDuplicateReject(std::move(answer), "Order refused: ClOrdID " + clOrdId + " is already in use",
                                      IdTGenerator::instance()->getId().id_);
    }
    // An order the engine hasn't stored goes by the new order's ClOrdID and Symbol, as for a status request
    if (!answer.isSetField(FIX::FIELD::ClOrdID))
    {
        answer.set(FIX::ClOrdID(clOrdId));
    }
    FIX::Symbol symbol;
    if (!answer.isSetField(FIX::FIELD::Symbol) && msg.getFieldIfSet(symbol))
    {
        answer.set(symbol);
    }
    try
    {
        FIX::Session::sendToTarget(answer, sid);
    }
    catch (const std::exception &ex)
    {
        aux::ExchLogger::instance()->error(std::string("FixGateway: could not answer the repeated order: ") +
                                           ex.what());
    }
    return true;
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
    // An order's first report tells a request waiting for the order that it's stored (#380). It does so once the report
    // is out, so that the gateway's own answer to the request can't go out first, and on every way out of here: in
    // tests, too, that have no session. A trade's report, like every other that can't be its order's first, reads
    // nothing, and none does while no order is unanswered.
    std::string_view key;
    if (canBeFirstReport(exec->type_) && (0 < unansweredCount_.load(std::memory_order_acquire)))
    {
        const RawDataEntry &clOrdId = order.clOrderId_.get();
        if (nullptr != clOrdId.data_)
        {
            key = std::string_view(clOrdId.data_, clOrdId.length_);
        }
    }
    const AnsweredOnExit answer(*this, key);

    // A replacement's report (the original's isn't sent): a status request that names the order it replaced is answered
    // with it from now on (#383). Learned before the report goes out, so that a request the client sends on reading it
    // finds it, and for an order of a session no longer configured too, which nothing is sent to.
    if (REPLACE_EXECTYPE == exec->type_)
    {
        learnReplacement(order);
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
    // OrderQty (38) too, or a closed order's quantity can't be read from its reports once LeavesQty is 0 (#384). A
    // replacement's is its own. Read without the order's lock, as the other quantities are: it never changes once the
    // order is stored, since a replace stores a new order (#74). Written as the whole number it is, the bytes QuickFIX
    // writes for the double: its conversion, to 15 significant digits, falls back to bignum arithmetic for these
    // values, about 2,900 instructions a field.
    report.setField(FIX::FIELD::OrderQty, std::to_string(order.orderQty_));

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
    // The refusal answers the order or replacement for a request waiting for it (#380), once it's out, as a report does.
    // evnt names the ClOrdID as it is stored, under the session's key.
    const AnsweredOnExit answer(*this, evnt.clOrderId_);
    // A refused replacement is remembered before the refusal goes out and the requests waiting for it wake, logged on or
    // not: a cancel or replace that names it is decided against its original (#398). A refused new order's ClOrdID, if
    // a refused replacement had it, names no order now. Not for a ClOrdID in use, which names another order, nor for
    // another source's: every refusal comes here.
    if (!evnt.duplicateClOrderId_ && source.starts_with(FIX_SOURCE_PREFIX))
    {
        if (evnt.replacement_)
        {
            learnRefusal(source, clientClOrdId(evnt.clOrderId_, source), clientClOrdId(evnt.origClOrderId_, source));
        }
        else
        {
            forgetRefusal(source, clientClOrdId(evnt.clOrderId_, source));
        }
    }
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
            const u64 execId = IdTGenerator::instance()->getId().id_;
            // A duplicate's reject carries the status of the order that has its ClOrdID, as EP284's F.1.a combines them
            // (#382). The gateway answers its sessions' duplicates before the engine sees them (answerRepeatedOrder()),
            // so this is for one it didn't: that order is read from the store alone, since the gateway's record of
            // what it sent under the ClOrdID would be the duplicate's.
            const OrderEntry *holder = nullptr;
            if (evnt.duplicateClOrderId_)
            {
                RawDataEntry rawKey(STRING_RAWDATATYPE, evnt.clOrderId_.c_str(),
                                    static_cast<u32>(evnt.clOrderId_.size()));
                holder = orderStorage_->locateByClOrderId(rawKey);
            }
            FIX50SP2::ExecutionReport report =
                (nullptr != holder)
                    ? buildDuplicateReject(answerStoredOrder(*holder, source, shown.clOrderId_, std::nullopt, false),
                                           shown.reason_, execId)
                    : buildOrderReject(shown, execId);
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

FIX50SP2::ExecutionReport FixGateway::buildDuplicateReject(FIX50SP2::ExecutionReport status, const std::string &reason,
                                                           u64 execId)
{
    // An ExecID of its own: FIX gives every status report (150=I) ExecID 0, and a reject is no status report
    status.set(FIX::ExecID("R" + std::to_string(execId)));
    status.set(FIX::ExecType(FIX::ExecType_REJECTED));
    status.set(FIX::OrdRejReason(FIX::OrdRejReason_DUPLICATE_ORDER));
    status.set(FIX::Text(reason));
    return status;
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

FIX50SP2::OrderCancelReject FixGateway::buildUnansweredOrderReject(char responseTo, const std::string &clOrdId,
                                                                   const std::string &origClOrdId)
{
    FIX50SP2::OrderCancelReject reject(FIX::OrderID("NONE"), FIX::ClOrdID(clOrdId),
                                       FIX::OrdStatus(FIX::OrdStatus_PENDING_NEW), FIX::CxlRejResponseTo(responseTo));
    reject.set(FIX::OrigClOrdID(origClOrdId)); // optional since FIX 5.0, as for the other rejects
    reject.set(FIX::CxlRejReason(FIX::CxlRejReason_OTHER));
    // In the words of the gateway's other answers to each: "Cancel rejected: unknown order", "Replace refused: ..."
    reject.set(FIX::Text(
        std::string(FIX::CxlRejResponseTo_ORDER_CANCEL_REQUEST == responseTo ? "Cancel rejected" : "Replace refused") +
        ": the order isn't acknowledged yet"));
    return reject;
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
