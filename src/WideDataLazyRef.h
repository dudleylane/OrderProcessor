/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <mutex>
#include "TypesDef.h"
#include "WideDataStorage.h"

namespace COP
{

template <typename T> class WideDataLazyRef
{
public:
    WideDataLazyRef(const SourceIdT &id, bool load = false) : id_(id), loaded_(false)
    {
        if (load)
        {
            this->load();
        }
    }

    /// A copy takes the value only once it sees it loaded (#455). get() and load() write the value under the mutex, then
    /// mark it loaded with a release store, so a copy that read the value before looking raced with another thread's
    /// first load. A copy of a ref not yet loaded makes its own first load. loaded_ is read once, for both members: read
    /// twice, a load between the reads would leave the copy marked loaded with no value.
    WideDataLazyRef(const WideDataLazyRef &other)
        : WideDataLazyRef(other, other.loaded_.load(std::memory_order_acquire))
    {
    }

    WideDataLazyRef &operator=(const WideDataLazyRef &other)
    {
        if (this != &other)
        {
            const bool loaded = other.loaded_.load(std::memory_order_acquire); // before the value, as a copy looks
            val_ = loaded ? other.val_ : T{};
            id_ = other.id_;
            loaded_.store(loaded, std::memory_order_release);
        }
        return *this;
    }

    ~WideDataLazyRef(void) {}

    const T &get() const
    {
        if (!loaded_.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> guard(mtx_);
            if (!loaded_.load(std::memory_order_relaxed))
            {
                Store::WideDataStorage::instance()->get(id_, &val_);
                loaded_.store(true, std::memory_order_release);
            }
        }
        return val_;
    }

    void load()
    {
        if (!loaded_.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> guard(mtx_);
            if (!loaded_.load(std::memory_order_relaxed))
            {
                Store::WideDataStorage::instance()->get(id_, &val_);
                loaded_.store(true, std::memory_order_release);
            }
        }
    }

    const SourceIdT &getId() const
    {
        return id_;
    };

private:
    WideDataLazyRef(const WideDataLazyRef &other, bool loaded)
        : val_(loaded ? other.val_ : T{}), id_(other.id_), loaded_(loaded)
    {
    }

    mutable T val_{};
    SourceIdT id_;
    mutable std::atomic<bool> loaded_;
    mutable std::mutex mtx_;
};

} // namespace COP