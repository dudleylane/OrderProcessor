#pragma once

#ifdef BUILD_FIX

#include <quickfix/Application.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <oneapi/tbb/spin_rw_mutex.h>
#include <quickfix/SessionID.h>
#include <quickfix/Session.h>
#include <quickfix/fix50sp2/NewOrderSingle.h>
#include <quickfix/fix50sp2/NewOrderMultileg.h>
#include <quickfix/fix50sp2/OrderCancelRequest.h>
#include <quickfix/fix50sp2/OrderCancelReplaceRequest.h>
#include <quickfix/fix50sp2/OrderStatusRequest.h>
#include <quickfix/fix50sp2/ExecutionReport.h>
#include <quickfix/fix50sp2/OrderCancelReject.h>
#include <quickfix/fix50sp2/BusinessMessageReject.h>
#include <quickfix/Group.h>
#include "QueuesDef.h"
#include "DataModelDef.h"
#include "FixCredentials.h"

// Catch QuickFIX ABI breaks at compile time (GroupArena added m_arena to FieldMap).
static_assert(sizeof(FIX::Group) == 128, "FIX::Group size changed — ABI break. Clean rebuild required.");

namespace COP
{

namespace Store
{
class WideParamsDataStorage;
class OrderDataStorage;
} // namespace Store

namespace App
{

class FixGateway : public FIX::Application
{
public:
    /// How long a cancel or replace waits, by default, for the engine to store or refuse the order it names, when that
    /// order was sent just before it (#380). The session's thread waits, and sends no heartbeat meanwhile: well under
    /// config/fix.cfg's HeartBtInt, 30 s.
    static constexpr std::chrono::milliseconds DEFAULT_ANSWER_WAIT{ 1000 };

    FixGateway(Queues::InQueues *inQueues, Store::WideParamsDataStorage *wideData,
               Store::OrderDataStorage *orderStorage, SourceIdT defaultClearingId = SourceIdT(),
               SourceIdT defaultAccountId = SourceIdT(), std::chrono::milliseconds answerWait = DEFAULT_ANSWER_WAIT);

    /// Who may log on, and what each counterparty may trade (#147), which the gateway checks from now on: set it before the
    /// acceptor starts. Throws std::runtime_error if it lists an account the data directory doesn't have. Without it, as
    /// in tests, every Logon of FIX 5.0 SP2 is accepted and an order trades the account it names.
    void setCredentials(const FixCredentials *credentials);

    // Application interface
    void onCreate(const FIX::SessionID &) override;
    void onLogon(const FIX::SessionID &) override;
    void onLogout(const FIX::SessionID &) override;
    void toAdmin(FIX::Message &, const FIX::SessionID &) override;
    void toApp(FIX::Message &, const FIX::SessionID &) override;
    void fromAdmin(const FIX::Message &, const FIX::SessionID &) override;
    void fromApp(const FIX::Message &, const FIX::SessionID &) override;

    // Inbound handlers, called by fromApp() for the message types it dispatches
    void onMessage(const FIX50SP2::NewOrderSingle &, const FIX::SessionID &);
    void onMessage(const FIX50SP2::NewOrderMultileg &, const FIX::SessionID &);
    void onMessage(const FIX50SP2::OrderCancelRequest &, const FIX::SessionID &);
    void onMessage(const FIX50SP2::OrderCancelReplaceRequest &, const FIX::SessionID &);
    /// Answered on the session's thread with answerOrderStatus()'s report: a status request only reads, so nothing
    /// reaches the engine (#383)
    void onMessage(const FIX50SP2::OrderStatusRequest &, const FIX::SessionID &);

