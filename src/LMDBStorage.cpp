/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include <cassert>
#include <sys/stat.h>

#include "LMDBStorage.h"

using namespace COP;
using namespace COP::Store;

namespace{

	const size_t DEFAULT_MAP_SIZE = 256UL * 1024UL * 1024UL; // 256 MB

	void checkLMDB(int rc, const char *context){
		if(rc != MDB_SUCCESS){
			std::string msg = std::string(context) + ": " + mdb_strerror(rc);
			throw std::runtime_error(msg);
		}
	}

}

MDB_val LMDBStorage::makeKey(const CompositeKey &key){
	MDB_val val;
	val.mv_size = sizeof(CompositeKey);
	val.mv_data = const_cast<CompositeKey *>(&key);
	return val;
}

LMDBStorage::CompositeKey LMDBStorage::readKey(const MDB_val &val){
	assert(val.mv_size == sizeof(CompositeKey));
	CompositeKey key;
	std::memcpy(&key, val.mv_data, sizeof(CompositeKey));
	return key;
}

LMDBStorage::LMDBStorage():
	env_(nullptr), dbi_(0), open_(false), generator_()
{
}

LMDBStorage::LMDBStorage(const std::string &path, FileStorageObserver *observer):
	env_(nullptr), dbi_(0), open_(false), generator_()
{
	load(path, observer);
}

LMDBStorage::~LMDBStorage()
{
	close();
}

void LMDBStorage::close()
{
	if(open_){
		mdb_dbi_close(env_, dbi_);
		open_ = false;
	}
	if(env_){
		mdb_env_close(env_);
		env_ = nullptr;
	}
}

void LMDBStorage::load(const std::string &path, FileStorageObserver *observer)
{
	assert(nullptr == env_);
	assert(nullptr != observer);

	// Ensure the directory exists
	mkdir(path.c_str(), 0755);

	int rc = mdb_env_create(&env_);
	checkLMDB(rc, "LMDBStorage::load mdb_env_create");

	rc = mdb_env_set_mapsize(env_, DEFAULT_MAP_SIZE);
	checkLMDB(rc, "LMDBStorage::load mdb_env_set_mapsize");

	rc = mdb_env_open(env_, path.c_str(), 0, 0664);
	if(rc != MDB_SUCCESS){
		mdb_env_close(env_);
		env_ = nullptr;
		checkLMDB(rc, "LMDBStorage::load mdb_env_open");
	}

	// Open the default (unnamed) database
	MDB_txn *txn = nullptr;
	rc = mdb_txn_begin(env_, nullptr, 0, &txn);
	if(rc != MDB_SUCCESS){
		mdb_env_close(env_);
		env_ = nullptr;
		checkLMDB(rc, "LMDBStorage::load mdb_txn_begin");
	}

	rc = mdb_dbi_open(txn, nullptr, MDB_CREATE, &dbi_);
	if(rc != MDB_SUCCESS){
		mdb_txn_abort(txn);
		mdb_env_close(env_);
		env_ = nullptr;
		checkLMDB(rc, "LMDBStorage::load mdb_dbi_open");
	}

	// Scan all existing records and notify observer
	observer->startLoad();

	MDB_cursor *cursor = nullptr;
	rc = mdb_cursor_open(txn, dbi_, &cursor);
	if(rc == MDB_SUCCESS){
		MDB_val key, data;
		while(mdb_cursor_get(cursor, &key, &data, MDB_NEXT) == MDB_SUCCESS){
			if(key.mv_size == sizeof(CompositeKey)){
				CompositeKey ck = readKey(key);
				observer->onRecordLoaded(ck.id_, ck.version_,
					static_cast<const char *>(data.mv_data), data.mv_size);
			}
		}
		mdb_cursor_close(cursor);
	}

	rc = mdb_txn_commit(txn);
	if(rc != MDB_SUCCESS){
		mdb_env_close(env_);
		env_ = nullptr;
		checkLMDB(rc, "LMDBStorage::load mdb_txn_commit");
	}

	open_ = true;
	observer->finishLoad();
}

IdT LMDBStorage::save(const char *buf, size_t size)
{
	IdT id = generator_.getId();
	save(id, buf, size);
	return id;
}

