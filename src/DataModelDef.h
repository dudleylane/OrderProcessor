/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <set>
#include <oneapi/tbb/spin_rw_mutex.h>
#include "TypesDef.h"
#include "WideDataLazyRef.h"
#include "StateMachineDef.h"

namespace COP
{

struct ExecTradeParams;
struct TradeExecEntry;

enum Currency
{
    INVALID_CURRENCY = 0,
    USD_CURRENCY,
    EUR_CURRENCY,
    GBP_CURRENCY,
    JPY_CURRENCY,
    CHF_CURRENCY,
    AUD_CURRENCY,
    CAD_CURRENCY,
    NZD_CURRENCY
};

enum InstrumentType
{
    INVALID_INSTRUMENTTYPE = 0,
    EQUITY_INSTRUMENTTYPE,
    FX_SPOT_INSTRUMENTTYPE,
    FX_SWAP_INSTRUMENTTYPE
};

// instrument's parameters, should be extended in future
struct InstrumentEntry
{
    StringT symbol_;
    StringT securityId_;
    StringT securityIdSource_;
    IdT id_;

    InstrumentType instrumentType_ = INVALID_INSTRUMENTTYPE;
    Currency baseCurrency_ = INVALID_CURRENCY;
    Currency termCurrency_ = INVALID_CURRENCY;

    bool isValid(std::string *invalid) const;
    bool isFxPair() const
    {
        return FX_SPOT_INSTRUMENTTYPE == instrumentType_ || FX_SWAP_INSTRUMENTTYPE == instrumentType_;
    }
};

class GroupInstrumentsBySymbol : public std::less<InstrumentEntry>
{
public:
    bool operator()(const InstrumentEntry &lft, const InstrumentEntry &rght) const;
};

enum AccountType
{
    INVALID_ACCOUNTTYPE = 0,
    PRINCIPAL_ACCOUNTTYPE,
    AGENCY_ACCOUNTTYPE
};

struct AccountEntry
{
    StringT account_;
    StringT firm_;
    IdT id_;
    AccountType type_ = INVALID_ACCOUNTTYPE;

    bool isValid(std::string *invalid) const;
};

struct ClearingEntry
{
    StringT firm_;
    IdT id_;

