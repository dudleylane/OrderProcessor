/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "FileStorageDef.h"
#include "DataModelDef.h"

#ifdef BUILD_PG
namespace COP::PG { class PGWriteBehind; }
#endif

namespace COP{

	class OrderBook;

	namespace Store{

	class OrderDataStorage;

	class FileStorage;

	/// parse incoming buffer into the record
	/// buffer format: <type - 32 bit><body, format depends on type>
	class StorageRecordDispatcher: public FileStorageObserver, public DataSaver, public OrderSaver
	{
	public:
		StorageRecordDispatcher(void);
		virtual ~StorageRecordDispatcher(void);

		void init(DataStorageRestore *storage, OrderBook *orderBook, FileSaver *fileStorage, OrderDataStorage *orderStorage);
	public:
		/// reimplemented from FileStorageObserver
		virtual void startLoad();
		virtual void onRecordLoaded(const IdT& id, u32 version, const char *buf, size_t size);
		virtual void finishLoad();

	public:
		/// reimplemented from DataSaver
		virtual void save(const InstrumentEntry &val);
		virtual void save(const IdT& id, const StringT &val);
		virtual void save(const RawDataEntry &val);
		virtual void save(const AccountEntry &val);
		virtual void save(const ClearingEntry &val);
		virtual void save(const ExecutionsT &val);

	public:
		/// reimplemented from OrderSaver
		virtual void save(const OrderEntry &val);

#ifdef BUILD_PG
	public:
		void setPGWriter(PG::PGWriteBehind* writer) { pgWriter_ = writer; }
#endif

	public:
		enum RecordType{
			INVALID_RECORDTYPE = 0,
			INSTRUMENT_RECORDTYPE,
			STRING_RECORDTYPE,
			ACCOUNT_RECORDTYPE,
			CLEARING_RECORDTYPE,
			RAWDATA_RECORDTYPE,
			ORDER_RECORDTYPE,
			EXECUTION_RECORDTYPE,
			EXECUTIONS_RECORDTYPE,
			TOTAL_RECORDTYPE
		};

	private:
		DataStorageRestore *storage_;
		OrderBook *orderBook_;
		FileSaver *fileStorage_;
		OrderDataStorage *orderStorage_;
#ifdef BUILD_PG
		PG::PGWriteBehind* pgWriter_ = nullptr;
#endif
	};

	}
}