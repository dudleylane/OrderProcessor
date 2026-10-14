/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include <cassert>
#include <sys/stat.h>

#include "LMDBStorage.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Store;

thread_local LMDBStorage *LMDBStorage::groupOwner_ = nullptr;
thread_local std::vector<LMDBStorage::StagedUpdate> LMDBStorage::group_;

namespace
{

void checkLMDB(int rc, const char *context)
{
    if (rc != MDB_SUCCESS)
    {
        std::string msg = std::string(context) + ": " + mdb_strerror(rc);
        throw std::runtime_error(msg);
    }
}

/// Aborts a write transaction that is still open when it goes out of scope, as when its body throws
class TxnGuard
{
public:
    explicit TxnGuard(MDB_txn *txn) : txn_(txn) {}
    ~TxnGuard()
    {
        if (nullptr != txn_)
        {
            mdb_txn_abort(txn_);
        }
    }

    /// LMDB frees the transaction whatever the commit returns
    int commit()
    {
        MDB_txn *txn = txn_;
        txn_ = nullptr;
        return mdb_txn_commit(txn);
    }

    TxnGuard(const TxnGuard &) = delete;
    TxnGuard &operator=(const TxnGuard &) = delete;

private:
    MDB_txn *txn_;
};

} // namespace

MDB_val LMDBStorage::makeKey(const CompositeKey &key)
{
    MDB_val val;
    val.mv_size = sizeof(CompositeKey);
    val.mv_data = const_cast<CompositeKey *>(&key);
    return val;
}

LMDBStorage::CompositeKey LMDBStorage::readKey(const MDB_val &val)
{
    assert(val.mv_size == sizeof(CompositeKey));
    CompositeKey key;
    std::memcpy(&key, val.mv_data, sizeof(CompositeKey));
    return key;
}

LMDBStorage::LMDBStorage() : LMDBStorage(DEFAULT_MAP_SIZE) {}

LMDBStorage::LMDBStorage(size_t mapSize)
    : env_(nullptr), dbi_(0), open_(false), generator_(), initialMapSize_(mapSize), pageSize_(0), mapSize_(0),
      mapUsed_(0)
{
}

LMDBStorage::LMDBStorage(const std::string &path, FileStorageObserver *observer, size_t mapSize) : LMDBStorage(mapSize)
{
    load(path, observer);
}

LMDBStorage::~LMDBStorage()
{
    close();
}

void LMDBStorage::close()
{
    if (open_)
    {
        mdb_dbi_close(env_, dbi_);
        open_ = false;
    }
    if (env_)
    {
        mdb_env_close(env_);
        env_ = nullptr;
    }
}