void LMDBStorage::save(const IdT &id, const char *buf, size_t size)
{
	assert(nullptr != env_);
	assert(nullptr != buf);
	assert(0 < size);

	CompositeKey ck(id, 0);
	MDB_val key = makeKey(ck);
	MDB_val data;
	data.mv_size = size;
	data.mv_data = const_cast<char *>(buf);

	MDB_txn *txn = nullptr;
	int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
	checkLMDB(rc, "LMDBStorage::save mdb_txn_begin");

	// Check if a record with this ID already exists
	MDB_val existingData;
	rc = mdb_get(txn, dbi_, &key, &existingData);
	if(rc == MDB_SUCCESS){
		mdb_txn_abort(txn);
		throw std::runtime_error("LMDBStorage::save: Unable to save record, record with this Id already exists!");
	}

	rc = mdb_put(txn, dbi_, &key, &data, 0);
	if(rc != MDB_SUCCESS){
		mdb_txn_abort(txn);
		checkLMDB(rc, "LMDBStorage::save mdb_put");
	}

	rc = mdb_txn_commit(txn);
	checkLMDB(rc, "LMDBStorage::save mdb_txn_commit");
}

u32 LMDBStorage::update(const IdT& id, const char *buf, size_t size)
{
	assert(nullptr != env_);
	assert(nullptr != buf);
	assert(0 < size);

	MDB_txn *txn = nullptr;
	int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
	checkLMDB(rc, "LMDBStorage::update mdb_txn_begin");

	// Find the max version for this ID using a cursor
	u32 newVersion = 0;
	MDB_cursor *cursor = nullptr;
	rc = mdb_cursor_open(txn, dbi_, &cursor);
	if(rc != MDB_SUCCESS){
		mdb_txn_abort(txn);
		checkLMDB(rc, "LMDBStorage::update mdb_cursor_open");
	}

	// Scan for existing versions of this ID
	MDB_val curKey, curData;
	rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
	while(rc == MDB_SUCCESS){
		if(curKey.mv_size == sizeof(CompositeKey)){
			CompositeKey ck = readKey(curKey);
			if(ck.id_ == id){
				if(ck.version_ >= newVersion)
					newVersion = ck.version_ + 1;
			}
		}
		rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
	}
	mdb_cursor_close(cursor);

	// Insert the new version
	CompositeKey ck(id, newVersion);
	MDB_val key = makeKey(ck);
	MDB_val data;
	data.mv_size = size;
	data.mv_data = const_cast<char *>(buf);

	rc = mdb_put(txn, dbi_, &key, &data, 0);
	if(rc != MDB_SUCCESS){
		mdb_txn_abort(txn);
		checkLMDB(rc, "LMDBStorage::update mdb_put");
	}

	rc = mdb_txn_commit(txn);
	checkLMDB(rc, "LMDBStorage::update mdb_txn_commit");

	return newVersion;
}

u32 LMDBStorage::replace(const IdT& id, u32 version, const char *buf, size_t size)
{
	assert(nullptr != env_);
	assert(nullptr != buf);
	assert(0 < size);

	MDB_txn *txn = nullptr;
	int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
	checkLMDB(rc, "LMDBStorage::replace mdb_txn_begin");

	// Find max version and verify the target version exists
	bool versionFound = false;
	u32 maxVersion = 0;

	MDB_cursor *cursor = nullptr;
	rc = mdb_cursor_open(txn, dbi_, &cursor);
	if(rc != MDB_SUCCESS){
		mdb_txn_abort(txn);
		checkLMDB(rc, "LMDBStorage::replace mdb_cursor_open");
	}

	bool idFound = false;
	MDB_val curKey, curData;
	rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
	while(rc == MDB_SUCCESS){
		if(curKey.mv_size == sizeof(CompositeKey)){
			CompositeKey ck = readKey(curKey);
			if(ck.id_ == id){
				idFound = true;
				if(ck.version_ == version)
					versionFound = true;
				if(ck.version_ > maxVersion)
					maxVersion = ck.version_;
			}
		}
		rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
	}
	mdb_cursor_close(cursor);

	if(!idFound){
		mdb_txn_abort(txn);
		throw std::runtime_error("LMDBStorage::replace: Unable to replace record, record with this Id not exists!");
	}

	if(!versionFound){
		mdb_txn_abort(txn);
		throw std::runtime_error("LMDBStorage::replace: Unable to replace record, record with this version not exists!");
	}

	// Delete old version
	CompositeKey oldKey(id, version);
	MDB_val delKey = makeKey(oldKey);
	rc = mdb_del(txn, dbi_, &delKey, nullptr);
	if(rc != MDB_SUCCESS && rc != MDB_NOTFOUND){
		mdb_txn_abort(txn);
		checkLMDB(rc, "LMDBStorage::replace mdb_del");
	}

	// Insert new version
	u32 newVersion = maxVersion + 1;
	CompositeKey newCk(id, newVersion);
	MDB_val newKey = makeKey(newCk);
	MDB_val data;
	data.mv_size = size;
	data.mv_data = const_cast<char *>(buf);

	rc = mdb_put(txn, dbi_, &newKey, &data, 0);
	if(rc != MDB_SUCCESS){
		mdb_txn_abort(txn);
		checkLMDB(rc, "LMDBStorage::replace mdb_put");
	}

	rc = mdb_txn_commit(txn);
	checkLMDB(rc, "LMDBStorage::replace mdb_txn_commit");

	return newVersion;
}

