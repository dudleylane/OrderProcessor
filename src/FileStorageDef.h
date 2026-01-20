/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TypesDef.h"

namespace COP{

	struct InstrumentEntry;
	struct AccountEntry;
	struct ClearingEntry;

	namespace Store{

		class FileStorageObserver;

		/// interface to the file storage. allows to save records to the file or load, remove from file
		class FileSaver{
		public:
			virtual ~FileSaver(){}

			/// loads records from file
			virtual void load(const std::string &fileName, FileStorageObserver *observer) = 0;

			/// saves record into the file
			virtual IdT save(const char *buf, size_t size) = 0;
			virtual void save(const IdT &id, const char *buf, size_t size) = 0;
			/// saves new version of the record and return version
			virtual u32 update(const IdT& id, const char *buf, size_t size) = 0;
			/// saves new version of the record and erase existing version. 
			/// return 0 if replaced version not exists, returns version otherwise
			virtual u32 replace(const IdT& id, u32 version, const char *buf, size_t size) = 0;
			/// erases version of the record from file
			virtual void erase(const IdT& id, u32 version) = 0;
			/// erases all versions of record from file
			virtual void erase(const IdT& id) = 0;
		};

		/// observer for the storage loader. handles loaded records and start/end loading events
		class FileStorageObserver{
		public:
			virtual ~FileStorageObserver(){}

			virtual void startLoad() = 0;
			virtual void onRecordLoaded(const IdT& id, u32 version, const char *buf, size_t size) = 0;
			virtual void finishLoad() = 0;
		};

		/// restores loaded from storage entities into the memory storage
		class DataStorageRestore{
		public:
			virtual ~DataStorageRestore(){}

			virtual void restore(InstrumentEntry *val) = 0;
			virtual void restore(const IdT& id, StringT *val) = 0;
			virtual void restore(RawDataEntry *val) = 0;
			virtual void restore(AccountEntry *val) = 0;
			virtual void restore(ClearingEntry *val) = 0;
			virtual void restore(ExecutionsT *val) = 0;
		};

		/// saves entities into the storage
		class DataSaver{
		public:
			virtual ~DataSaver(){}

			virtual void save(const InstrumentEntry &val) = 0;
			virtual void save(const IdT& id, const StringT &val) = 0;
			virtual void save(const RawDataEntry &val) = 0;
			virtual void save(const AccountEntry &val) = 0;
			virtual void save(const ClearingEntry &val) = 0;
			virtual void save(const ExecutionsT &val) = 0;
		};

	}
}