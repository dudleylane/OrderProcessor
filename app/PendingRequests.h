#pragma once

#include <cstddef>
#include <iterator>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace COP
{
namespace App
{

/// The new orders a session took whose requests named themselves (#683): each request's id, kept by the ClOrdID the
/// session made for its order, until the order's first order_update to the session names the request (#790). An order
/// the engine refuses unstored is forgotten as its REJECT goes out, which names the request already. One whose first
/// update never comes, as a failed transaction can leave one (#514), would be kept for good: so at most capacity are
/// kept, and the oldest goes first, counted. Only the session's strand uses it.
class PendingRequests
{
public:
    explicit PendingRequests(std::size_t capacity) : capacity_(capacity) {}

    /// Keeps the request's id by its order's ClOrdID, which the session made and no other order has, dropping the
    /// oldest kept when capacity are
    void remember(std::string clOrdId, std::string requestId)
    {
        if ((0 == capacity_) || index_.contains(clOrdId))
        {
            return;
        }
        if (index_.size() >= capacity_)
        {
            index_.erase(entries_.front().first);
            entries_.pop_front();
            ++dropped_;
        }
        entries_.emplace_back(std::move(clOrdId), std::move(requestId));
        index_.emplace(entries_.back().first, std::prev(entries_.end()));
    }

    /// The id of the request kept by the order's ClOrdID, forgotten as it is taken; none when none is kept by it
    std::optional<std::string> take(std::string_view clOrdId)
    {
        const auto found = index_.find(clOrdId);
        if (index_.end() == found)
        {
            return std::nullopt;
        }
        const auto entry = found->second;
        index_.erase(found); // first: its key is a view of the entry's ClOrdID
        std::optional<std::string> requestId(std::move(entry->second));
        entries_.erase(entry);
        return requestId;
    }

    /// Forgets the request kept by the order's ClOrdID, if one is
    void forget(std::string_view clOrdId)
    {
        take(clOrdId);
    }

    bool empty() const
    {
        return index_.empty();
    }
    std::size_t size() const
    {
        return index_.size();
    }
    /// How many were dropped, the oldest first, to keep within the capacity
    std::size_t dropped() const
    {
        return dropped_;
    }

private:
    /// Each order's ClOrdID and its request's id, the oldest first: a list, so that each key's view stays valid
    using Entries = std::list<std::pair<std::string, std::string>>;

    std::size_t capacity_;
    Entries entries_;
    std::unordered_map<std::string_view, Entries::iterator> index_;
    std::size_t dropped_ = 0;
};

} // namespace App
} // namespace COP
