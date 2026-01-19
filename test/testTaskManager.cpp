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
#include <atomic>
#include <oneapi/tbb/mutex.h>

#include "TasksDef.h"
#include "TransactionDef.h"
#include "TransactionMgr.h"
#include "TaskManager.h"
#include "OrderStorage.h"
#include "Processor.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "IdTGenerator.h"
#include "OrderBookImpl.h"
#include <tbb/tick_count.h>
#include "ExchUtils.h"

using namespace std;
using namespace COP;
using namespace COP::Tasks;
using namespace COP::ACID;
using namespace COP::Store;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace tbb;
using namespace test;

namespace{

	SourceIdT addInstrument(const std::string &name){
		std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
		instr->symbol_ = name;
		instr->securityId_ = "AAA";
		instr->securityIdSource_ = "AAASrc";
		return WideDataStorage::instance()->add(instr.release());
	}


	std::unique_ptr<OrderEntry> createCorrectOrder(SourceIdT instr){
		SourceIdT srcId, destId, accountId, clearingId, clOrdId, origClOrderID, execList;

		{
			srcId = WideDataStorage::instance()->add(new StringT("CLNT"));
			destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));
			std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestClOrderId", 13));
			clOrdId = WideDataStorage::instance()->add(clOrd.release());

			std::unique_ptr<AccountEntry> account(new AccountEntry());
			account->type_ = PRINCIPAL_ACCOUNTTYPE;
			account->firm_ = "ACTFirm";
			account->account_ = "ACT";
			account->id_ = IdT();
			accountId = WideDataStorage::instance()->add(account.release());

			std::unique_ptr<ClearingEntry> clearing(new ClearingEntry());
			clearing->firm_ = "CLRFirm";
			clearingId = WideDataStorage::instance()->add(clearing.release());

			std::unique_ptr<ExecutionsT> execLst(new ExecutionsT());
			execList = WideDataStorage::instance()->add(execLst.release());
		}

		std::unique_ptr<OrderEntry> ptr(
			new OrderEntry(srcId, destId, clOrdId, origClOrderID, instr, accountId, clearingId, execList));
		
		ptr->creationTime_ = 100;
		ptr->lastUpdateTime_ = 115;
		ptr->expireTime_ = 175;
		ptr->settlDate_ = 225;
		ptr->price_ = 1.46;

		ptr->status_ = RECEIVEDNEW_ORDSTATUS;
		ptr->side_ = BUY_SIDE;
		ptr->ordType_ = LIMIT_ORDERTYPE;
		ptr->tif_ = DAY_TIF;
		ptr->settlType_ = _3_SETTLTYPE;
		ptr->capacity_ = PRINCIPAL_CAPACITY;
		ptr->currency_ = USD_CURRENCY;
		ptr->orderQty_ = 100;
		ptr->leavesQty_ = 100;

		return std::unique_ptr<OrderEntry>(ptr);
	}

	std::string getOrderId(int id){
		string val("TestClOrderId_");
		char buf[64];
		val += _itoa(id, buf, 10);
		return val;
	}
	void assignClOrderId(OrderEntry *order){
		static int id = 1;
		string val = getOrderId(++id);
		std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
		order->clOrderId_ = WideDataStorage::instance()->add(clOrd.release());
	}

	const int AMOUNT = 1000;//5000
	const int EVENT_AMOUNT = 6000 - 1;

	class TestOutQueues: public Queues::OutQueues{
	public:
		TestOutQueues(): events_(), es_(){events_.store(0);}

		virtual void push(const ExecReportEvent &, const std::string &){
			if(EVENT_AMOUNT == events_.fetch_add(1))
				es_ = tick_count::now();
			//mutex::scoped_lock lock(lock_);
			//cout<< "ExecReport "<< evnt.type_<< endl;
		}

		virtual void push(const CancelRejectEvent &, const std::string &){
			if(EVENT_AMOUNT == events_.fetch_add(1))
				es_ = tick_count::now();
			//mutex::scoped_lock lock(lock_);
			//cout<< "CancelReject "<< endl;
		}
		virtual void push(const BusinessRejectEvent &, const std::string &){
			if(EVENT_AMOUNT == events_.fetch_add(1))
				es_ = tick_count::now();
			//mutex::scoped_lock lock(lock_);
			//cout<< "BusinessReject "<< endl;
		}
		std::atomic<int> events_;
		tick_count es_;
		oneapi::tbb::mutex lock_;
	};
}


