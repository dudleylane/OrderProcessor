/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <vector>
#include <cstring>
#include <lmdb.h>

#include "IdTGenerator.h"
#include "FileStorageDef.h"

namespace COP{
	namespace Store{

	class LMDBStorage final : public FileSaver
	{
	public:
		LMDBStorage();
		LMDBStorage(const std::string &path, FileStorageObserver *observer);
		~LMDBStorage();

	public:
		/// reimplemented from FileSaver
		void load(const std::string &path, FileStorageObserver *observer) override;
		IdT save(const char *buf, size_t size) override;
		void save(const IdT &id, const char *buf, size_t size) override;
		u32 update(const IdT& id, const char *buf, size_t size) override;
		u32 replace(const IdT& id, u32 version, const char *buf, size_t size) override;
		void erase(const IdT& id, u32 version) override;
		void erase(const IdT& id) override;

	public:
		/// return true if record exists
		bool isExists(const IdT& id) const;
		/// return true if version of record exists
		bool isExists(const IdT& id, u32 version) const;
		/// returns latest version of the record
		u32 getTopVersion(const IdT& id) const;
		/// returns size of the record with version
		size_t recordSize(const IdT& id, u32 version) const;
		/// loads record from file into the buf
		/// return false if version of record not exists
		bool loadRecord(const IdT& id, u32 version, std::vector<char> *buf);

	private:
		struct CompositeKey{
			IdT id_;
			u32 version_;

			CompositeKey(){ std::memset(this, 0, sizeof(CompositeKey)); }
			CompositeKey(const IdT &id, u32 version){
				std::memset(this, 0, sizeof(CompositeKey));
				id_ = id;
				version_ = version;
			}
		};

		static MDB_val makeKey(const CompositeKey &key);
		static CompositeKey readKey(const MDB_val &val);

		void close();

		LMDBStorage(const LMDBStorage &) = delete;
		LMDBStorage &operator =(const LMDBStorage &) = delete;

	private:
		MDB_env *env_;
		MDB_dbi dbi_;
		bool open_;
		IdTValueGenerator generator_;
	};

	}
}