    bool isValid(std::string *invalid) const;
};

enum OrderType
{
    INVALID_ORDERTYPE = 0,
    MARKET_ORDERTYPE,
    LIMIT_ORDERTYPE,
    STOP_ORDERTYPE,
    STOPLIMIT_ORDERTYPE,
    FXSWAP_ORDERTYPE
};

enum ExecType
{
    INVALID_EXECTYPE = 0,
    NEW_EXECTYPE,
    TRADE_EXECTYPE,
    DFD_EXECTYPE,
    CORRECT_EXECTYPE,
    CANCEL_EXECTYPE,
    REJECT_EXECTYPE,
    REPLACE_EXECTYPE,
    EXPIRED_EXECTYPE,
    SUSPENDED_EXECTYPE,
    STATUS_EXECTYPE,
    RESTATED_EXECTYPE,
    PEND_CANCEL_EXECTYPE,
    PEND_REPLACE_EXECTYPE,
    // A stop order's stop was reached and the order released (#223, #236): FIX's ExecType L (Triggered or Activated by
    // System). Last, so the values above keep theirs.
    TRIGGERED_EXECTYPE
};

enum Capacity
{
    INVALID_CAPACITY = 0,
    AGENCY_CAPACITY,
    PRINCIPAL_CAPACITY,
    PROPRIETARY_CAPACITY,
    INDIVIDUAL_CAPACITY,
    RISKLESS_PRINCIPAL_CAPACITY,
    AGENT_FOR_ANOTHER_MEMBER_CAPACITY
};

enum TimeInForce
{
    INVALID_TIF = 0,
    DAY_TIF,
    GTD_TIF,
    GTC_TIF,
    FOK_TIF,
    IOC_TIF,
    OPG_TIF,
    ATCLOSE_TIF
};

enum Side
{
    INVALID_SIDE = 0,
    BUY_SIDE,
    SELL_SIDE,
    BUY_MINUS_SIDE,
    SELL_PLUS_SIDE,
    SELL_SHORT_SIDE,
    CROSS_SIDE
};

enum SettlTypeBase
{
    INVALID_SETTLTYPE = 0,
    _0_SETTLTYPE,
    _1_SETTLTYPE,
    _2_SETTLTYPE,
    _3_SETTLTYPE,
    _4_SETTLTYPE,
    _5_SETTLTYPE,
    _6_SETTLTYPE,
    _7_SETTLTYPE,
    _8_SETTLTYPE,
    _9_SETTLTYPE,
    _B_SETTLTYPE,
    _C_SETTLTYPE,
    TENOR_SETTLTYPE
};

enum OrderStatus
{
    INVALID_ORDSTATUS = 0,
    RECEIVEDNEW_ORDSTATUS,
    REJECTED_ORDSTATUS,
    PENDINGNEW_ORDSTATUS,
    PENDINGREPLACE_ORDSTATUS,
    NEW_ORDSTATUS,
    PARTFILL_ORDSTATUS,
    FILLED_ORDSTATUS,
    EXPIRED_ORDSTATUS,
    DFD_ORDSTATUS,
    SUSPENDED_ORDSTATUS,
    REPLACED_ORDSTATUS,
    CANCELED_ORDSTATUS
};

enum Instructions
{
    INVALID_INSTRUCTION = 0,
    STAY_ON_OFFER_SIDE_INSTRUCTION,
    NOT_HELD_INSTRUCTION,
    WORK_INSTRUCTION,
    GO_ALONG_INSTRUCTION,
    _INSTRUCTION // ToDo
};

typedef std::set<Instructions> InstructionSetT;

// Order's parameters, will be stored in persistent storage.
//
// Field layout is organized for cache locality:
//   Hot fields first  — accessed on every order operation (matching, trading)
//   Warm fields next  — accessed during processing but not inner loops
//   Cold fields last  — lazy-loaded reference data, rarely touched after init
struct OrderParams
{
public:
    explicit OrderParams(const SourceIdT &dest, const SourceIdT &instrument, const SourceIdT &account,
                         const SourceIdT &clearing, const SourceIdT &source, const SourceIdT &clOrderId,
                         const SourceIdT &origClOrderID, const SourceIdT &executions);
    OrderParams(const OrderParams &ord);
    ~OrderParams();

    // --- Hot path fields (first cache lines) ---
    /// Per-order mutex protecting all fields after locateByOrderId().
    /// Deadlock convention: when locking two orders simultaneously,
    /// always lock the order with the smaller orderId_ first.
    mutable oneapi::tbb::spin_rw_mutex entryMutex_;

    IdT orderId_;
    IdT origOrderId_;

    PriceT price_; //8
    OrderStatus status_;
    Side side_;
    OrderType ordType_;
    QuantityT leavesQty_;
    QuantityT cumQty_;
    QuantityT orderQty_;
    TimeInForce tif_;
    /// Set while the transaction that books a newly stored order or replacement is not yet enqueued, so that a cancel
    /// or replace of the order waits for it (#83). Store::PublishGuard sets and clears it; a copy starts clear.
    std::atomic<bool> bookingPending_{ false };
    /// Waits until the transaction that books the order is enqueued, if it is not yet (#83). Anything that finds the
    /// order and enqueues a transaction about it calls this first.
    ///
    /// A new order or replacement can be found from the moment it is stored, a little before its creator enqueues the
    /// transaction that matches and books it. A transaction about the order enqueued in that gap would get the lower id
    /// and run first: a cancel would be decided before the order was booked, and the booking transaction would then
    /// match an order with nothing left. TransactionMgr::addTransaction() draws a transaction's id under its lock, and
    /// the creator clears the mark (release) only after that call returns, so a caller that sees the mark clear
    /// (acquire) and then enqueues draws a higher id. No caller waits here while holding a lock, and the creator never
    /// waits for an event handler or a transaction, so this cannot deadlock. The wait is short: the gap holds no I/O,
    /// only the rest of the creator's state machine run and the enqueue.
    void waitUntilBookingEnqueued() const noexcept;
    /// A stop order's stop was reached: a trade in its instrument reached its stop price, and the order was released,
    /// a STOP to trade as a market order and a STOPLIMIT as a limit order (#223). Until then the order is held in its
    /// instrument's stop book. Persisted (OrderCodec format version 2).
    bool stopTriggered_ = false;

