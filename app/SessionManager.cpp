#include "SessionManager.h"
#include "WsSession.h"

#include <algorithm>
#include <boost/asio/post.hpp>

using namespace COP::App;

void SessionManager::addSession(std::shared_ptr<WsSession> session) {
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
    sessions_.push_back(std::move(session));
}

void SessionManager::removeSession(std::shared_ptr<WsSession> session) {
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
    sessions_.erase(
        std::remove(sessions_.begin(), sessions_.end(), session),
        sessions_.end());
}

void SessionManager::broadcast(const std::string& json) {
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
    for (auto& session : sessions_) {
        auto msg = std::make_shared<std::string>(json);
        boost::asio::post(session->executor(), [session, msg]() {
            session->send(*msg);
        });
    }
}

void SessionManager::broadcastBookUpdate(const std::string& symbol, const std::string& json) {
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
    for (auto& session : sessions_) {
        if (!session->isSubscribedTo(symbol))
            continue;
        auto msg = std::make_shared<std::string>(json);
        boost::asio::post(session->executor(), [session, msg]() {
            session->send(*msg);
        });
    }
}