void LMDBStorage::load(const std::string &path, FileStorageObserver *observer)
{
    // Runs before anything else uses the storage, so it takes no lock: the observer may not write back during the load
    assert(nullptr == env_);
    assert(nullptr != observer);

    // Ensure the directory exists
    mkdir(path.c_str(), 0755);

    int rc = mdb_env_create(&env_);
    checkLMDB(rc, "LMDBStorage::load mdb_env_create");

    // A directory that already holds more than this opens with what it holds: LMDB never maps less than its data
    rc = mdb_env_set_mapsize(env_, initialMapSize_);
    checkLMDB(rc, "LMDBStorage::load mdb_env_set_mapsize");

    rc = mdb_env_open(env_, path.c_str(), 0, 0664);
    if (rc != MDB_SUCCESS)
    {
        mdb_env_close(env_);
        env_ = nullptr;
        checkLMDB(rc, "LMDBStorage::load mdb_env_open");
    }

    // Open the default (unnamed) database
    MDB_txn *txn = nullptr;
    rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != MDB_SUCCESS)
    {
        mdb_env_close(env_);
        env_ = nullptr;
        checkLMDB(rc, "LMDBStorage::load mdb_txn_begin");
    }

    rc = mdb_dbi_open(txn, nullptr, MDB_CREATE, &dbi_);
    if (rc != MDB_SUCCESS)
    {
        mdb_txn_abort(txn);
        mdb_env_close(env_);
        env_ = nullptr;
        checkLMDB(rc, "LMDBStorage::load mdb_dbi_open");
    }

    // Scan all existing records and notify observer. A record the observer refuses throws out of the scan: close the
    // cursor, the transaction and the environment first, as the failures above do. Left open, the cursor leaked and the
    // destructor closed the environment under an open write transaction (#216).
    MDB_cursor *cursor = nullptr;
    try
    {
        observer->startLoad();
        rc = mdb_cursor_open(txn, dbi_, &cursor);
        if (rc == MDB_SUCCESS)
        {
            MDB_val key, data;
            while (mdb_cursor_get(cursor, &key, &data, MDB_NEXT) == MDB_SUCCESS)
            {
                if (key.mv_size == sizeof(CompositeKey))
                {
                    CompositeKey ck = readKey(key);
                    observer->onRecordLoaded(ck.id_, ck.version_, static_cast<const char *>(data.mv_data),
                                             data.mv_size);
                }
            }
        }
    }
    catch (...)
    {
        if (nullptr != cursor)
        {
            mdb_cursor_close(cursor);
        }
        mdb_txn_abort(txn);
        mdb_env_close(env_);
        env_ = nullptr;
        throw;
    }
    if (nullptr != cursor)
    {
        mdb_cursor_close(cursor);
    }

    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS)
    {
        mdb_env_close(env_);
        env_ = nullptr;
        checkLMDB(rc, "LMDBStorage::load mdb_txn_commit");
    }

    MDB_stat stat;
    checkLMDB(mdb_env_stat(env_, &stat), "LMDBStorage::load mdb_env_stat");
    pageSize_ = stat.ms_psize;
    noteUsage();

    open_ = true;
    observer->finishLoad();
}

template <typename Body> void LMDBStorage::write(const char *context, Body &&body)
{
    assert(nullptr != env_);
    std::lock_guard<std::mutex> lock(txnLock_);
    for (;;)
    {
        MDB_txn *txn = nullptr;
        checkLMDB(mdb_txn_begin(env_, nullptr, 0, &txn), context);
        int rc = MDB_SUCCESS;
        {
            TxnGuard guard(txn);
            rc = body(txn);
            if (MDB_SUCCESS == rc)
            {
                rc = guard.commit();
            }
        } // the guard aborts a transaction that didn't commit, so none is open from here on
        if (MDB_MAP_FULL != rc)
        {
            checkLMDB(rc, context);
            commits_.fetch_add(1, std::memory_order_relaxed);
            noteUsage();
            return;
        }
        grow(context);
    }
}

void LMDBStorage::grow(const char *context)
{
    MDB_envinfo info;
    checkLMDB(mdb_env_info(env_, &info), context);
    const size_t from = info.me_mapsize;
    const size_t to = 2 * from;
    checkLMDB(mdb_env_set_mapsize(env_, to), context);
    noteUsage();
    aux::ExchLogger::instance()->warn(std::string("LMDB map full at ") + std::to_string(from >> 20) +
                                      " MiB; doubled it to " + std::to_string(to >> 20) + " MiB (#148)");
}

void LMDBStorage::noteUsage()
{
    MDB_envinfo info;
    if (MDB_SUCCESS == mdb_env_info(env_, &info))
    {
        mapSize_.store(info.me_mapsize, std::memory_order_relaxed);
        mapUsed_.store((info.me_last_pgno + 1) * pageSize_, std::memory_order_relaxed);
    }
}

size_t LMDBStorage::mapSize() const noexcept
{
    return mapSize_.load(std::memory_order_relaxed);
}

size_t LMDBStorage::mapUsed() const noexcept
{
    return mapUsed_.load(std::memory_order_relaxed);
}

