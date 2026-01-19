/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TestAux.h"
#include <iostream>
#include <vector>
#include <tbb/tick_count.h>

#include "DataModelDef.h"

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
using namespace COP::Codec;
using namespace COP::Store;
using namespace aux;
using namespace tbb;
using namespace test;

namespace{
	class TestDataStorageRestore: public DataStorageRestore{
	public:
		virtual ~TestDataStorageRestore(){
			for(size_t i = 0; i < instr_.size(); ++i)
				delete instr_.at(i);
			for(size_t i = 0; i < account_.size(); ++i)
				delete account_.at(i);
			for(size_t i = 0; i < string_.size(); ++i)
				delete string_.at(i).str_;
			for(size_t i = 0; i < rawData_.size(); ++i){
				delete rawData_.at(i)->data_;
				delete rawData_.at(i);
			}

		}

		virtual void restore(InstrumentEntry *val){
			assert(NULL != val);
			instr_.push_back(val);
		}
		virtual void restore(const IdT& id, StringT *val){
			assert(NULL != val);
			string_.push_back(IdString(val, id));
		}
		virtual void restore(RawDataEntry *val){
			assert(NULL != val);
			rawData_.push_back(val);	
		}
		virtual void restore(AccountEntry *val){
			assert(NULL != val);
			account_.push_back(val);	
		}
		virtual void restore(ClearingEntry *val){			
			assert(NULL != val);
			clearing_.push_back(val);	
		}
		virtual void restore(ExecutionsT *val){}

	public:
		typedef std::deque<InstrumentEntry *> InstrumentsT;
		InstrumentsT instr_;

		typedef std::deque<AccountEntry *> AccountsT;
		AccountsT account_;

		typedef std::deque<ClearingEntry *> ClearingT;
		ClearingT clearing_;

		typedef std::deque<RawDataEntry *> RawDatasT;
		RawDatasT rawData_;

		struct IdString{
			StringT *str_;
			IdT id_;

			IdString(): str_(NULL), id_(){}
			IdString(StringT *v, const IdT &id): str_(v), id_(id){}
		};
		typedef std::deque<IdString> StringsT;
		StringsT string_;
	};

	class TestOrderBook: public OrderBook{
	public:
		virtual ~TestOrderBook(){}

		virtual void add(const OrderEntry& order){}
		virtual void remove(const OrderEntry& order){}
		virtual IdT find(const OrderFunctor &functor)const{
			assert(false);
			return IdT();
		}
		virtual void findAll(const OrderFunctor &functor, OrdersT *result)const{}

		virtual IdT getTop(const SourceIdT &instrument, const Side &side)const{
			assert(false);
			return IdT();
		}

		virtual void restore(const OrderEntry& order){
			orders_.push_back(&order); //its incorrect to save pointer, just for testing
		}

		typedef std::deque<const OrderEntry *> OrdersT;
		OrdersT orders_;

	};

	class TestFileSaver: public FileSaver{
	public:
		TestFileSaver(){}

		~TestFileSaver(){}

		virtual void load(const std::string &fileName, FileStorageObserver *observer){}
		virtual IdT save(const char *buf, size_t size){
			assert(NULL != buf);
			assert(0 < size);
			records_.insert(RecordsT::value_type(IdT(), Record(string(buf, size))));
			return IdT();
		}
		virtual void save(const IdT &id, const char *buf, size_t size){
			assert(NULL != buf);
			assert(0 < size);
			records_.insert(RecordsT::value_type(id, Record(string(buf, size))));
		}
		virtual u32 update(const IdT& id, const char *buf, size_t size){
			assert(false);
			assert(NULL != buf);
			assert(0 < size);
			//records_.push_back(Record(id, string(buf, size)));
			return 0;
		}
		virtual u32 replace(const IdT& id, u32 version, const char *buf, size_t size){
			assert(false);
			return 0;
		}
		virtual void erase(const IdT& id, u32 version){}
		virtual void erase(const IdT& id){}

		struct Record{
			string record_;

			Record(): record_(){}
			Record(const std::string &rec): record_(rec){}
		};
		typedef std::map<IdT, Record> RecordsT;
		RecordsT records_;
	};

	SourceIdT addInstrument(const std::string &name){
		auto_ptr<InstrumentEntry> instr(new InstrumentEntry());
		instr->symbol_ = name;
		instr->securityId_ = "AAA";
		instr->securityIdSource_ = "AAASrc";
		return WideDataStorage::instance()->add(instr.release());
	}

	SourceIdT addRawData(const std::string &name){
		auto_ptr<RawDataEntry> raw(new RawDataEntry());
		raw->type_ = STRING_RAWDATATYPE;
		auto_ptr<char> val(new char[name.size()]);
		memcpy(val.get(), name.c_str(), name.size());
		raw->data_ = val.release();
		raw->length_ = name.size();
		return WideDataStorage::instance()->add(raw.release());
	}