    // --- Warm fields (accessed during processing) ---
    PriceT stopPx_;
    PriceT avgPx_;
    PriceT dayAvgPx_;

    DateTimeT creationTime_; //8
    DateTimeT lastUpdateTime_;
    DateTimeT expireTime_;
    DateTimeT settlDate_;
    /// The start of the business day the day figures are for, 17:00 New York (#244, ExpiryScheduler::dayStartOf()).
    /// They are FIX's DayOrderQty, DayCumQty and DayAvgPx: dayOrderQty_ is what the order had left to trade when the
    /// day began, orderQty_ less what it traded before, and dayCumQty_ and dayAvgPx_ are the day's fills. 0 for no
    /// day: an order from before format version 4, whose figures were never kept. Persisted from version 4.
    DateTimeT dayStart_;

    SettlTypeBase settlType_;
    Capacity capacity_;
    Currency currency_;
    QuantityT minQty_; //4
    QuantityT dayOrderQty_;
    QuantityT dayCumQty_;

    OrdState::OrderStatePersistence stateMachinePersistance_;

    // --- FX Swap fields (warm — checked during swap processing only) ---
    PriceT farPrice_;        // forward price for far leg (0.0 if not swap)
    DateTimeT farSettlDate_; // far leg settlement date (0 if not swap)

    // --- Cold fields (lazy-loaded, rarely accessed after init) ---
    WideDataLazyRef<InstrumentEntry> instrument_; //136
    WideDataLazyRef<AccountEntry> account_;       //112
    WideDataLazyRef<ClearingEntry> clearing_;     //72
    WideDataLazyRef<StringT> destination_;        //~32 + 24
    InstructionSetT execInstruct_;                //28

    WideDataLazyRef<RawDataEntry> clOrderId_; //56
    WideDataLazyRef<RawDataEntry> origClOrderId_;
    WideDataLazyRef<StringT> source_;

    WideDataLazyRef<ExecutionsT *> executions_;

    /// Applies a fill in the business day that starts at dayStart (#244): one later than the day figures' begins them
    /// again first, as the day's restatement would have. 0 keeps the figures' day.
    void applyTrade(const TradeExecEntry *trade, DateTimeT dayStart);
    /// Begins the day figures for the business day that starts at dayStart, unless they are for it already or a later
    /// one (#244). Returns whether it began them: a second call for one day keeps that day's fills.
    bool startDay(DateTimeT dayStart) noexcept;
    void addExecution(const IdT &execId);