IdT LMDBStorage::save(const char *buf, size_t size)
{
    IdT id = generator_.getId();
    save(id, buf, size);
    return id;
}

void LMDBStorage::save(const IdT &id, const char *buf, size_t size)
{
    assert(nullptr != buf);
    assert(0 < size);

    const CompositeKey ck(id, 0);
    write("LMDBStorage::save",
          [&](MDB_txn *txn)
          {
              MDB_val key = makeKey(ck);
              // Check if a record with this ID already exists
              MDB_val existingData;
              if (MDB_SUCCESS == mdb_get(txn, dbi_, &key, &existingData))
              {
                  throw std::runtime_error(
                      "LMDBStorage::save: Unable to save record, record with this Id already exists!");
              }
              MDB_val data;
              data.mv_size = size;
              data.mv_data = const_cast<char *>(buf);
              return mdb_put(txn, dbi_, &key, &data, 0);
          });
}

int LMDBStorage::newestVersion(MDB_txn *txn, const IdT &id, bool *found, u32 *newest) const
{
    MDB_cursor *cursor = nullptr;
    int rc = mdb_cursor_open(txn, dbi_, &cursor);
    if (MDB_SUCCESS != rc)
    {
        return rc;
    }

    // The key is {id, version} compared byte-wise, so all versions of an id are contiguous: seek to the first one and
    // walk just that range. Those bytes are not in numeric order (little-endian u32), so take the maximum by value rather
    // than the last key.
    const CompositeKey first(id, 0);
    *found = false;
    *newest = 0;
    MDB_val curKey = makeKey(first);
    MDB_val curData;
    rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_SET_RANGE);
    while (MDB_SUCCESS == rc)
    {
        if (curKey.mv_size == sizeof(CompositeKey))
        {
            const CompositeKey ck = readKey(curKey);
            if (!(ck.id_ == id))
            {
                break;
            }
            if (!*found || (*newest < ck.version_))
            {
                *newest = ck.version_;
            }
            *found = true;
        }
        rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
    }
    mdb_cursor_close(cursor);
    return (MDB_NOTFOUND == rc) ? MDB_SUCCESS : rc;
}

int LMDBStorage::writeVersion(MDB_txn *txn, const IdT &id, const char *buf, size_t size, u32 *version, bool staged)
{
    bool found = false;
    u32 newest = 0;
    int rc = newestVersion(txn, id, &found, &newest);
    if (MDB_SUCCESS != rc)
    {
        return rc;
    }
    const u32 next = found ? newest + 1 : 0;
    if (staged && (next != *version))
    {
        // The transactions that persist an order run one after another, so nothing writes it between a group's update()
        // and its commit
        throw std::logic_error("LMDBStorage::commitGroup: record " + std::to_string(id.id_) + " is at version " +
                               std::to_string(next) + ", not the " + std::to_string(*version) + " its group expected");
    }

    // Erase every version older than the newest (#148), in this transaction, so the erasing costs no commit of its own.
    // The newest stays: the transaction writing the new version may still roll it back, and erase(id, version) then
    // removes only that one, leaving the newest before it to restore. Every version older than that one is final,
    // because the transactions that persist an order run one after another (TransactionMgr orders transactions that
    // share an object, and a persist operation's object is its order), and Processor::persistOrder() persists an order
    // once per transaction.
    if (found)
    {
        MDB_cursor *cursor = nullptr;
        rc = mdb_cursor_open(txn, dbi_, &cursor);
        if (MDB_SUCCESS != rc)
        {
            return rc;
        }
        const CompositeKey first(id, 0);
        MDB_val curKey = makeKey(first);
        MDB_val curData;
        rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_SET_RANGE);
        while (MDB_SUCCESS == rc)
        {
            if (curKey.mv_size == sizeof(CompositeKey))
            {
                const CompositeKey ck = readKey(curKey);
                if (!(ck.id_ == id))
                {
                    break;
                }
                if (ck.version_ != newest)
                {
                    // The cursor moves on to the next record, which MDB_GET_CURRENT reads
                    rc = mdb_cursor_del(cursor, 0);
                    if (MDB_SUCCESS == rc)
                    {
                        rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_GET_CURRENT);
                    }
                    continue;
                }
            }
            rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
        if ((MDB_SUCCESS != rc) && (MDB_NOTFOUND != rc))
        {
            return rc;
        }
    }

    // Insert the new version
    *version = next;
    const CompositeKey ck(id, next);
    MDB_val key = makeKey(ck);
    MDB_val data;
    data.mv_size = size;
    data.mv_data = const_cast<char *>(buf);
    return mdb_put(txn, dbi_, &key, &data, 0);
}

