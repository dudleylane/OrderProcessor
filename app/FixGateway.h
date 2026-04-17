#pragma once

#ifdef BUILD_FIX

// QuickFIX's Config23.h polyfills std::flat_map as boost::container::flat_map
// on GCC. This header MUST be included BEFORE any header that includes
// <flat_map> (e.g. NLinkedTree.h via TransactionMgr.h). NLinkedTree.h
// conditionally skips #include <flat_map> when the polyfill is active.
#include <quickfix/Application.h>
#include <unordered_map>
#include <oneapi/tbb/spin_rw_mutex.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/SessionID.h>
#include <quickfix/Session.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/NewOrderMultileg.h>
#include <quickfix/fix44/OrderCancelRequest.h>
#include <quickfix/fix44/OrderCancelReplaceRequest.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/OrderCancelReject.h>
#include <quickfix/fix44/BusinessMessageReject.h>
#include "QueuesDef.h"
#include "DataModelDef.h"

namespace COP {

namespace Store {
    class WideParamsDataStorage;
    class OrderDataStorage;
}

namespace App {

class FixGateway : public FIX::Application,
                   public FIX44::MessageCracker {
public:
    FixGateway(Queues::InQueues* inQueues,
               Store::WideParamsDataStorage* wideData,
               Store::OrderDataStorage* orderStorage,
               SourceIdT defaultClearingId = SourceIdT());

    // Application interface
    void onCreate(const FIX::SessionID&) override;
    void onLogon(const FIX::SessionID&) override;
    void onLogout(const FIX::SessionID&) override;
    void toAdmin(FIX::Message&, const FIX::SessionID&) override;
    void toApp(FIX::Message&, const FIX::SessionID&) override;
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override;
    void fromApp(const FIX::Message&, const FIX::SessionID&) override;

    // MessageCracker overrides (inbound)
    void onMessage(const FIX44::NewOrderSingle&, const FIX::SessionID&);
    void onMessage(const FIX44::NewOrderMultileg&, const FIX::SessionID&);
    void onMessage(const FIX44::OrderCancelRequest&, const FIX::SessionID&);
    void onMessage(const FIX44::OrderCancelReplaceRequest&, const FIX::SessionID&);

    // Outbound — called by FixOutQueues
    void sendExecutionReport(const ExecutionEntry* exec, const OrderEntry& order);
    void sendCancelReject(const IdT& orderId, const std::string& clOrdId);
    void sendBusinessReject(const IdT& refOrderId, const std::string& reason);

    // Session lookup
    bool hasFixSession(const std::string& source) const;

    // Build source string from SessionID
    static std::string makeSourceString(const FIX::SessionID& sid);

private:
    Queues::InQueues* inQueues_;
    Store::WideParamsDataStorage* wideData_;
    Store::OrderDataStorage* orderStorage_;

    std::unordered_map<std::string, FIX::SessionID> sessionMap_;
    mutable oneapi::tbb::spin_rw_mutex sessionMapLock_;

    SourceIdT defaultClearingId_;  // pre-resolved clearing for FIX orders

public:
    // FIX → internal enum conversions (public static for testability)
    static Side toSide(char fixSide);
    static OrderType toOrdType(char fixOrdType);
    static TimeInForce toTif(char fixTif);
    static Currency toCurrency(const std::string& fixCcy);

    // Internal → FIX enum conversions
    static char fromOrdStatus(OrderStatus s);
    static char fromExecType(ExecType t);
    static char fromSide(Side s);
};

}} // namespace COP::App

#endif // BUILD_FIX