    /// Where a working order rests (#223): in its instrument's price book, in its stop book, or in neither
    enum class Book
    {
        NONE,  // a market order, a stop order released as one, or an IOC or FOK order: matched once, never rested
        PRICE, // a limit order, or a stop limit order released as one
        STOP   // a stop or stop limit order whose stop hasn't been reached
    };
    /// A stop or stop limit order whose stop hasn't been reached, held until a trade reaches it (#223)
    bool heldStop() const noexcept
    {
        return ((STOP_ORDERTYPE == ordType_) || (STOPLIMIT_ORDERTYPE == ordType_)) && !stopTriggered_;
    }
    /// Matched as a market order: a market order, or a stop order its stop released (#223)
    bool marketLike() const noexcept
    {
        return (MARKET_ORDERTYPE == ordType_) || ((STOP_ORDERTYPE == ordType_) && stopTriggered_);
    }
    /// Matched once and never rested: a market order, a stop order released as one, or an IOC or FOK order, which keep
    /// their limit (#240). What one doesn't trade when it is matched is canceled.
    bool cancelsWhatIsLeft() const noexcept
    {
        return marketLike() || (IOC_TIF == tif_) || (FOK_TIF == tif_);
    }
    Book book() const noexcept
    {
        return heldStop() ? Book::STOP : (cancelsWhatIsLeft() ? Book::NONE : Book::PRICE);
    }

private:
    const OrderParams &operator=(const OrderParams &);
};

struct OrderEntry : public OrderParams
{
public:
    OrderEntry(const SourceIdT &source, const SourceIdT &dest, const SourceIdT &clOrderId,
               const SourceIdT &origClOrderID, const SourceIdT &instrument, const SourceIdT &account,
               const SourceIdT &clearing, const SourceIdT &executions);
    OrderEntry(const OrderEntry &ord);
    ~OrderEntry();

    //Todo: change string reason to error code
    bool isValid(std::string *invalid) const;        // verifies that order is correct
    bool isReplaceValid(std::string *invalid) const; // verifies that order could be replaced

    OrdState::OrderStatePersistence stateMachinePersistance() const;
    void setStateMachinePersistance(const OrdState::OrderStatePersistence &persist);

    OrderEntry *clone() const;
    bool compare(const OrderEntry &val) const;

private:
    const OrderEntry &operator=(const OrderEntry &);
};

class OrderFunctor
{
public:
    virtual ~OrderFunctor() {}
    virtual SourceIdT instrument() const = 0;
    inline Side side() const
    {
        return side_;
    }
    virtual bool match(const IdT &order, bool *stop) const = 0;

protected:
    Side side_;
};

class OrderBook
{
public:
    virtual ~OrderBook() {}

    typedef std::deque<IdT> OrdersT;

    virtual void add(const OrderEntry &order) = 0;
    virtual void remove(const OrderEntry &order) = 0;
    virtual IdT find(const OrderFunctor &functor) const = 0;
    virtual void findAll(const OrderFunctor &functor, OrdersT *result) const = 0;

    virtual IdT getTop(const SourceIdT &instrument, const Side &side) const = 0;

    virtual void restore(const OrderEntry &order) = 0;

    /// The stop book (#223): stop and stop limit orders held until a trade in their instrument reaches their stop
    /// price. A held order is in neither side of the price book, so it is neither matched nor shown.
    virtual void addStop(const OrderEntry &order) = 0;
    virtual void removeStop(const OrderEntry &order) = 0;
    /// Whether the instrument holds a stop: a trade asks this first, and an instrument that holds none takes no lock
    virtual bool holdsStops(const SourceIdT &instrument) const = 0;
    /// The held stops a trade at lastPx in the instrument reaches, in the order they are to be released: buy stops whose
    /// stop price is at or below lastPx, from the lowest, then sell stops at or above it, from the highest, each price's
    /// in arrival order. They stay held: the release takes them out.
    virtual void triggeredStops(const SourceIdT &instrument, PriceT lastPx, OrdersT *result) const = 0;
};

///////////////////////////////////////////////////////////////////////////////////////////
// Incapsulates information about order's executions

/// used when execution created by internal system instead of market
const std::string INTERNAL_EXECUTION = "Internal";

enum ExecLegType : uint8_t
{
    SINGLE_LEG = 0,
    NEAR_LEG,
    FAR_LEG
};

// general parameters of the executions, will be stored in persistent storage
struct ExecParams
{
    // type of the execution
    ExecType type_;
    // creation timestamp
    DateTimeT transactTime_;
    // order's id that changed according execution
    IdT orderId_;
    // id of the execution
    IdT execId_;
    //Describes the current state of order
    OrderStatus orderStatus_;
    // market name, where order was executed. If internal, than equal to INTERNAL_EXECUTION
    StringT market_;
    // which leg this execution belongs to (SINGLE_LEG for non-swap orders)
    ExecLegType execLegType_;
    // A swap fill's link: the same on one side's near and far reports, so a client can pair them, and drawn afresh for
    // each side of each fill (#199). 0 for an execution that isn't a swap leg.
    u64 tradeLink_;