    /// The ExecutionReport answering an OrderStatusRequest (35=H) from sid (#383), as EP284's section G answers one:
    /// ExecType Order Status (150=I), ExecID 0, and the state of the order the request names among the session's, by
    /// ClOrdID (11), OrderID (37), or both, which must name the same order. Another session's order is unknown, as for a
    /// cancel (#71). An unknown order gets OrdStatus Rejected (39=8) with OrdRejReason 5 (G.1.a). One the session sent
    /// that the engine hasn't acknowledged yet is Pending New (39=A, G.1.c), and a replacement not yet acknowledged is
    /// its original, Pending Replace (39=E). A replaced order is answered with the order that replaced it, naming the
    /// one requested in OrigClOrdID (41), as G.1.c does. OrdStatusReqID (790) is echoed. Throws FIX::FieldNotFound for a
    /// request that names no order by either field. Public so that tests can check it without a session.
    FIX50SP2::ExecutionReport answerOrderStatus(const FIX50SP2::OrderStatusRequest &msg, const FIX::SessionID &sid);

    // Outbound — called by FixOutQueues
    /// requestClOrdId is the ClOrdID of the cancel request a cancel's acknowledgement answers (#98); empty otherwise
    void sendExecutionReport(const ExecutionEntry *exec, const OrderEntry &order,
                             const std::string &requestClOrdId = std::string());
    /// Refuses a cancel to the session that sent it, which target names (#73); silent when that is not a FIX session
    void sendCancelReject(const Queues::CancelRejectEvent &evnt, const std::string &target);
    /// A BusinessMessageReject to the session of the order refOrderId names, and nothing when it names none (#189)
    void sendBusinessReject(const IdT &refOrderId, const std::string &reason);
    /// Refuses, to its sender, a new order or replacement the engine did not store (#67); source names the session
    void sendOrderReject(const Queues::OrderRejectEvent &evnt, const std::string &source);

    // Session lookup
    bool hasFixSession(const std::string &source) const;

    /// How many cancel and replace requests are waiting now for the engine to store or refuse the order they name
    /// (#380). Tests wait for one before they let the engine go on.
    size_t waitingRequests() const;

