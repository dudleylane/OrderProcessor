#include "BookPublisher.h"

#include <boost/asio/post.hpp>

#include "JsonSerializer.h"
#include "OrderBookImpl.h"
#include "SessionManager.h"

using namespace COP;
using namespace COP::App;

std::shared_ptr<BookPublisher> BookPublisher::create(boost::asio::io_context &ioc, SessionManager *sessionMgr,
                                                     const OrderBookImpl *book,
                                                     const Store::OrderDataStorage *orderStorage,
                                                     std::chrono::milliseconds interval)
{
    return create(
        ioc,
        [sessionMgr](const std::string &symbol)
        {
            return sessionMgr->hasBookSubscribers(symbol);
        },
        [sessionMgr](const std::string &symbol, const std::string &json)
        {
            sessionMgr->broadcastBookUpdate(symbol, json);
        },
        book, orderStorage, interval);
}

std::shared_ptr<BookPublisher> BookPublisher::create(boost::asio::io_context &ioc, Watched watched, Send send,
                                                     const OrderBookImpl *book,
                                                     const Store::OrderDataStorage *orderStorage,
                                                     std::chrono::milliseconds interval)
{
    return std::shared_ptr<BookPublisher>(
        new BookPublisher(ioc, std::move(watched), std::move(send), book, orderStorage, interval));
}

BookPublisher::BookPublisher(boost::asio::io_context &ioc, Watched watched, Send send, const OrderBookImpl *book,
                             const Store::OrderDataStorage *orderStorage, std::chrono::milliseconds interval)
    : ioc_(ioc), timer_(ioc), interval_(interval), watched_(std::move(watched)), send_(std::move(send)), book_(book),
      orderStorage_(orderStorage)
{
}

void BookPublisher::changed(const SourceIdT &instrument, const std::string &symbol)
{
    bool arm = false;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (stopped_)
        {
            return;
        }
        auto it = books_.find(instrument);
        if (books_.end() == it)
        {
            it = books_.emplace(instrument, Book{ symbol }).first;
        }
        it->second.changed = true;
        arm = !armed_;
        armed_ = true;
    }
    // Posted with the lock released: the context takes its own lock to queue it
    if (arm)
    {
        boost::asio::post(ioc_,
                          [weak = weak_from_this()]
                          {
                              if (std::shared_ptr<BookPublisher> self = weak.lock())
                              {
                                  self->arm();
                              }
                          });
    }
}

void BookPublisher::stop()
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        stopped_ = true;
    }
    timer_.cancel();
}

void BookPublisher::arm()
{
    timer_.expires_after(interval_);
    timer_.async_wait(
        [weak = weak_from_this()](boost::system::error_code ec)
        {
            if (ec)
            {
                return;
            }
            if (std::shared_ptr<BookPublisher> self = weak.lock())
            {
                self->send();
            }
        });
}

void BookPublisher::send()
{
    due_.clear();
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (stopped_)
        {
            return;
        }
        // From here a mark arms the next send. Its arming is posted to this thread, so it waits for this send, and the
        // next is an interval after it at the earliest.
        armed_ = false;
        for (auto &[instrument, book] : books_)
        {
            if (book.changed)
            {
                book.changed = false;
                due_.emplace_back(instrument, book.symbol);
            }
        }
    }
    // The books are built with no lock of ours held, each as #525 builds it: a side's lock, released, and then each
    // order's under its own, never both
    for (const auto &[instrument, symbol] : due_)
    {
        // Its last subscriber may have gone since the mark: nothing to build
        if (!watched_(symbol))
        {
            continue;
        }
        send_(symbol, serializeBookUpdate(symbol, book_->getSnapshot(instrument, orderStorage_)));
    }
}