    ExecParams();
    ExecParams(const ExecParams &param);
};

// trade parameters, will be stored in persistent storage
struct ExecTradeParams
{
    // trade's quantity
    QuantityT lastQty_;
    // trade's price
    PriceT lastPx_;
    // trade's currency
    Currency currency_;
    // the business day the trade counts in, 17:00 New York to 17:00 New York, as YYYYMMDD (#258)
    DateTimeT tradeDate_;

    ExecTradeParams();
    ExecTradeParams(const ExecTradeParams &param);
};

// execution correct parameters, will be stored in persistent storage
struct ExecCorrectParams
{
    QuantityT cumQty_;
    QuantityT leavesQty_;
    QuantityT lastQty_;
    PriceT lastPx_;
    Currency currency_;
    DateTimeT tradeDate_;

    // order's id that changed according execution
    IdT origOrderId_;
};

// execution reject parameters, will be stored in persistent storage
struct ExecRejectParams
{
    StringT rejectReason_;
};

struct ExecutionEntry : public ExecParams
{
public:
    ExecutionEntry();
    explicit ExecutionEntry(const ExecParams &param);
    virtual ~ExecutionEntry() {}
    virtual ExecutionEntry *clone() const;
};

struct TradeExecEntry : public ExecutionEntry, public ExecTradeParams
{
    TradeExecEntry();
    TradeExecEntry(const ExecutionEntry &exec, const ExecTradeParams &trade);
    TradeExecEntry(const TradeExecEntry &params);
    virtual ExecutionEntry *clone() const;
};

struct RejectExecEntry : public ExecutionEntry, public ExecRejectParams
{
    virtual ExecutionEntry *clone() const;
};

struct ExecCorrectExecEntry : public ExecutionEntry, public ExecCorrectParams
{
    IdT execRefId_;

    virtual ExecutionEntry *clone() const;
};

struct ReplaceExecEntry : public ExecutionEntry
{
    IdT origOrderId_;

    virtual ExecutionEntry *clone() const;
};

struct TradeCancelExecEntry : public ExecutionEntry
{
    IdT execRefId_;

    virtual ExecutionEntry *clone() const;
};

class OrderSaver
{
public:
    virtual ~OrderSaver() {};
    /// Persists the order as a new version of its record and returns that version.
    virtual u32 save(const OrderEntry &order) = 0;
    /// Erases one persisted version, undoing a save().
    virtual void erase(const IdT &orderId, u32 version) = 0;
    /// The saves a thread makes between beginGroup() and commitGroup() become durable together, or none of them do
    /// (#163). A saver that can't group writes each save at once, and these do nothing.
    virtual void beginGroup() {}
    virtual void commitGroup() {}
    virtual void abortGroup() noexcept {}
    /// Persists an instrument's last trade, its price and its execution id, as a new version of the instrument's
    /// last-trade record, and returns that version (#237). A held stop that a trade reached is released in a transaction
    /// of its own, after the trade's: a crash between the two leaves the stop held, and the restart releases it from
    /// this record. A saver that keeps none writes nothing.
    virtual u32 saveLastTrade(const SourceIdT & /*instrument*/, PriceT /*lastPx*/, const IdT & /*execId*/)
    {
        return 0;
    }
    /// Erases one version of it, undoing a saveLastTrade()
    virtual void eraseLastTrade(const SourceIdT & /*instrument*/, u32 /*version*/) {}
};

} // namespace COP