    // Build source string from SessionID
    static std::string makeSourceString(const FIX::SessionID &sid);
    /// The key a FIX order's ClOrdID is stored under: its session's source string (makeSourceString()), '|', and the
    /// ClOrdID as the client sent it. FIX scopes a ClOrdID to the client that sent it, so two sessions may use the same
    /// one, and a session finds only its own orders by it (#71).
    static std::string sessionClOrdId(const std::string &source, const std::string &clOrdId);
    /// The ClOrdID a client sent, from the key it is stored under: the key without source's prefix. A key without that
    /// prefix, as an order stored before #71 has, or a ClOrdID the server made up, comes back whole.
    static std::string clientClOrdId(const std::string &stored, const std::string &source);
    /// The OrderRejectEvent as the session that sent the order sees it: its ClOrdIDs, and the reason, which quotes a
    /// ClOrdID in use as it is stored, without the session's prefix (#71)
    static Queues::OrderRejectEvent clientView(const Queues::OrderRejectEvent &evnt, const std::string &source);

private:
    /// builds the typed message for its MsgType and calls the matching handler
    void dispatch(const FIX::Message &msg, const FIX::SessionID &sid);
    /// logs an exception a handler threw and answers the one message with a BusinessMessageReject
    void rejectContained(const FIX::Message &msg, const FIX::SessionID &sid, const char *what);
    /// answers the one message with a BusinessMessageReject to its sender, quoting its MsgSeqNum, MsgType and ClOrdID
    void rejectMessage(const FIX::Message &msg, const FIX::SessionID &sid, int reason, const std::string &text);
    /// answers a message whose ApplVerID (1128) isn't FIX 5.0 SP2's with a Reject (35=3) to its sender: RefTagID 1128,
    /// SessionRejectReason 18, invalid or unsupported application version
    void rejectApplVerId(const FIX::Message &msg, const FIX::SessionID &sid);
    /// refuses a new order to its sender with buildOrderRefusal()'s ExecutionReport, before the engine sees it (#145)
    void refuseOrder(const FIX::Message &msg, const FIX::SessionID &sid, int ordRejReason, const std::string &text);
    /// Reads a settlement date field from fields, the order msg or one of its legs, if set, as a LocalMktDate (#198).
    /// Returns false, having refused msg to its sender, if it isn't one (#190). name says which field, in the Text.
    bool readSettlDate(const FIX::FieldMap &fields, const FIX::Message &msg, const FIX::SessionID &sid, int tag,
                       const char *name, DateTimeT *date);
    /// Applies the session's account list to an order's Account (#147): an order that names none trades the
    /// counterparty's first account, and one that names another account is refused, with OrdRejReason 15. Returns false
    /// when it refused the order. Without credentials, the account stays as the order names it.
    bool applySessionAccounts(const FIX::Message &msg, const FIX::SessionID &sid, std::string *account);
    /// The order a request from the session source names by ClOrdID (#71): the one stored under the session's key, or,
    /// failing that, the one stored under the ClOrdID alone if this session placed it, as an order stored before #71
    /// was. Another session's order is not found.
    OrderEntry *locateSessionOrder(const std::string &source, const std::string &clOrdId) const;
    /// What the gateway keeps of a new order or replacement it sent the engine, until the engine answers it (#380)
    struct Unanswered
    {
        QuantityT orderQty = 0; // its OrderQty (38), which a status request's Pending New reports (#383)
        std::string origKey;    // a replacement's: the key of the order it replaces; empty for a new order
    };
    /// The order a request from source names by ClOrdID, as locateSessionOrder() finds it, and, in *pending, what the
    /// gateway keeps of it while the engine hasn't answered it, without waiting (#383). Looked for, its record read, and
    /// looked for again: stored since the first look, it is found by the second.
    OrderEntry *locateReceivedOrder(const std::string &source, const std::string &clOrdId,
                                    std::optional<Unanswered> *pending) const;
    /// The order the session sent with the ClOrdID stored under key, as the engine's own duplicate check finds one
    /// (#382): the one stored under key, and in *pending what the gateway keeps of it while the engine hasn't answered
    /// it. The record is read before the store: the engine answers an order after it stores it, and the answer takes
    /// its key out of the record, so an order the record no longer has is in the store, if the engine stored it. Read
    /// the other way round, an order answered between the two looks would be in neither. An order stored before #71,
    /// under its ClOrdID alone, isn't looked for, as the engine doesn't look for it.
    OrderEntry *locateSentOrder(const std::string &key, std::optional<Unanswered> *pending) const;
    /// The order a cancel or replace from source names, as locateSessionOrder() finds it, once the engine has answered
    /// it (#380). An order the session sent just before the request may not be stored yet: the request waits for the
    /// engine to store or refuse it, up to answerWait_. When nothing is found, *unanswered says whether that is because
    /// the engine still hasn't answered the order, rather than because no such order exists.
    OrderEntry *locateAnsweredOrder(const std::string &source, const std::string &clOrdId, bool *unanswered);
    /// Pushes evnt, the new order or replacement whose ClOrdID is stored under key, to the engine: unanswered until the
    /// engine reports it or refuses it (#380), and kept meanwhile as record says
    template <typename Event>
    void pushUnanswered(const std::string &source, const std::string &key, Unanswered record, const Event &evnt);
    /// What the gateway keeps of the order or replacement whose ClOrdID is stored under key, if the engine hasn't
    /// answered it yet (#383)
    std::optional<Unanswered> unansweredOf(std::string_view key) const;
    /// The engine has answered the order or replacement whose ClOrdID is stored under key: it sent its first report,
    /// or refused it. Ends the wait of each request that names it.
    void answered(std::string_view key);
    /// A replacement of a FIX order: a status request that names the order it replaced is answered with it from now on
    /// (#383). Nothing for any other order.
    void learnReplacement(const OrderEntry &replacement);
    /// The order that replaced the one whose ClOrdID is stored under key, or nothing when none did, or its report isn't
    /// out yet (#383)
    OrderEntry *replacementOf(std::string_view key) const;
    /// How many replacements the gateway knows of, which no chain of them can be longer than (#383)
    size_t knownReplacements() const;
    /// The answer to a status request for an order the engine has stored, read under its lock (#383). requested is the
    /// ClOrdID the request named, or empty; pending, what the gateway keeps of the order while the engine hasn't
    /// answered it. followReplacements: a replaced order is answered with the order working in its place, as G.1.c
    /// answers one; else with its own state (#382). Either way it is Pending Replace until its replacement's report is
    /// out.
    FIX50SP2::ExecutionReport answerStoredOrder(const OrderEntry &order, const std::string &source,
                                                const std::string &requested, const std::optional<Unanswered> &pending,
                                                bool followReplacements) const;
    /// What a status request for clOrdId is answered with, for the order a lookup found and what the gateway keeps of
    /// it, pending (#383); nothing when neither is there. side is the request's, for an order the engine hasn't stored.
    /// followReplacements as for answerStoredOrder().
    std::optional<FIX50SP2::ExecutionReport> answerReceivedOrder(const OrderEntry *order,
                                                                 const std::optional<Unanswered> &pending,
                                                                 const std::string &source, const std::string &clOrdId,
                                                                 char side, bool followReplacements) const;
    /// Answers a new order msg from sid whose ClOrdID the session has sent before, before the engine sees it (#382),
    /// and returns true. Sent again, PossResend (97) Y, it gets that order's state, as a status request for it does
    /// (EP284's F.1.b); otherwise it is refused as a duplicate with that order's status (F.1.a). Returns false, having
    /// sent nothing, for a ClOrdID the session hasn't sent. key is the ClOrdID under the session's key.
    bool answerRepeatedOrder(const FIX::Message &msg, const FIX::SessionID &sid, const std::string &source,
                             const std::string &key, const std::string &clOrdId, char side);
    /// Calls answered() for its key, unless that is empty, when it goes out of scope: once a report or a refusal is
    /// out, or on a way out without one
    class AnsweredOnExit;

