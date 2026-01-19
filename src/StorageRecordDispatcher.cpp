/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "StorageRecordDispatcher.h"

#include "InstrumentCodec.h"
#include "StringTCodec.h"
#include "AccountCodec.h"
#include "ClearingCodec.h"
#include "RawDataCodec.h"
#include "OrderCodec.h"
#include "OrderStorage.h"

using namespace std;
using namespace COP;
using namespace COP::Store;

namespace{
	const int MINIMAL_SIZE = 4;
}

StorageRecordDispatcher::StorageRecordDispatcher(void): 
	storage_(nullptr), orderBook_(nullptr), fileStorage_(nullptr), orderStorage_(nullptr)
{
}

StorageRecordDispatcher::~StorageRecordDispatcher(void)
{
}

void StorageRecordDispatcher::init(DataStorageRestore *storage, OrderBook *orderBook, FileSaver *fileStorage, OrderDataStorage *orderStorage)
{
	assert(nullptr == storage_);
	assert(nullptr == orderBook_);
	assert(nullptr == fileStorage_);
	assert(nullptr == orderStorage_);

	storage_ = storage;
	orderBook_ = orderBook;
	fileStorage_ = fileStorage;
	orderStorage_ = orderStorage;
}

void StorageRecordDispatcher::startLoad()
{
	
}

void StorageRecordDispatcher::onRecordLoaded(const IdT& id, u32 version, const char *buf, size_t size)
{
	assert(nullptr != buf);

	if(MINIMAL_SIZE > size)
		throw std::runtime_error("Record size invalid, record could not be restored!");

	RecordType type;
	memcpy(&type, buf, sizeof(type));
	switch(type){
	case INSTRUMENT_RECORDTYPE:
		{
			std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
			Codec::InstrumentCodec::decode(id, version, buf + sizeof(type), size - sizeof(type), instr.get());
			storage_->restore(instr.get());
			instr.release();
		}
		break;
	case STRING_RECORDTYPE:
		{
			std::unique_ptr<StringT> str(new StringT());
			Codec::StringTCodec::decode(buf + sizeof(type), size - sizeof(type), str.get());
			storage_->restore(id, str.get());
			str.release();
		}
		break;
	case ACCOUNT_RECORDTYPE:
		{
			std::unique_ptr<AccountEntry> acct(new AccountEntry());
			Codec::AccountCodec::decode(id, version, buf + sizeof(type), size - sizeof(type), acct.get());
			storage_->restore(acct.get());
			acct.release();
		}
		break;
	case CLEARING_RECORDTYPE:
		{
			std::unique_ptr<ClearingEntry> clr(new ClearingEntry());
			Codec::ClearingCodec::decode(id, version, buf + sizeof(type), size - sizeof(type), clr.get());
			storage_->restore(clr.get());
			clr.release();
		}
		break;
	case RAWDATA_RECORDTYPE:
		{
			std::unique_ptr<RawDataEntry> raw(new RawDataEntry());
			Codec::RawDataCodec::decode(id, version, buf + sizeof(type), size - sizeof(type), raw.get());
			storage_->restore(raw.get());
			raw.release();
		}
		break;
	case EXECUTION_RECORDTYPE:
		break;
	case EXECUTIONS_RECORDTYPE:
		break;
	case ORDER_RECORDTYPE:
		{
			std::unique_ptr<OrderEntry> order(Codec::OrderCodec::decode(id, version, buf + sizeof(type), size - sizeof(type)));
			orderBook_->restore(*order.get());
			orderStorage_->restore(order.get());
			order.release();
		}
		break;
	default:
		throw std::runtime_error("Invalid record type, unable to decode record!");
	};
}

void StorageRecordDispatcher::finishLoad()
{

}

void StorageRecordDispatcher::save(const InstrumentEntry &val)
{
	string buffer;
	{
		char typebuf[36];
		int t = StorageRecordDispatcher::INSTRUMENT_RECORDTYPE;
		memcpy(typebuf, &t, sizeof(t));
		buffer.append(typebuf, sizeof(t));
	}
	IdT id; 
	u32 version;
	Codec::InstrumentCodec::encode(val, &buffer, &id, &version);
	fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const IdT& id, const StringT &val)
{
	string buffer;
	{
		char typebuf[36];
		int t = StorageRecordDispatcher::STRING_RECORDTYPE;
		memcpy(typebuf, &t, sizeof(t));
		buffer.append(typebuf, sizeof(t));
	}
	Codec::StringTCodec::encode(val, &buffer);
	fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const RawDataEntry &val)
{
	string buffer;
	{
		char typebuf[36];
		int t = StorageRecordDispatcher::RAWDATA_RECORDTYPE;
		memcpy(typebuf, &t, sizeof(t));
		buffer.append(typebuf, sizeof(t));
	}
	IdT id; 
	u32 version;
	Codec::RawDataCodec::encode(val, &buffer, &id, &version);
	fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const AccountEntry &val)
{
	string buffer;
	{
		char typebuf[36];
		int t = StorageRecordDispatcher::ACCOUNT_RECORDTYPE;
		memcpy(typebuf, &t, sizeof(t));
		buffer.append(typebuf, sizeof(t));
	}
	IdT id; 
	u32 version;
	Codec::AccountCodec::encode(val, &buffer, &id, &version);
	fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const ClearingEntry &val)
{
	string buffer;
	{
		char typebuf[36];
		int t = StorageRecordDispatcher::CLEARING_RECORDTYPE;
		memcpy(typebuf, &t, sizeof(t));
		buffer.append(typebuf, sizeof(t));
	}
	IdT id; 
	u32 version;
	Codec::ClearingCodec::encode(val, &buffer, &id, &version);
	fileStorage_->save(id, buffer.c_str(), buffer.size());
}

void StorageRecordDispatcher::save(const ExecutionsT &/*val*/)
{
	assert(false);
}

void StorageRecordDispatcher::save(const OrderEntry &val)
{
	string buffer;
	{
		char typebuf[36];
		int t = StorageRecordDispatcher::ORDER_RECORDTYPE;
		memcpy(typebuf, &t, sizeof(t));
		buffer.append(typebuf, sizeof(t));
	}
	IdT id; 
	u32 version;
	Codec::OrderCodec::encode(val, &buffer, &id, &version);
	fileStorage_->save(id, buffer.c_str(), buffer.size());
}