	OrderEntry *createOrder(){
		SourceIdT v;
		SourceIdT instrId = addInstrument("instrument");
		SourceIdT clOrderId = addRawData("clOrderId_");
		auto_ptr<OrderEntry> val(new OrderEntry(SourceIdT(1, 1), SourceIdT(2, 2), clOrderId, 
					SourceIdT(4, 4), instrId, SourceIdT(6, 6), SourceIdT(7, 7), SourceIdT(8, 8)));
		val->creationTime_ = 1111;
		val->lastUpdateTime_ = 1112;
		val->expireTime_ = 1113;
		val->settlDate_ = 1114;
		val->price_ = 22.22;
		val->stopPx_ = 23.23;
		val->avgPx_ = 24.24;
		val->dayAvgPx_ = 24.24;
		val->status_ = REJECTED_ORDSTATUS;
		val->side_ = BUY_SIDE;
		val->ordType_ = LIMIT_ORDERTYPE;
		val->tif_ = GTD_TIF;
		val->settlType_ = _4_SETTLTYPE;
		val->capacity_ = PROPRIETARY_CAPACITY;
		val->currency_ = USD_CURRENCY;
		val->minQty_ = 3333;
		val->orderQty_ = 3334;
		val->leavesQty_ = 3335;
		val->cumQty_ = 3336;
		val->dayOrderQty_ = 3337;
		val->dayCumQty_ = 3338;
		val->orderId_ = IdT(4444, 4445);
		val->origOrderId_ = IdT(5555, 5556);
		val->stateMachinePersistance_.orderData_ = val.get();
		val->stateMachinePersistance_.stateZone1Id_ = 6;
		val->stateMachinePersistance_.stateZone2Id_ = 7;
		return val.release();
	}
}