    Queues::InQueues *inQueues_;
    Store::WideParamsDataStorage *wideData_;
    Store::OrderDataStorage *orderStorage_;

    /// Every session QuickFIX has created, by source string: from onCreate, not from logon. A report for a session's
    /// order goes to QuickFIX while the client is logged out too, and QuickFIX keeps it for a ResendRequest (#160).
    std::unordered_map<std::string, FIX::SessionID> sessionMap_;
    std::unordered_set<std::string> loggedOn_;          // the sessions logged on now, by source string
    mutable oneapi::tbb::spin_rw_mutex sessionMapLock_; // guards both

    SourceIdT defaultClearingId_;                 // pre-resolved clearing for FIX orders
    SourceIdT defaultAccountId_;                  // pre-resolved account for FIX orders that name none (#34)
    const FixCredentials *credentials_ = nullptr; // who may log on and what each may trade (#147); owned by FixServer

    /// Hashes a key and a view of one alike, so that a report's ClOrdID is looked up without a copy
    struct KeyHash
    {
        using is_transparent = void;
        size_t operator()(std::string_view key) const noexcept
        {
            return std::hash<std::string_view>{}(key);
        }
    };
    /// The new orders and replacements the gateway has sent the engine, and the engine hasn't yet stored and reported
    /// or refused, by the key their ClOrdID is stored under (#380). A request that names one waits for its answer.
    std::unordered_map<std::string, Unanswered, KeyHash, std::equal_to<>> unanswered_;
    size_t waiting_ = 0;                         // requests waiting for an answer now
    mutable std::mutex answersLock_;             // guards both
    std::condition_variable answers_;            // notified on each answer
    std::atomic<size_t> unansweredCount_{ 0 };   // unanswered_.size(), which the engine's reports read without the lock
    const std::chrono::milliseconds answerWait_; // how long a request waits for its order's answer