u32 LMDBStorage::update(const IdT &id, const char *buf, size_t size)
{
    assert(nullptr != buf);
    assert(0 < size);

    if (this == groupOwner_)
    {
        return stage(id, buf, size);
    }
    u32 newVersion = 0;
    write("LMDBStorage::update",
          [&](MDB_txn *txn)
          {
              return writeVersion(txn, id, buf, size, &newVersion, false);
          });
    return newVersion;
}

u32 LMDBStorage::stage(const IdT &id, const char *buf, size_t size)
{
    for (const StagedUpdate &staged : group_)
    {
        if (staged.id_ == id)
        {
            throw std::logic_error("LMDBStorage: record " + std::to_string(id.id_) + " is updated twice in one group");
        }
    }
    bool found = false;
    u32 newest = 0;
    {
        // A read transaction, under the lock every transaction takes (#148)
        std::lock_guard<std::mutex> lock(txnLock_);
        MDB_txn *txn = nullptr;
        checkLMDB(mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn), "LMDBStorage::update (group)");
        const int rc = newestVersion(txn, id, &found, &newest);
        mdb_txn_abort(txn);
        checkLMDB(rc, "LMDBStorage::update (group)");
    }
    const u32 version = found ? newest + 1 : 0;
    group_.push_back(StagedUpdate{ id, version, std::vector<char>(buf, buf + size) });
    return version;
}

void LMDBStorage::beginGroup()
{
    if (nullptr != groupOwner_)
    {
        throw std::logic_error("LMDBStorage::beginGroup: this thread has a group open already");
    }
    group_.clear();
    groupOwner_ = this;
}

void LMDBStorage::commitGroup()
{
    assert(this == groupOwner_);
    // Closed whatever happens: a commit that fails has written nothing
    groupOwner_ = nullptr;
    std::vector<StagedUpdate> staged;
    staged.swap(group_);
    if (staged.empty())
    {
        return;
    }
    // One transaction for them all. A map that fills runs the whole group again, after it grows (#148).
    write("LMDBStorage::commitGroup",
          [&](MDB_txn *txn)
          {
              for (StagedUpdate &record : staged)
              {
                  u32 version = record.version_;
                  const int rc =
                      writeVersion(txn, record.id_, record.data_.data(), record.data_.size(), &version, true);
                  if (MDB_SUCCESS != rc)
                  {
                      return rc;
                  }
              }
              return MDB_SUCCESS;
          });
}

void LMDBStorage::abortGroup() noexcept
{
    if (this == groupOwner_)
    {
        group_.clear();
        groupOwner_ = nullptr;
    }
}

u64 LMDBStorage::commits() const noexcept
{
    return commits_.load(std::memory_order_relaxed);
}