void LMDBStorage::erase(const IdT& id, u32 version)
{
	assert(nullptr != env_);

	CompositeKey ck(id, version);
	MDB_val key = makeKey(ck);

	MDB_txn *txn = nullptr;
	int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
	checkLMDB(rc, "LMDBStorage::erase(version) mdb_txn_begin");

	rc = mdb_del(txn, dbi_, &key, nullptr);
	if(rc != MDB_SUCCESS && rc != MDB_NOTFOUND){
		mdb_txn_abort(txn);
		checkLMDB(rc, "LMDBStorage::erase(version) mdb_del");
	}

	rc = mdb_txn_commit(txn);
	checkLMDB(rc, "LMDBStorage::erase(version) mdb_txn_commit");
}

void LMDBStorage::erase(const IdT& id)
{
	assert(nullptr != env_);

	MDB_txn *txn = nullptr;
	int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
	checkLMDB(rc, "LMDBStorage::erase(all) mdb_txn_begin");

	MDB_cursor *cursor = nullptr;
	rc = mdb_cursor_open(txn, dbi_, &cursor);
	if(rc != MDB_SUCCESS){
		mdb_txn_abort(txn);
		checkLMDB(rc, "LMDBStorage::erase(all) mdb_cursor_open");
	}

	MDB_val curKey, curData;
	rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
	while(rc == MDB_SUCCESS){
		if(curKey.mv_size == sizeof(CompositeKey)){
			CompositeKey ck = readKey(curKey);
			if(ck.id_ == id){
				mdb_cursor_del(cursor, 0);
			}
		}
		rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_NEXT);
	}
	mdb_cursor_close(cursor);

	rc = mdb_txn_commit(txn);
	checkLMDB(rc, "LMDBStorage::erase(all) mdb_txn_commit");
}

bool LMDBStorage::isExists(const IdT& id) const
{
	assert(nullptr != env_);

	MDB_txn *txn = nullptr;
	int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
	checkLMDB(rc, "LMDBStorage::isExists mdb_txn_begin");

	bool found = false;
	MDB_cursor *cursor = nullptr;
	rc = mdb_cursor_open(txn, dbi_, &cursor);
	if(rc == MDB_SUCCESS){
		MDB_val curKey, curData;
		rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
		while(rc == MDB_SUCCESS){
			if(curKey.mv_size == sizeof(CompositeKey)){
				CompositeKey ck = readKey(curKey);
				if(ck.id_ == id){
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

bool LMDBStorage::isExists(const IdT& id, u32 version) const
{
	assert(nullptr != env_);

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

u32 LMDBStorage::getTopVersion(const IdT& id) const
{
	assert(nullptr != env_);

	MDB_txn *txn = nullptr;
	int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
	checkLMDB(rc, "LMDBStorage::getTopVersion mdb_txn_begin");

	u32 topVersion = 0;
	bool found = false;

	MDB_cursor *cursor = nullptr;
	rc = mdb_cursor_open(txn, dbi_, &cursor);
	if(rc == MDB_SUCCESS){
		MDB_val curKey, curData;
		rc = mdb_cursor_get(cursor, &curKey, &curData, MDB_FIRST);
		while(rc == MDB_SUCCESS){
			if(curKey.mv_size == sizeof(CompositeKey)){
				CompositeKey ck = readKey(curKey);
				if(ck.id_ == id){
					if(!found || ck.version_ > topVersion)
						topVersion = ck.version_;
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

size_t LMDBStorage::recordSize(const IdT& id, u32 version) const
{
	assert(nullptr != env_);

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

bool LMDBStorage::loadRecord(const IdT& id, u32 version, std::vector<char> *buf)
{
	assert(nullptr != env_);
	assert(nullptr != buf);

	CompositeKey ck(id, version);
	MDB_val key = makeKey(ck);
	MDB_val data;

	MDB_txn *txn = nullptr;
	int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
	checkLMDB(rc, "LMDBStorage::loadRecord mdb_txn_begin");

	rc = mdb_get(txn, dbi_, &key, &data);
	if(rc != MDB_SUCCESS){
		mdb_txn_abort(txn);
		return false;
	}

	buf->resize(data.mv_size);
	std::memcpy(buf->data(), data.mv_data, data.mv_size);

	mdb_txn_abort(txn);
	return true;
}
