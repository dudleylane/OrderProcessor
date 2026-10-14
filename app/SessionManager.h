#pragma once

#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include <string>
#include <oneapi/tbb/spin_rw_mutex.h>

namespace COP::App
{

class WsSession;

class SessionManager
{
public:
    void addSession(std::shared_ptr<WsSession> session);
    /// Ends its book subscriptions too
    void removeSession(std::shared_ptr<WsSession> session);
    void broadcast(const std::string &json);
    void broadcastBookUpdate(const std::string &symbol, const std::string &json);
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
