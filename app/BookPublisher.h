#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "TypesDef.h"

namespace COP
{
class OrderBookImpl;
namespace Store
{
class OrderDataStorage;
}
namespace App
{

class SessionManager;

/// How often, at most, a book's depth goes to the sessions that subscribe to it (#694)
inline constexpr std::chrono::milliseconds BOOK_UPDATE_INTERVAL{ 100 };

/// Sends the depth of each book that reports changed to the sessions that subscribe to it, from the IO thread, at most
/// once an interval (#694). Until #694 a transaction worker built the depth for every report it pushed on a watched
/// instrument, walking the whole book each time, and a sweep of N resting orders walked it 2N times.
///
/// A report marks its instrument changed on the worker that pushes it (changed()). The first mark after a send arms a
/// timer on the IO thread, and when it fires the IO thread builds each marked book that still has a subscriber, as #525
/// builds it, and sends it. So a book goes out once, an interval after it first changed, with its state then: never
/// between a trade's two reports, which are pushed back to back, where it would show the trade half applied.
///
/// A later build is safe because every change that moves the depth is written before its report's push: a new order
/// and a replacement are booked by their transition's action and reported by the state's entry, which MSM runs after
/// it; a released stop-limit is booked inside its release, ahead of its TRIGGERED report; and a fill's or a closing's
/// figures are published before the push, while the depth leaves out an order whose published leaves are 0, so a
/// closed order's removal, which runs later, changes nothing it shows.
class BookPublisher : public std::enable_shared_from_this<BookPublisher>
{
public:
    /// Whether a symbol's book has a subscriber, and the send of a built book's JSON to its subscribers: the
    /// SessionManager's, which a test replaces
    using Watched = std::function<bool(const std::string &symbol)>;
    using Send = std::function<void(const std::string &symbol, const std::string &json)>;

    /// Made only as a shared object: what it posts to the IO thread holds a weak reference, so a publisher freed with
    /// its WsOutQueues leaves nothing to run, and one that no shared_ptr owned would never send
    static std::shared_ptr<BookPublisher> create(boost::asio::io_context &ioc, SessionManager *sessionMgr,
                                                 const OrderBookImpl *book, const Store::OrderDataStorage *orderStorage,
                                                 std::chrono::milliseconds interval = BOOK_UPDATE_INTERVAL);
    static std::shared_ptr<BookPublisher> create(boost::asio::io_context &ioc, Watched watched, Send send,
                                                 const OrderBookImpl *book, const Store::OrderDataStorage *orderStorage,
                                                 std::chrono::milliseconds interval = BOOK_UPDATE_INTERVAL);

    /// From any thread, once the report about the instrument has its figures published: its book has changed
    void changed(const SourceIdT &instrument, const std::string &symbol);
    /// Sends nothing more: a change after it is dropped, and a send that is due does nothing. Called on the IO thread,
    /// or once no thread runs the context, as the server calls it when its IO thread has returned; the transaction
    /// workers may still report changes after it.
    void stop();

private:
    BookPublisher(boost::asio::io_context &ioc, Watched watched, Send send, const OrderBookImpl *book,
                  const Store::OrderDataStorage *orderStorage, std::chrono::milliseconds interval);

    /// The IO thread's: arms the timer for the books changed since the last send
    void arm();
    /// The IO thread's: builds and sends each changed book that has a subscriber
    void send();

    boost::asio::io_context &ioc_;
    /// Touched on the IO thread only
    boost::asio::steady_timer timer_;
    const std::chrono::milliseconds interval_;
    Watched watched_;
    Send send_;
    const OrderBookImpl *book_;
    const Store::OrderDataStorage *orderStorage_;

    struct Book
    {
        std::string symbol;
        bool changed = false;
    };
    /// A leaf in the lock order (docs/ARCHITECTURE.md, section 6.2): held only to mark a book or to take the marked
    /// ones, never while a book is built, its subscribers asked, its JSON sent, or anything posted
    std::mutex lock_;
    /// Every book a report has marked, kept, so that a mark allocates only for a book's first: guarded by lock_
    std::map<SourceIdT, Book> books_;
    /// A send is due, so a mark arms nothing: guarded by lock_
    bool armed_ = false;
    /// Guarded by lock_
    bool stopped_ = false;
    /// The IO thread's: the books a send takes, kept from one send to the next
    std::vector<std::pair<SourceIdT, std::string>> due_;
};

} // namespace App
} // namespace COP
