#include "SessionManager.h"
#include "WsSession.h"

#include <algorithm>
#include <boost/asio/post.hpp>

using namespace COP::App;

void SessionManager::addSession(std::shared_ptr<WsSession> session)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
    sessions_.push_back(std::move(session));
}

void SessionManager::removeSession(std::shared_ptr<WsSession> session)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
    sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), session), sessions_.end());
    for (auto it = subscribers_.begin(); it != subscribers_.end();)
    {
        it->second.erase(session);
        it = it->second.empty() ? subscribers_.erase(it) : std::next(it);
    }
}

void SessionManager::subscribe(const std::shared_ptr<WsSession> &session, const std::string &symbol)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
    subscribers_[symbol].insert(session);
}

void SessionManager::unsubscribe(const std::shared_ptr<WsSession> &session, const std::string &symbol)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, true);
    auto it = subscribers_.find(symbol);
    if (it == subscribers_.end())
    {
        return;
    }
    it->second.erase(session);
    if (it->second.empty())
    {
        subscribers_.erase(it);
    }
}

void SessionManager::broadcast(const std::string &json)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
    if (sessions_.empty())
    {
        return;
    }
    // One copy, which every session's queue shares: each used to get its own (#159)
    auto msg = std::make_shared<const std::string>(json);
    for (auto &session : sessions_)
    {
        boost::asio::post(session->executor(),
                          [session, msg]()
                          {
                              session->send(msg);
                          });
    }
}

void SessionManager::broadcastToAccount(const SourceIdT &account, const std::string &json)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
    std::shared_ptr<const std::string> msg; // made for the first session that gets it, and shared (#159)
    for (auto &session : sessions_)
    {
        const WsIdentity *identity = session->identity();
        if ((nullptr != identity) && !identity->mayUse(account))
        {
            continue;
        }
        if (!msg)
        {
            msg = std::make_shared<const std::string>(json);
        }
        boost::asio::post(session->executor(),
                          [session, msg]()
                          {
                              session->send(msg);
                          });
    }
}

void SessionManager::sendToSource(const std::string &source, const std::string &json)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
    std::shared_ptr<const std::string> msg;
    for (auto &session : sessions_)
    {
        const WsIdentity *identity = session->identity();
        if ((nullptr != identity) && (identity->name != source))
        {
            continue;
        }
        if (!msg)
        {
            msg = std::make_shared<const std::string>(json);
        }
        boost::asio::post(session->executor(),
                          [session, msg]()
                          {
                              session->send(msg);
                          });
    }
}

size_t SessionManager::sessionCount() const
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
    return sessions_.size();
}

void SessionManager::broadcastBookUpdate(const std::string &symbol, const std::string &json)
{
    oneapi::tbb::spin_rw_mutex::scoped_lock lock(rwLock_, false);
    if (subscribers_.empty())
    {
        return; // no client has a book open: skip hashing the symbol
    }
    auto subscribed = subscribers_.find(symbol);
    if (subscribed == subscribers_.end())
    {
        return;
    }
    // One copy, which every subscriber's queue shares (#159)
    auto msg = std::make_shared<const std::string>(json);
    for (const auto &session : subscribed->second)
    {
        boost::asio::post(session->executor(),
                          [session, msg]()
                          {
                              session->send(msg);
                          });
    }
}