bool testTaskManager()
{
	DummyOrderSaver saver;
	WideDataStorage::create();
	IdTGenerator::create();
	OrderStorage::create();

	IncomingQueues inQueues;
	TestOutQueues outQueues;//OutgoingQueues outQueues;
	OrderBookImpl books;

	TransactionMgrParams transParams(IdTGenerator::instance());
	TransactionMgr transMgr;
	transMgr.init(transParams);
	ProcessorParams procParams(IdTGenerator::instance(), OrderStorage::instance(), 
                &books, &inQueues, &outQueues, &inQueues, &transMgr);
	OrderBookImpl::InstrumentsT instr;
	SourceIdT instrId1 = addInstrument("aaa");
	instr.insert(instrId1);
	SourceIdT instrId2 = addInstrument("bbb");
	instr.insert(instrId2);
	books.init(instr, &saver);

	tick_count bs;

	std::deque<RawDataEntry> orderIds;
	{
		TaskManagerParams params;
		{
			std::unique_ptr<Processor> proc(new Processor());
			proc->init(procParams);
			params.evntProcessors_.push_back(proc.release());
			std::unique_ptr<Processor> proc1(new Processor());
			proc1->init(procParams);
			params.evntProcessors_.push_back(proc1.release());
			std::unique_ptr<Processor> proc2(new Processor());
			proc2->init(procParams);
			params.evntProcessors_.push_back(proc2.release());
		}
		params.transactMgr_ = &transMgr;
		{
			std::unique_ptr<Processor> proc(new Processor());
			proc->init(procParams);
			params.transactProcessors_.push_back(proc.release());
			std::unique_ptr<Processor> proc1(new Processor());
			proc1->init(procParams);
			params.transactProcessors_.push_back(proc1.release());
			std::unique_ptr<Processor> proc2(new Processor());
			proc2->init(procParams);
			params.transactProcessors_.push_back(proc2.release());
		}
		params.inQueues_ = &inQueues;

		TaskManager manager(params);
		{
			bs = tick_count::now();
			for(int i = 0; i < AMOUNT; ++i){
				std::unique_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
				assignClOrderId(ord.get());
				orderIds.push_back(ord->clOrderId_.get());
				std::unique_ptr<OrderEntry> ord2(createCorrectOrder(instrId1));
				assignClOrderId(ord2.get());
				ord2->side_ = SELL_SIDE;
				orderIds.push_back(ord2->clOrderId_.get());

				inQueues.push("test", OrderEvent(ord.release()));
				inQueues.push("test", OrderEvent(ord2.release()));
			}
		}
		check(manager.waitUntilTransactionsFinished(60));
		cout<< "\t\tFinished benchmark test."<< endl;
		cout<< " Events enqueued: "<< outQueues.events_<< endl;
	}
	double diff = (outQueues.es_ - bs).seconds();
	cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)2*AMOUNT/diff<< " transactions/sec"<< endl;
	cout<< "\t\t AvgLatency is "<< diff/(AMOUNT*2)<< " sec"<< endl;

	cout<< "\t OrderIds enququed: "<< orderIds.size()<< endl;
/*	for(size_t i = 0; i < orderIds.size(); ++i){
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(orderIds[i]);
		cout<< " Status of order = "<< ord->stateMachinePersistance_.stateZone1Id_<< ", "<< 
			ord->stateMachinePersistance_.stateZone2Id_;//<< endl;
	}*/

	transMgr.stop();
		
	WideDataStorage::destroy();
	IdTGenerator::destroy();
	OrderStorage::destroy();

	return true;
}