    /// The key of the order that replaced each replaced FIX order, by the key of the one it replaced (#383): learned
    /// from each replacement's report, and at startup from the orders stored. Kept for good, as the orders are.
    std::unordered_map<std::string, std::string, KeyHash, std::equal_to<>> replacements_;
    mutable std::mutex replacementsLock_; // guards it; taken with no other lock held

public:
    // FIX → internal enum conversions (public static for testability)
    static Side toSide(char fixSide);
    static OrderType toOrdType(char fixOrdType);
    static TimeInForce toTif(char fixTif);
    static Currency toCurrency(const std::string &fixCcy);
    /// OrderQty (38) as the engine's quantity: a whole number from 1 to the largest QuantityT, or nothing. QuickFIX hands
    /// a QTY field over as a double with no range check, and converting a double outside QuantityT's range is undefined
    /// behaviour (#145, as #16 was in the JSON parser).
    static std::optional<QuantityT> toQuantity(double orderQty);
    /// Whether an order of ordType can trade at price, a Price (44), LegPrice (566) or a swap's far price (6500): finite,
    /// and above zero unless the order is a market order, which trades at no price of its own. QuickFIX accepts any
    /// number of digits in a PRICE field, and 310 of them convert to infinity (#145).
    static bool isTradablePrice(double price, OrderType ordType);
    /// A LocalMktDate (YYYYMMDD), as the engine keeps a settlement date: milliseconds since the epoch at midnight UTC of
    /// that day, as the frontend sends one (#198). Nothing for anything else: not 8 digits, no such day, or before 1970.
    /// The gateway used to read these fields with std::stoull, as milliseconds: "20261015" was 5.6 hours into 1970, "-5"
    /// wrapped to 18446744073709551611, and a non-number threw after the order was half stored (#190).
    static std::optional<DateTimeT> fromLocalMktDate(std::string_view text);
    /// The LocalMktDate of a settlement date the engine keeps: its day in UTC (#198)
    static std::string toLocalMktDate(DateTimeT date);
    /// Why an order of tif is refused for its ExpireTime (126), or nothing, with *expireTime set to it for a GTD order
    /// (#240): the engine expires a GTD order then, in milliseconds since the epoch, and refuses one without it. A GTD
    /// order needs one ahead of now, which no other order may have, as on WebSocket. ExpireDate (432) isn't read.
    static std::optional<std::string> expiryRefusal(const FIX::FieldMap &msg, TimeInForce tif, DateTimeT now,
                                                    DateTimeT *expireTime);
    /// An expiry the engine keeps as an ExpireTime (126) carries it, to the millisecond (#240)
    static FIX::UtcTimeStamp toUtcTimeStamp(DateTimeT when);

    // Internal → FIX enum conversions
    static char fromOrdStatus(OrderStatus s);
    static char fromExecType(ExecType t);
    static char fromSide(Side s);