u32 LMDBStorage::replace(const IdT &id, u32 version, const char *buf, size_t size)
{
    assert(nullptr != buf);
    assert(0 < size);

    u32 newVersion = 0;
    write("LMDBStorage::replace",
          [&](MDB_txn *txn)
          {
              // Find max version and verify the target version exists
              bool versionFound = false;
              u32 maxVersion = 0;

              MDB_cursor *cursor = nullptr;
              int rc = mdb_cursor_open(txn, dbi_, &cursor);
              if (rc != MDB_SUCCESS)
              {
                  return rc;
              }

              bool idFound = false;
              MDB_val curKey, curData;
              rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
              while (rc == MDB_SUCCESS)
              {
                  if (curKey.mv_size == sizeof(CompositeKey))
                  {
                      CompositeKey ck = readKey(curKey);
                      if (ck.id_ == id)
                      {
                          idFound = true;
                          if (ck.version_ == version)
                          {
                              versionFound = true;
                          }
                          if (ck.version_ > maxVersion)
                          {
                              maxVersion = ck.version_;
                          }
                      }
                  }
                  rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
              }
              mdb_cursor_close(cursor);

              if (!idFound)
              {
                  throw std::runtime_error(
                      "LMDBStorage::replace: Unable to replace record, record with this Id not exists!");
              }

              if (!versionFound)
              {
                  throw std::runtime_error(
                      "LMDBStorage::replace: Unable to replace record, record with this version not exists!");
              }

              // Delete old version
              CompositeKey oldKey(id, version);
              MDB_val delKey = makeKey(oldKey);
              rc = mdb_del(txn, dbi_, &delKey, nullptr);
              if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND)
              {
                  return rc;
              }

              // Insert new version
              newVersion = maxVersion + 1;
              CompositeKey newCk(id, newVersion);
              MDB_val newKey = makeKey(newCk);
              MDB_val data;
              data.mv_size = size;
              data.mv_data = const_cast<char *>(buf);
              return mdb_put(txn, dbi_, &newKey, &data, 0);
          });
    return newVersion;
}

void LMDBStorage::erase(const IdT &id, u32 version)
{
    const CompositeKey ck(id, version);
    write("LMDBStorage::erase(version)",
          [&](MDB_txn *txn)
          {
              MDB_val key = makeKey(ck);
              const int rc = mdb_del(txn, dbi_, &key, nullptr);
              return (MDB_NOTFOUND == rc) ? MDB_SUCCESS : rc;
          });
}

void LMDBStorage::erase(const IdT &id)
{
    write("LMDBStorage::erase(all)",
          [&](MDB_txn *txn)
          {
              MDB_cursor *cursor = nullptr;
              int rc = mdb_cursor_open(txn, dbi_, &cursor);
              if (rc != MDB_SUCCESS)
              {
                  return rc;
              }

              MDB_val curKey, curData;
              rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
              while (rc == MDB_SUCCESS)
              {
                  bool deleted = false;
                  if (curKey.mv_size == sizeof(CompositeKey))
                  {
                      CompositeKey ck = readKey(curKey);
                      if (ck.id_ == id)
                      {
                          mdb_cursor_del(cursor, 0);
                          deleted = true;
                      }
                  }
                  if (!deleted)
                  {
                      rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
                  }
                  else
                  {
                      rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_GET_CURRENT);
                  }
              }
              mdb_cursor_close(cursor);
              return MDB_SUCCESS;
          });
}

