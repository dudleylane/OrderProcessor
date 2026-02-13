#pragma once

#include "QueuesDef.h"

namespace COP {
    class OrderBookImpl;

namespace Store {
    class WideParamsDataStorage;
    class OrderDataStorage;
}

namespace App {

class SessionManager;

class WsOutQueues : public Queues::OutQueues {
public:
    WsOutQueues(SessionManager* sessionMgr,
                Store::WideParamsDataStorage* wideData,
                Store::OrderDataStorage* orderStorage,
                OrderBookImpl* orderBook);

    void push(const Queues::ExecReportEvent& evnt, const std::string& target) override;
    void push(const Queues::CancelRejectEvent& evnt, const std::string& target) override;
    void push(const Queues::BusinessRejectEvent& evnt, const std::string& target) override;

private:
    SessionManager* sessionMgr_;
    Store::WideParamsDataStorage* wideData_;
    Store::OrderDataStorage* orderStorage_;
    OrderBookImpl* orderBook_;
};

}}
