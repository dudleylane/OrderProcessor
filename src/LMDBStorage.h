/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <lmdb.h>

#include "IdTGenerator.h"
#include "FileStorageDef.h"

namespace COP
{
namespace Store
{

class LMDBStorage final : public FileSaver
{
public:
    /// The map LMDB starts with unless asked for another (#148). A write that finds the map full doubles it.
    static constexpr size_t DEFAULT_MAP_SIZE = 256UL * 1024UL * 1024UL;

    LMDBStorage();
    /// mapSize: the map to start with. A directory that holds more opens with what it holds.
    explicit LMDBStorage(size_t mapSize);
    LMDBStorage(const std::string &path, FileStorageObserver *observer, size_t mapSize = DEFAULT_MAP_SIZE);
    ~LMDBStorage();

public:
    /// reimplemented from FileSaver
    void load(const std::string &path, FileStorageObserver *observer) override;
    IdT save(const char *buf, size_t size) override;
    void save(const IdT &id, const char *buf, size_t size) override;
    u32 update(const IdT &id, const char *buf, size_t size) override;
    u32 replace(const IdT &id, u32 version, const char *buf, size_t size) override;
    void erase(const IdT &id, u32 version) override;
    void erase(const IdT &id) override;
    /// A thread's group (#163): between beginGroup() and commitGroup(), update() works out each record's new version and
    /// holds it, and commitGroup() writes them all in one transaction. The versions update() returns are the ones
    /// written. A record may be updated once in a group: a transaction persists an order once.
    void beginGroup() override;
    void commitGroup() override;
    void abortGroup() noexcept override;

public:
    /// return true if record exists
    bool isExists(const IdT &id) const;
    /// return true if version of record exists
    bool isExists(const IdT &id, u32 version) const;
    /// returns latest version of the record
    u32 getTopVersion(const IdT &id) const;
    /// returns size of the record with version
    size_t recordSize(const IdT &id, u32 version) const;
    /// loads record from file into the buf
    /// return false if version of record not exists
    bool loadRecord(const IdT &id, u32 version, std::vector<char> *buf);

    /// The map's size, and how much of it is in use (the high-water mark of its pages), as of the last commit (#148).
    /// Safe from any thread.
    size_t mapSize() const noexcept;
    size_t mapUsed() const noexcept;
    /// Write transactions committed since the storage was made (#163)
    u64 commits() const noexcept;

private:
    struct CompositeKey
    {
        IdT id_;
        u32 version_;

        CompositeKey()
        {
            std::memset(this, 0, sizeof(CompositeKey));
        }
        CompositeKey(const IdT &id, u32 version)
        {
            std::memset(this, 0, sizeof(CompositeKey));
            id_ = id;
            version_ = version;
        }
    };

    /// One record's new version, held until its group commits (#163)
    struct StagedUpdate
    {
        IdT id_;
        u32 version_;
        std::vector<char> data_;
    };

    static MDB_val makeKey(const CompositeKey &key);
    static CompositeKey readKey(const MDB_val &val);
    /// id's newest version, in any transaction; *found is false when it has none
    int newestVersion(MDB_txn *txn, const IdT &id, bool *found, u32 *newest) const;
    /// update()'s work in a write transaction: the version after the newest, with every version but the newest erased
    /// (#148). When staged, *version is the one a group's update() returned, which must still be the next.
    int writeVersion(MDB_txn *txn, const IdT &id, const char *buf, size_t size, u32 *version, bool staged);
    /// update() within this thread's group: works the version out now, and holds the record for commitGroup()
    u32 stage(const IdT &id, const char *buf, size_t size);

    void close();

    /// Runs body(txn), which returns an LMDB result, in a write transaction and commits it. A write that finds the map
    /// full doubles the map and runs again (#148).
    template <typename Body> void write(const char *context, Body &&body);
    /// doubles the map; txnLock_ held, and no transaction open
    void grow(const char *context);
    /// records the map's size and use for mapSize() and mapUsed()
    void noteUsage();

    LMDBStorage(const LMDBStorage &) = delete;
    LMDBStorage &operator=(const LMDBStorage &) = delete;

private:
    MDB_env *env_;
    MDB_dbi dbi_;
    bool open_;
    IdTValueGenerator generator_;
    size_t initialMapSize_;
    size_t pageSize_;
    std::atomic<size_t> mapSize_;
    std::atomic<size_t> mapUsed_;
    /// Held around every transaction. LMDB serializes writers itself, but it lets the map's size change only while no
    /// transaction is open in the process (#148), and only a lock of the storage's own can promise that.
    mutable std::mutex txnLock_;
    std::atomic<u64> commits_{ 0 };

    /// This thread's open group (#163): the storage it belongs to, and the records it holds
    static thread_local LMDBStorage *groupOwner_;
    static thread_local std::vector<StagedUpdate> group_;
};

} // namespace Store
} // namespace COP