bool LMDBStorage::isExists(const IdT &id) const
{
    assert(nullptr != env_);
    std::lock_guard<std::mutex> lock(txnLock_);

    MDB_txn *txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    checkLMDB(rc, "LMDBStorage::isExists mdb_txn_begin");

    bool found = false;
    MDB_cursor *cursor = nullptr;
    rc = mdb_cursor_open(txn, dbi_, &cursor);
    if (rc == MDB_SUCCESS)
    {
        MDB_val curKey, curData;
        rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
        while (rc == MDB_SUCCESS)
        {
            if (curKey.mv_size == sizeof(CompositeKey))
            {
                CompositeKey ck = readKey(curKey);
                if (ck.id_ == id)
                {
                    found = true;
                    break;
                }
            }
            rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
    }

    mdb_txn_abort(txn);
    return found;
}

bool LMDBStorage::isExists(const IdT &id, u32 version) const
{
    assert(nullptr != env_);
    std::lock_guard<std::mutex> lock(txnLock_);

    CompositeKey ck(id, version);
    MDB_val key = makeKey(ck);
    MDB_val data;

    MDB_txn *txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    checkLMDB(rc, "LMDBStorage::isExists(version) mdb_txn_begin");

    rc = mdb_get(txn, dbi_, &key, &data);
    mdb_txn_abort(txn);

    return rc == MDB_SUCCESS;
}

FileSaver::Newest LMDBStorage::loadNewest(const IdT &id, std::vector<char> *buf)
{
    assert(nullptr != env_);
    assert(nullptr != buf);
    std::lock_guard<std::mutex> lock(txnLock_);

    MDB_txn *txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    checkLMDB(rc, "LMDBStorage::loadNewest mdb_txn_begin");
    // One read transaction for both steps, and the id's own versions only: getTopVersion() walks every record
    bool found = false;
    u32 newest = 0;
    rc = newestVersion(txn, id, &found, &newest);
    MDB_val data{};
    if ((MDB_SUCCESS == rc) && found)
    {
        const CompositeKey ck(id, newest);
        MDB_val key = makeKey(ck);
        rc = mdb_get(txn, dbi_, &key, &data);
    }
    if ((MDB_SUCCESS != rc) || !found)
    {
        mdb_txn_abort(txn);
        checkLMDB(rc, "LMDBStorage::loadNewest");
        return Newest::None;
    }
    buf->assign(static_cast<const char *>(data.mv_data), static_cast<const char *>(data.mv_data) + data.mv_size);
    mdb_txn_abort(txn);
    return Newest::Found;
}

u32 LMDBStorage::getTopVersion(const IdT &id) const
{
    assert(nullptr != env_);
    std::lock_guard<std::mutex> lock(txnLock_);

    MDB_txn *txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    checkLMDB(rc, "LMDBStorage::getTopVersion mdb_txn_begin");

    u32 topVersion = 0;
    bool found = false;

    MDB_cursor *cursor = nullptr;
    rc = mdb_cursor_open(txn, dbi_, &cursor);
    if (rc == MDB_SUCCESS)
    {
        MDB_val curKey, curData;
        rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
        while (rc == MDB_SUCCESS)
        {
            if (curKey.mv_size == sizeof(CompositeKey))
            {
                CompositeKey ck = readKey(curKey);
                if (ck.id_ == id)
                {
                    if (!found || ck.version_ > topVersion)
                    {
                        topVersion = ck.version_;
                    }
                    found = true;
                }
            }
            rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
    }

    mdb_txn_abort(txn);
    return topVersion;
}

size_t LMDBStorage::recordSize(const IdT &id, u32 version) const
{
    assert(nullptr != env_);
    std::lock_guard<std::mutex> lock(txnLock_);

    CompositeKey ck(id, version);
    MDB_val key = makeKey(ck);
    MDB_val data;

    MDB_txn *txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    checkLMDB(rc, "LMDBStorage::recordSize mdb_txn_begin");

    rc = mdb_get(txn, dbi_, &key, &data);
    size_t sz = (rc == MDB_SUCCESS) ? data.mv_size : 0;

    mdb_txn_abort(txn);
    return sz;
}

bool LMDBStorage::loadRecord(const IdT &id, u32 version, std::vector<char> *buf)
{
    assert(nullptr != env_);
    assert(nullptr != buf);
    std::lock_guard<std::mutex> lock(txnLock_);

    CompositeKey ck(id, version);
    MDB_val key = makeKey(ck);
    MDB_val data;

    MDB_txn *txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    checkLMDB(rc, "LMDBStorage::loadRecord mdb_txn_begin");

    rc = mdb_get(txn, dbi_, &key, &data);
    if (rc != MDB_SUCCESS)
    {
        mdb_txn_abort(txn);
        return false;
    }

    buf->resize(data.mv_size);
    std::memcpy(buf->data(), data.mv_data, data.mv_size);

    mdb_txn_abort(txn);
    return true;
}