    /// The ExecutionReport (35=8) for one of an order's execution reports; sendExecutionReport() sends it to the order's
    /// session. Public and static so that tests can check it without a FIX session. A replacement's REPLACE report
    /// (150=5) also names the order it replaced in OrigClOrdID (41). A cancel's acknowledgement names the cancel
    /// request in ClOrdID (11), when requestClOrdId gives it, and the cancelled order in OrigClOrdID (41) (#98).
    static FIX50SP2::ExecutionReport buildExecutionReport(const ExecutionEntry *exec, const OrderEntry &order,
                                                          const std::string &requestClOrdId = std::string());
    /// Whether a FIX client gets this report. FIX acknowledges a replace once, with the replacement's report, as the
    /// Order State Changes scenarios do (C.1.a); the replaced order's own REPLACE report is not sent, since its status,
    /// OrdStatus 5 (Replaced), has been out of use since FIX 4.4 (#74).
    static bool reportsOverFix(const ExecutionEntry *exec);
    /// The ExecutionReport (35=8) rejecting a new order the engine refused without storing it (#67): OrderID NONE,
    /// ExecID R<execId>, 150=8, 39=8, OrdRejReason 6 when its ClOrdID is already in use (else 0), and the reason in
    /// Text (58). sendOrderReject() draws execId from the shared id generator, so it doesn't repeat after a restart
    /// (#81). For a ClOrdID in use by an order the store has, it sends buildDuplicateReject()'s instead (#382).
    static FIX50SP2::ExecutionReport buildOrderReject(const Queues::OrderRejectEvent &evnt, u64 execId);
    /// The ExecutionReport (35=8) refusing a new order whose ClOrdID an order of its session's has, as EP284's F.1.a
    /// combines one (#382): status, that order's status report, as a reject, with ExecID R<execId>, ExecType Rejected
    /// (150=8), OrdRejReason 6 (duplicate order) and reason in Text (58). Its OrdStatus, OrderID and quantities stay
    /// that order's.
    static FIX50SP2::ExecutionReport buildDuplicateReject(FIX50SP2::ExecutionReport status, const std::string &reason,
                                                          u64 execId);
    /// The ExecutionReport (35=8) refusing a new order the gateway rejects itself, before the engine sees it (#145): as
    /// buildOrderReject() builds one, with the order's ClOrdID, Symbol, and its Side and OrderQty as they were sent,
    /// ordRejReason in OrdRejReason (103) and text in Text (58)
    static FIX50SP2::ExecutionReport buildOrderRefusal(const FIX::Message &order, int ordRejReason,
                                                       const std::string &text, u64 execId);
    /// The OrderCancelReject (35=9) answering a cancel/replace request the engine refused (#67, #74): 434=2,
    /// CxlRejReason 6 when the replacement's ClOrdID is already in use, else from the engine's reason as for a cancel
    /// (0 too late, 1 unknown order, 3 a cancel or replace pending, 99 other), and the reason in Text (58). origOrderId
    /// and origStatus describe the order it was to replace.
    static FIX50SP2::OrderCancelReject buildReplaceReject(const Queues::OrderRejectEvent &evnt,
                                                          const std::string &origOrderId, char origStatus);
    /// The gateway's own answer to a cancel/replace request whose OrigClOrdID names no order (#74): 434=2, 102=1,
    /// OrderID NONE, OrdStatus 8, and the request's ClOrdIDs
    static FIX50SP2::OrderCancelReject buildUnknownReplaceReject(const std::string &clOrdId,
                                                                 const std::string &origClOrdId);
    /// The gateway's own answer to a cancel (responseTo 1) or a cancel/replace request (2) that names an order the
    /// session sent just before it, and the engine still hadn't stored or refused when the request had waited for it
    /// (#380): 102=99, OrderID NONE, OrdStatus A (Pending New), the request's ClOrdIDs, and why in Text (58)
    static FIX50SP2::OrderCancelReject buildUnansweredOrderReject(char responseTo, const std::string &clOrdId,
                                                                  const std::string &origClOrdId);
    /// The OrderCancelReject (35=9) refusing a cancel request (#73): 434=1, CxlRejReason from the engine's reason (0 too
    /// late, 1 unknown order, 3 a replace pending, 99 other), the order's OrdStatus (8 when the order is unknown), and
    /// why in Text (58), with clOrdId in ClOrdID (11) and origClOrdId in OrigClOrdID (41).
    static FIX50SP2::OrderCancelReject buildCancelReject(const Queues::CancelRejectEvent &evnt,
                                                         const std::string &orderId, const std::string &clOrdId,
                                                         const std::string &origClOrdId);
    /// The OrderCancelReject for a cancel the engine refused, given the order it named: ClOrdID (11) is the cancel
    /// request's own, which the engine carries back since #98, and OrigClOrdID (41) the order's; an event without the
    /// request's ClOrdID falls back to the order's in 11, as before.
    static FIX50SP2::OrderCancelReject buildEngineCancelReject(const Queues::CancelRejectEvent &evnt,
                                                               const std::string &orderId,
                                                               const std::string &orderClOrdId);
};

} // namespace App
} // namespace COP

#endif // BUILD_FIX