bool testStorageRecordDispatcher()
{
	{/// empty initialization
		TestDataStorageRestore restore;
		TestOrderBook ordBook;
		TestFileSaver saver;
		OrderDataStorage ordStore;
		StorageRecordDispatcher tst;

		tst.init(&restore, &ordBook, &saver, &ordStore);
	}
	{/// initialization with data in storage
		TestDataStorageRestore restore;
		TestOrderBook ordBook;
		TestFileSaver saver;
		OrderDataStorage ordStore;
		StorageRecordDispatcher tst;

		WideDataStorage::create();

		tst.init(&restore, &ordBook, &saver, &ordStore);	
		tst.startLoad();
		{// load InstrumentEntry
			InstrumentEntry val;
			val.id_ = IdT(1111, 6789);
			val.securityId_ = "securityId_";
			val.securityIdSource_ = "securityIdSource_";
			val.symbol_ = "symbol_";
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::INSTRUMENT_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			InstrumentCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());

			tst.onRecordLoaded(id, version, buf.c_str(), buf.size());
			check(1 == restore.instr_.size());
			check(NULL != restore.instr_.at(0));
			check(restore.instr_.at(0)->id_ == val.id_);
			check(restore.instr_.at(0)->securityId_ == val.securityId_);
			check(restore.instr_.at(0)->securityIdSource_ == val.securityIdSource_);
			check(restore.instr_.at(0)->symbol_ == val.symbol_);		
		}
		{// load StringT
			StringT val = "string_";
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::STRING_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			StringTCodec::encode(val, &buf);
			check(!buf.empty());

			tst.onRecordLoaded(id, version, buf.c_str(), buf.size());
			check(1 == restore.instr_.size());
			check(1 == restore.string_.size());
			check(restore.string_.at(0).id_ == id);
			check(NULL != restore.string_.at(0).str_);
			check(*(restore.string_.at(0).str_) == "string_");
		}
		{// load ACCOUNT_RECORDTYPE
			AccountEntry val;
			val.id_ = IdT(1111, 6789);
			val.account_ = "account_";
			val.firm_ = "firm_";
			val.type_ = PRINCIPAL_ACCOUNTTYPE;
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::ACCOUNT_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			AccountCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());

			tst.onRecordLoaded(id, version, buf.c_str(), buf.size());
			check(1 == restore.instr_.size());
			check(1 == restore.string_.size());
			check(1 == restore.account_.size());
			check(NULL != restore.account_.at(0));
			check(restore.account_.at(0)->id_ == val.id_);
			check(restore.account_.at(0)->account_ == val.account_);
			check(restore.account_.at(0)->firm_ == val.firm_);
			check(restore.account_.at(0)->type_ == val.type_);		
		}
		{// load CLEARING_RECORDTYPE
			ClearingEntry val;
			val.id_ = IdT(1111, 6789);
			val.firm_ = "firm_";
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::CLEARING_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			ClearingCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());

			tst.onRecordLoaded(id, version, buf.c_str(), buf.size());
			check(1 == restore.instr_.size());
			check(1 == restore.string_.size());
			check(1 == restore.account_.size());
			check(1 == restore.clearing_.size());
			check(NULL != restore.clearing_.at(0));
			check(restore.clearing_.at(0)->id_ == val.id_);
			check(restore.clearing_.at(0)->firm_ == val.firm_);
		}
		{// load RAWDATA_RECORDTYPE
			RawDataEntry val;
			char cbuf[32] = "RawDataValue_";
			val.id_ = IdT(1111, 6789);
			val.data_ = cbuf;
			val.length_ = 13;
			val.type_ = STRING_RAWDATATYPE;
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::RAWDATA_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			RawDataCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());

			tst.onRecordLoaded(id, version, buf.c_str(), buf.size());
			check(1 == restore.instr_.size());
			check(1 == restore.string_.size());
			check(1 == restore.account_.size());
			check(1 == restore.clearing_.size());
			check(1 == restore.rawData_.size());
			check(NULL != restore.rawData_.at(0));
			check(restore.rawData_.at(0)->id_ == val.id_);
			check(restore.rawData_.at(0)->length_ == val.length_);
			check(restore.rawData_.at(0)->type_ == val.type_);
			check(0 == memcmp(restore.rawData_.at(0)->data_, val.data_, val.length_));
		}
		{// load ORDER_RECORDTYPE
			auto_ptr<OrderEntry> val(createOrder());
			val->orderId_ = IdT(1111, 6789);
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::ORDER_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			OrderCodec::encode(*(val.get()), &buf, &id, &version);
			check(!buf.empty());

			tst.onRecordLoaded(id, version, buf.c_str(), buf.size());
			check(1 == restore.instr_.size());
			check(1 == restore.string_.size());
			check(1 == restore.account_.size());
			check(1 == restore.clearing_.size());
			check(1 == restore.rawData_.size());
			check(1 == ordBook.orders_.size());
			check(NULL != ordBook.orders_.at(0));
			check(ordBook.orders_.at(0)->compare(*val));			
		}



		tst.finishLoad();

		WideDataStorage::destroy();

	}

	{/// save data into storage
		TestDataStorageRestore restore;
		TestOrderBook ordBook;
		TestFileSaver saver;
		OrderDataStorage ordStore;
		StorageRecordDispatcher tst;

		WideDataStorage::create();

		tst.init(&restore, &ordBook, &saver, &ordStore);	
		{// save InstrumentEntry
			InstrumentEntry val;
			val.id_ = IdT(1111, 6789);
			val.securityId_ = "securityId_";
			val.securityIdSource_ = "securityIdSource_";
			val.symbol_ = "symbol_";
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::INSTRUMENT_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			InstrumentCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());

			tst.save(val);
			check(1 == saver.records_.size());
			check(saver.records_.end() != saver.records_.find(id));
			check(0 == saver.records_.begin()->second.record_.compare(buf));
		}
		{// load StringT
			StringT val = "string_";
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::STRING_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id(1, 1234);
			u32 version = 0;
			StringTCodec::encode(val, &buf);
			check(!buf.empty());

			tst.save(id, val);
			check(2 == saver.records_.size());
			TestFileSaver::RecordsT::const_iterator it = saver.records_.find(id);
			check(saver.records_.end() != it);
			check(0 == it->second.record_.compare(buf));
		}
		{// load ACCOUNT_RECORDTYPE
			AccountEntry val;
			val.id_ = IdT(1112, 6789);
			val.account_ = "account_";
			val.firm_ = "firm_";
			val.type_ = PRINCIPAL_ACCOUNTTYPE;
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::ACCOUNT_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			AccountCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());

			tst.save(val);
			TestFileSaver::RecordsT::const_iterator it = saver.records_.find(id);
			check(saver.records_.end() != it);
			check(0 == it->second.record_.compare(buf));
		}
		{// load CLEARING_RECORDTYPE
			ClearingEntry val;
			val.id_ = IdT(1113, 6789);
			val.firm_ = "firm_";
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::CLEARING_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			ClearingCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());

			tst.save(val);
			TestFileSaver::RecordsT::const_iterator it = saver.records_.find(id);
			check(saver.records_.end() != it);
			check(0 == it->second.record_.compare(buf));
		}
		{// load RAWDATA_RECORDTYPE
			RawDataEntry val;
			char cbuf[32] = "RawDataValue_";
			val.id_ = IdT(1114, 6789);
			val.data_ = cbuf;
			val.length_ = 13;
			val.type_ = STRING_RAWDATATYPE;
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::RAWDATA_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			RawDataCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());

			tst.save(val);
			TestFileSaver::RecordsT::const_iterator it = saver.records_.find(id);
			check(saver.records_.end() != it);
			check(0 == it->second.record_.compare(buf));
		}
		{// load ORDER_RECORDTYPE
			auto_ptr<OrderEntry> val(createOrder());
			val->orderId_ = IdT(1115, 6789);
			std::string buf;
			{
				char typebuf[36];
				int t = StorageRecordDispatcher::ORDER_RECORDTYPE;
				memcpy(typebuf, &t, sizeof(t));
				buf.append(typebuf, sizeof(t));
			}
			IdT id;
			u32 version = 0;
			OrderCodec::encode(*(val.get()), &buf, &id, &version);
			check(!buf.empty());

			tst.save(*(val.get()));
			TestFileSaver::RecordsT::const_iterator it = saver.records_.find(id);
			check(saver.records_.end() != it);
			check(0 == it->second.record_.compare(buf));
		}

		tst.finishLoad();

		WideDataStorage::destroy();

	}

	return true;
}

