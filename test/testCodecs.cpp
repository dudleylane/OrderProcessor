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

#include "InstrumentCodec.h"
#include "StringTCodec.h"
#include "AccountCodec.h"
#include "ClearingCodec.h"
#include "RawDataCodec.h"
#include "OrderCodec.h"

#include "WideDataStorage.h"

using namespace std;
using namespace COP;
using namespace COP::Codec;
using namespace COP::Store;
using namespace aux;
using namespace tbb;
using namespace test;

namespace{
	SourceIdT addInstrument(const std::string &name){
		auto_ptr<InstrumentEntry> instr(new InstrumentEntry());
		instr->symbol_ = name;
		instr->securityId_ = "AAA";
		instr->securityIdSource_ = "AAASrc";
		return WideDataStorage::instance()->add(instr.release());
	}

	bool testInstrumentCodec(){
		{// encode/decode empty type
			InstrumentEntry val;
			std::string buf;
			IdT id;
			u32 version = 0;
			InstrumentCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.id_);

			InstrumentEntry decVal;
			InstrumentCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);
			check(decVal.id_ == val.id_);
			check(decVal.securityId_ == val.securityId_);
			check(decVal.securityId_.empty());
			check(decVal.securityIdSource_ == val.securityIdSource_);
			check(decVal.securityIdSource_.empty());
			check(decVal.symbol_ == val.symbol_);
			check(decVal.symbol_.empty());
		}
		{// encode/decode filled type
			InstrumentEntry val;
			val.id_ = IdT(1234, 6789);
			val.securityId_ = "securityId_";
			val.securityIdSource_ = "securityIdSource_";
			val.symbol_ = "symbol_";
			std::string buf;
			IdT id;
			u32 version = 0;
			InstrumentCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.id_);

			InstrumentEntry decVal;
			InstrumentCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);
			check(decVal.id_ == val.id_);
			check(decVal.securityId_ == val.securityId_);
			check(decVal.securityIdSource_ == val.securityIdSource_);
			check(decVal.symbol_ == val.symbol_);		
		}
		return true;
	}

	bool testStringTCodec(){
		{// encode/decode empty type
			StringT val;
			std::string buf;
			StringTCodec::encode(val, &buf);
			check(!buf.empty());

			StringT decVal;
			StringTCodec::decode(buf.c_str(), buf.size(), &decVal);
			check(decVal == val);
			check(decVal.empty());
		}
		{// encode/decode filled type
			StringT val;
			val = "value_";
			std::string buf;
			StringTCodec::encode(val, &buf);
			check(!buf.empty());

			StringT decVal;
			StringTCodec::decode(buf.c_str(), buf.size(), &decVal);
			check(decVal == val);
		}
		return true;
	}

	bool testAccountCodec(){
		{// encode/decode empty type
			AccountEntry val;
			std::string buf;
			IdT id;
			u32 version = 0;
			val.type_ = INVALID_ACCOUNTTYPE;
			AccountCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.id_);

			AccountEntry decVal;
			AccountCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);
			check(decVal.id_ == val.id_);
			check(decVal.account_ == val.account_);
			check(decVal.account_.empty());
			check(decVal.firm_ == val.firm_);
			check(decVal.firm_.empty());
			check(decVal.type_ == val.type_);
			check(INVALID_ACCOUNTTYPE == decVal.type_);
		}
		{// encode/decode filled type
			AccountEntry val;
			val.id_ = IdT(1234, 6789);
			val.account_ = "account_";
			val.firm_ = "firm_";
			val.type_ = AGENCY_ACCOUNTTYPE;
			std::string buf;
			IdT id;
			u32 version = 0;
			AccountCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.id_);

			AccountEntry decVal;
			AccountCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);
			check(decVal.id_ == val.id_);
			check(decVal.account_ == val.account_);
			check(decVal.firm_ == val.firm_);
			check(decVal.type_ == val.type_);		
		}
		return true;	
	}

	bool testClearingCodec(){
		{// encode/decode empty type
			ClearingEntry val;
			std::string buf;
			IdT id;
			u32 version = 0;
			ClearingCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.id_);

			ClearingEntry decVal;
			ClearingCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);
			check(decVal.id_ == val.id_);
			check(decVal.firm_ == val.firm_);
			check(decVal.firm_.empty());
		}
		{// encode/decode filled type
			ClearingEntry val;
			val.id_ = IdT(1234, 6789);
			val.firm_ = "firm_";
			std::string buf;
			IdT id;
			u32 version = 0;
			ClearingCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.id_);

			ClearingEntry decVal;
			ClearingCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);
			check(decVal.id_ == val.id_);
			check(decVal.firm_ == val.firm_);
		}
		return true;	
	}

	bool testRawDataCodec(){
		{// encode/decode empty type
			RawDataEntry val;
			std::string buf;
			IdT id;
			u32 version = 0;
			val.type_ = INVALID_RAWDATATYPE;
			val.data_ = NULL;
			val.length_ = 0;
			RawDataCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.id_);

			RawDataEntry decVal;
			RawDataCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);
			check(decVal.id_ == val.id_);
			check(decVal.type_ == val.type_);
			check(decVal.data_ == val.data_);
			check(decVal.length_ == val.length_);
		}
		{// encode/decode filled type
			RawDataEntry val;
			val.id_ = IdT(1234, 6789);
			std::string buf;
			IdT id;
			u32 version = 0;
			val.type_ = STRING_RAWDATATYPE;
			char b[64] = "data_";
			val.data_ = &b[0];
			val.length_ = 5;
			RawDataCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.id_);

			RawDataEntry decVal;
			RawDataCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);
			check(decVal.id_ == val.id_);
			check(decVal.type_ == val.type_);
			check(decVal.length_ == val.length_);
			check(0 == memcmp(decVal.data_, val.data_, val.length_));
			delete [] decVal.data_;
		}
		return true;
	}

	bool testOrderCodec(){
		WideDataStorage::create();
		{// encode/decode empty type
			SourceIdT instrId = addInstrument("aaa");
			SourceIdT v;
			OrderEntry val(v, v, v, v, instrId, v, v, v);
			std::string buf;
			IdT id;
			u32 version = 0;
			val.creationTime_ = 0;
			val.lastUpdateTime_ = 0;
			val.expireTime_ = 0;
			val.settlDate_ = 0;
			val.price_ = 0;
			val.stopPx_ = 0;
			val.avgPx_ = 0;
			val.dayAvgPx_ = 0;
			val.status_ = INVALID_ORDSTATUS;
			val.side_ = INVALID_SIDE;
			val.ordType_ = INVALID_ORDERTYPE;
			val.tif_ = INVALID_TIF;
			val.settlType_ = INVALID_SETTLTYPE;
			val.capacity_ = INVALID_CAPACITY;
			val.currency_ = INVALID_CURRENCY;
			val.minQty_ = 0;
			val.orderQty_ = 0;
			val.leavesQty_ = 0;
			val.cumQty_ = 0;
			val.dayOrderQty_ = 0;
			val.dayCumQty_ = 0;
			val.stateMachinePersistance_.orderData_ = NULL;
			val.stateMachinePersistance_.stateZone1Id_ = 0;
			val.stateMachinePersistance_.stateZone2Id_ = 0;
			OrderCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.orderId_);

			auto_ptr<OrderEntry> decVal(OrderCodec::decode(id, version, buf.c_str(), buf.size()));
			check(NULL != decVal.get());
			check(decVal->instrument_.getId() == val.instrument_.getId());
			check(decVal->account_.getId() == val.account_.getId());
			check(decVal->clearing_.getId() == val.clearing_.getId());
			check(decVal->destination_.getId() == val.destination_.getId());
			check(decVal->execInstruct_ == val.execInstruct_);
			check(decVal->creationTime_ == val.creationTime_);
			check(decVal->lastUpdateTime_ == val.lastUpdateTime_);
			check(decVal->expireTime_ == val.expireTime_);
			check(decVal->settlDate_ == val.settlDate_);
			check(decVal->price_ == val.price_);
			check(decVal->stopPx_ == val.stopPx_);
			check(decVal->avgPx_ == val.avgPx_);
			check(decVal->dayAvgPx_ == val.dayAvgPx_);
			check(decVal->status_ == val.status_);
			check(decVal->side_ == val.side_);
			check(decVal->ordType_ == val.ordType_);
			check(decVal->tif_ == val.tif_);
			check(decVal->settlType_ == val.settlType_);
			check(decVal->capacity_ == val.capacity_);
			check(decVal->currency_ == val.currency_);
			check(decVal->minQty_ == val.minQty_);
			check(decVal->orderQty_ == val.orderQty_);
			check(decVal->leavesQty_ == val.leavesQty_);
			check(decVal->cumQty_ == val.cumQty_);
			check(decVal->dayOrderQty_ == val.dayOrderQty_);
			check(decVal->dayCumQty_ == val.dayCumQty_);
			check(decVal->clOrderId_.getId() == val.clOrderId_.getId());
			check(decVal->origClOrderId_.getId() == val.origClOrderId_.getId());
			check(decVal->source_.getId() == val.source_.getId());
			check(decVal->orderId_ == val.orderId_);
			check(decVal->origOrderId_ == val.origOrderId_);
			check(decVal->executions_.getId() == val.executions_.getId());
			check(decVal->stateMachinePersistance_.orderData_ == val.stateMachinePersistance_.orderData_);
			check(decVal->stateMachinePersistance_.stateZone1Id_ == val.stateMachinePersistance_.stateZone1Id_);
			check(decVal->stateMachinePersistance_.stateZone2Id_ == val.stateMachinePersistance_.stateZone2Id_);
		}
		{// encode/decode filled type
			SourceIdT v;
			SourceIdT instrId = addInstrument("instrument");
			OrderEntry val(SourceIdT(1, 1), SourceIdT(2, 2), SourceIdT(3, 3), SourceIdT(4, 4), instrId, 
							SourceIdT(6, 6), SourceIdT(7, 7), SourceIdT(8, 8));
			std::string buf;
			IdT id;
			u32 version = 0;

			val.creationTime_ = 1111;
			val.lastUpdateTime_ = 1112;
			val.expireTime_ = 1113;
			val.settlDate_ = 1114;
			val.price_ = 22.22;
			val.stopPx_ = 23.23;
			val.avgPx_ = 24.24;
			val.dayAvgPx_ = 24.24;
			val.status_ = REJECTED_ORDSTATUS;
			val.side_ = BUY_SIDE;
			val.ordType_ = LIMIT_ORDERTYPE;
			val.tif_ = GTD_TIF;
			val.settlType_ = _4_SETTLTYPE;
			val.capacity_ = PROPRIETARY_CAPACITY;
			val.currency_ = USD_CURRENCY;
			val.minQty_ = 3333;
			val.orderQty_ = 3334;
			val.leavesQty_ = 3335;
			val.cumQty_ = 3336;
			val.dayOrderQty_ = 3337;
			val.dayCumQty_ = 3338;
			val.orderId_ = IdT(4444, 4445);
			val.origOrderId_ = IdT(5555, 5556);
			val.stateMachinePersistance_.orderData_ = &val;
			val.stateMachinePersistance_.stateZone1Id_ = 6;
			val.stateMachinePersistance_.stateZone2Id_ = 7;

			OrderCodec::encode(val, &buf, &id, &version);
			check(!buf.empty());
			check(id == val.orderId_);

			auto_ptr<OrderEntry> decVal(OrderCodec::decode(id, version, buf.c_str(), buf.size()));
			check(NULL != decVal.get());
			check(decVal->instrument_.getId() == val.instrument_.getId());
			check(decVal->account_.getId() == val.account_.getId());
			check(decVal->clearing_.getId() == val.clearing_.getId());
			check(decVal->destination_.getId() == val.destination_.getId());
			check(decVal->execInstruct_ == val.execInstruct_);
			check(decVal->creationTime_ == val.creationTime_);
			check(decVal->lastUpdateTime_ == val.lastUpdateTime_);
			check(decVal->expireTime_ == val.expireTime_);
			check(decVal->settlDate_ == val.settlDate_);
			check(decVal->price_ == val.price_);
			check(decVal->stopPx_ == val.stopPx_);
			check(decVal->avgPx_ == val.avgPx_);
			check(decVal->dayAvgPx_ == val.dayAvgPx_);
			check(decVal->status_ == val.status_);
			check(decVal->side_ == val.side_);
			check(decVal->ordType_ == val.ordType_);
			check(decVal->tif_ == val.tif_);
			check(decVal->settlType_ == val.settlType_);
			check(decVal->capacity_ == val.capacity_);
			check(decVal->currency_ == val.currency_);
			check(decVal->minQty_ == val.minQty_);
			check(decVal->orderQty_ == val.orderQty_);
			check(decVal->leavesQty_ == val.leavesQty_);
			check(decVal->cumQty_ == val.cumQty_);
			check(decVal->dayOrderQty_ == val.dayOrderQty_);
			check(decVal->dayCumQty_ == val.dayCumQty_);
			check(decVal->clOrderId_.getId() == val.clOrderId_.getId());
			check(decVal->origClOrderId_.getId() == val.origClOrderId_.getId());
			check(decVal->source_.getId() == val.source_.getId());
			check(decVal->orderId_ == val.orderId_);
			check(decVal->origOrderId_ == val.origOrderId_);
			check(decVal->executions_.getId() == val.executions_.getId());
			check(NULL == decVal->stateMachinePersistance_.orderData_);
			check(decVal->stateMachinePersistance_.stateZone1Id_ == val.stateMachinePersistance_.stateZone1Id_);
			check(decVal->stateMachinePersistance_.stateZone2Id_ == val.stateMachinePersistance_.stateZone2Id_);
		}
		WideDataStorage::destroy();
		return true;
	}
}

bool testCodecs()
{
	return testInstrumentCodec()&&
		testStringTCodec()&&
		testAccountCodec()&&
		testClearingCodec()&&
		testRawDataCodec()&&
		testOrderCodec();
}

