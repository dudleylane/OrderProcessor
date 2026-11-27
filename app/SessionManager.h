#pragma once

#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include <string>
#include <string_view>
#include <oneapi/tbb/spin_rw_mutex.h>

#include "TypesDef.h"

namespace COP::App
{

class WsSession;

class SessionManager
{
public:
    void addSession(std::shared_ptr<WsSession> session);
    /// Ends its book subscriptions too
    void removeSession(std::shared_ptr<WsSession> session);
    /// To every session: what any session may see, such as the book or the metrics
    void broadcast(const std::string &json);
    /// To every session that may see the account's orders: one without an identity, or one whose identity has the
    /// account (#225). An order's reports and updates go this way, so another identity's session never gets them.
    void broadcastToAccount(const SourceIdT &account, const std::string &json);
    /// To the sessions of the identity named source, and to every session without an identity: the answer to a request
    /// that names no order a session may see, routed by the source the request came in under (#225)
    void sendToSource(const std::string &source, const std::string &json);
    /// broadcastToAccount() for an order's order_update: the session that took the order's request names the request on
    /// the order's first update, when the request named itself (#790). json is moved into the copy the sessions share,
    /// with where it holds clOrdId, found once here, so the sessions get the ClOrdID with no copy of it.
    void broadcastOrderUpdate(const SourceIdT &account, std::string json, std::string_view clOrdId);
    /// sendToSource() for the REJECT of an order the engine refused without storing it, which names its request
    /// already (#683): each session it reaches forgets the request it kept for the order's ClOrdID, if it kept one
    /// (#790). json is moved, as broadcastOrderUpdate()'s is.
    void sendUnstoredReject(const std::string &source, std::string json, std::string_view clOrdId);
    void broadcastBookUpdate(const std::string &symbol, const std::string &json);
    /// Whether any session subscribes to the symbol's book: a report builds the book's depth only then (#525)
    bool hasBookSubscribers(const std::string &symbol) const;
    /// A session's book subscriptions are kept here, under the lock the broadcasts take: the session's own thread
    /// changes them, and broadcastBookUpdate reads them on any thread, a transaction worker's through WsOutQueues. Each
    /// session used to keep its own, read without a lock: a data race (#159).
    void subscribe(const std::shared_ptr<WsSession> &session, const std::string &symbol);
    void unsubscribe(const std::shared_ptr<WsSession> &session, const std::string &symbol);
    size_t sessionCount() const;

private:
    mutable oneapi::tbb::spin_rw_mutex rwLock_; // guards sessions_ and subscribers_
    std::vector<std::shared_ptr<WsSession>> sessions_;
    /// The sessions subscribed to each symbol's book: a book update finds them at once, however many sessions there are
    std::unordered_map<std::string, std::set<std::shared_ptr<WsSession>>> subscribers_;
};

} // namespace COP::App
