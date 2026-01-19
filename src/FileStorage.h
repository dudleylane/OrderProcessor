/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <map>
#include <deque>
#include <vector>
#include <tbb/mutex.h>
#include "IdTGenerator.h"

#include "FileStorageDef.h"

namespace COP{
	namespace Store{

	struct FileRecord{
		IdT id_;
		size_t offset_;
		u32 size_;
		u32 version_;
		bool enabled_;

		FileRecord():id_(), offset_(0), size_(0), version_(0), enabled_(false){}

		FileRecord(const IdT &id, u32 size):id_(id), offset_(0), size_(size), version_(0), enabled_(true){}
	};

	/// file stored in following format:
	// first line contains '<Sxxxx::Version x.x.x.xE>'
	// '<S'<header>'::'<record body>'E>'
	// <header> ::= <4bytes record size (from '<S' until 'E>')>'.'<valid record Y/N>'.'<16 bytes record id>'.'<4 bytes version>

	class FileStorage: public FileSaver
	{
	public:
		FileStorage();
		FileStorage(const std::string &fileName, FileStorageObserver *observer);
		~FileStorage(void);

		static void init();

	public:
		/// reimplemented from FileSaver
		virtual void load(const std::string &fileName, FileStorageObserver *observer);
		virtual IdT save(const char *buf, size_t size);
		virtual void save(const IdT &id, const char *buf, size_t size);
		virtual u32 update(const IdT& id, const char *buf, size_t size);
		virtual u32 replace(const IdT& id, u32 version, const char *buf, size_t size);
		virtual void erase(const IdT& id, u32 version);
		virtual void erase(const IdT& id);

	public:
		/// return true if record exists
		bool isExists(const IdT& id)const;
		/// return true if version of record exists
		bool isExists(const IdT& id, u32 version)const;
		/// returns latest version of the record
		u32 getTopVersion(const IdT& id)const;
		/// returns size of the record with version
		size_t recordSize(const IdT& id, u32 version)const;
		/// loads record from file into the buf
		/// return false if version of record not exists
		bool loadRecord(const IdT& id, u32 version, std::vector<char> *buf);
		/// loads record from file into the buf
		/// return -1 if version of record not exists
		/// return 0 if record was loaded
		/// return >1 if bufSize less than record size, record size returned
		int loadRecord(const IdT& id, u32 version, char **buf, size_t bufSize);

	private:

		void pocessRecord(FILE *file, FileStorageObserver *observer);

		void clear();

		FileStorage(const FileStorage &);
		FileStorage &operator =(const FileStorage &);
	private:
		mutable tbb::mutex lock_;

		FILE *file_;

		IdTValueGenerator generator_;

		typedef std::deque<FileRecord> RecordChainT;
		typedef std::map<IdT, RecordChainT *> RecordsT;

		RecordsT records_;
	};

	}
}