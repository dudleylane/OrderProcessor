/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TestAux.h"
#include "DataModelDef.h"
#include "SubscrManager.h"
#include "IdTGenerator.h"
#include "TransactionScope.h"
#include "StateMachineHelper.h"
#include <iostream>
#include "OrderStorage.h"
#include "QueuesDef.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "OrderBookImpl.h"
#include "TransactionMgr.h"
#include "Processor.h"
#include "Logger.h"
#include "TaskManager.h"
#include <tbb/tick_count.h>
#include "StorageRecordDispatcher.h"
#include "FileStorage.h"
#include "ExchUtils.h"

using namespace std;
using namespace COP;
using namespace COP::Store;
using namespace COP::OrdState;
using namespace COP::SubscrMgr;
using namespace COP::ACID;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace COP::Tasks;
using namespace COP::Store;
using namespace test;
using namespace tbb;

namespace{

	auto_ptr<OrderEntry> createCorrectOrder(SourceIdT instr){
		SourceIdT srcId, destId, accountId, clearingId, clOrdId, origClOrderID, execList;

		{
			srcId = WideDataStorage::instance()->add(new StringT("CLNT"));
			destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));
			auto_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestClOrderId", 13));
			clOrdId = WideDataStorage::instance()->add(clOrd.release());

			auto_ptr<AccountEntry> account(new AccountEntry());
			account->type_ = PRINCIPAL_ACCOUNTTYPE;
			account->firm_ = "ACTFirm";
			account->account_ = "ACT";
			account->id_ = IdT();
			accountId = WideDataStorage::instance()->add(account.release());

			auto_ptr<ClearingEntry> clearing(new ClearingEntry());
			clearing->firm_ = "CLRFirm";
			clearingId = WideDataStorage::instance()->add(clearing.release());

			auto_ptr<ExecutionsT> execLst(new ExecutionsT());
			execList = WideDataStorage::instance()->add(execLst.release());
		}

		auto_ptr<OrderEntry> ptr(
			new OrderEntry(srcId, destId, clOrdId, origClOrderID, instr, accountId, clearingId, execList));
		
		ptr->creationTime_ = 100;
		ptr->lastUpdateTime_ = 115;
		ptr->expireTime_ = 175;
		ptr->settlDate_ = 225;
		ptr->price_ = 1.46;

		ptr->status_ = RECEIVEDNEW_ORDSTATUS;
		ptr->side_ = BUY_SIDE;
		ptr->ordType_ = MARKET_ORDERTYPE;
		ptr->tif_ = DAY_TIF;
		ptr->settlType_ = _3_SETTLTYPE;
		ptr->capacity_ = PRINCIPAL_CAPACITY;
		ptr->currency_ = USD_CURRENCY;
		ptr->orderQty_ = 77;
		ptr->leavesQty_ = 77;

		return auto_ptr<OrderEntry>(ptr);
	}

	auto_ptr<TradeExecEntry> createTradeExec(const OrderEntry &order, const IdT &execId){
		auto_ptr<TradeExecEntry> ptr(new TradeExecEntry);
		
		ptr->execId_ = execId;
		ptr->orderId_ = order.orderId_;

		ptr->type_ = TRADE_EXECTYPE;
		ptr->orderStatus_ = PARTFILL_ORDSTATUS;
		ptr->transactTime_ = 1;

		ptr->lastQty_ = 100;
		ptr->lastPx_ = 10.25;
		ptr->currency_ = order.currency_;
		ptr->tradeDate_ = 1;

		OrderStorage::instance()->save(ptr.get());
		return auto_ptr<TradeExecEntry>(ptr);
	}

	auto_ptr<ExecCorrectExecEntry> createCorrectExec(const OrderEntry &order, const IdT &execId){
		auto_ptr<ExecCorrectExecEntry> ptr(new ExecCorrectExecEntry);

		ptr->execRefId_ = IdT();

		ptr->execId_ = execId;
		ptr->orderId_ = order.orderId_;


		ptr->type_ = TRADE_EXECTYPE;
		ptr->orderStatus_ = PARTFILL_ORDSTATUS;
		ptr->transactTime_ = 1;

		ptr->cumQty_ = 30;
		ptr->leavesQty_ = 20;
		ptr->lastQty_ = 10;
		ptr->lastPx_ = 10.34;
		ptr->currency_ = order.currency_;
		ptr->tradeDate_ = 1;

		OrderStorage::instance()->save(ptr.get());
		return auto_ptr<ExecCorrectExecEntry>(ptr);
	}

	auto_ptr<OrderEntry> createReplOrder(){
		SourceIdT srcId, destId, accountId, clearingId, instrument, clOrdId, origClOrderID, execList;

		{
			srcId = WideDataStorage::instance()->add(new StringT("CLNT1"));
			destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));
			auto_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestReplClOrderId", 17));
			clOrdId = WideDataStorage::instance()->add(clOrd.release());
			auto_ptr<RawDataEntry> origclOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestClOrderId", 13));
			origClOrderID = WideDataStorage::instance()->add(origclOrd.release());

			auto_ptr<AccountEntry> account(new AccountEntry());
			account->type_ = PRINCIPAL_ACCOUNTTYPE;
			account->firm_ = "ACTFirm";
			account->account_ = "ACT";
			account->id_ = IdT();
			accountId = WideDataStorage::instance()->add(account.release());

			auto_ptr<ClearingEntry> clearing(new ClearingEntry());
			clearing->firm_ = "CLRFirm";
			clearingId = WideDataStorage::instance()->add(clearing.release());

			auto_ptr<InstrumentEntry> instr(new InstrumentEntry());
			instr->symbol_ = "AAASymbl";
			instr->securityId_ = "AAA";
			instr->securityIdSource_ = "AAASrc";
			instrument = WideDataStorage::instance()->add(instr.release());

			auto_ptr<ExecutionsT> execLst(new ExecutionsT());
			execList = WideDataStorage::instance()->add(execLst.release());
		}

		auto_ptr<OrderEntry> ptr(
			new OrderEntry(srcId, destId, clOrdId, origClOrderID, instrument, accountId, clearingId, execList));
		
		ptr->creationTime_ = 130;
		ptr->lastUpdateTime_ = 135;
		ptr->expireTime_ = 185;
		ptr->settlDate_ = 225;
		ptr->price_ = 2.46;

		ptr->status_ = RECEIVEDNEW_ORDSTATUS;
		ptr->side_ = BUY_SIDE;
		ptr->ordType_ = MARKET_ORDERTYPE;
		ptr->tif_ = DAY_TIF;
		ptr->settlType_ = _3_SETTLTYPE;
		ptr->capacity_ = PRINCIPAL_CAPACITY;
		ptr->currency_ = USD_CURRENCY;
		ptr->orderQty_ = 300;

		//OrderStorage::instance()->save(*ptr.get());
		return auto_ptr<OrderEntry>(ptr);
	}

	void assignClOrderId(OrderEntry *order){
		static int id = 0;

		string val("TestClOrderId_");
		char buf[64];
		val += _itoa(++id, buf, 10);
		auto_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
		order->clOrderId_ = WideDataStorage::instance()->add(clOrd.release());
	}

	class TestInQueueObserver: public InQueueProcessor{
	public:
		virtual bool process(){
			return false;
		}

		virtual void onEvent(const std::string &source, const OrderEvent &evnt){
			orders_.push_back(OrderQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const OrderCancelEvent &evnt){
			orderCancels_.push_back(OrderCancelQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const OrderReplaceEvent &evnt){
			orderReplaces_.push_back(OrderReplaceQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const OrderChangeStateEvent &evnt){
			orderStates_.push_back(OrderStateQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const ProcessEvent &evnt){
			processes_.push_back(ProcessQueueT::value_type(source, evnt));
		}
		virtual void onEvent(const std::string &source, const TimerEvent &evnt){
			timers_.push_back(TimerQueueT::value_type(source, evnt));
		}
	public:
		typedef std::deque<std::pair<std::string, OrderEvent> > OrderQueueT;
		typedef std::deque<std::pair<std::string, OrderCancelEvent> > OrderCancelQueueT;
		typedef std::deque<std::pair<std::string, OrderReplaceEvent> > OrderReplaceQueueT;
		typedef std::deque<std::pair<std::string, OrderChangeStateEvent> > OrderStateQueueT;
		typedef std::deque<std::pair<std::string, ProcessEvent> > ProcessQueueT;
		typedef std::deque<std::pair<std::string, TimerEvent> > TimerQueueT;

		OrderQueueT orders_;
		OrderCancelQueueT orderCancels_;
		OrderReplaceQueueT orderReplaces_;
		OrderStateQueueT orderStates_;
		ProcessQueueT processes_;
		TimerQueueT timers_;
	};

	SourceIdT addInstrument(const std::string &name){
		auto_ptr<InstrumentEntry> instr(new InstrumentEntry());
		instr->symbol_ = name;
		instr->securityId_ = "AAA";
		instr->securityIdSource_ = "AAASrc";
		return WideDataStorage::instance()->add(instr.release());
	}

	const int AMOUNT = 2000;//5000
	const int EVENT_AMOUNT = 8000 - 1;

	class TestOutQueues: public Queues::OutQueues{
	public:
		TestOutQueues(): events_(), es_(){events_.fetch_and_store(0);}

		virtual void push(const ExecReportEvent &, const std::string &){
			if(EVENT_AMOUNT == events_.fetch_and_increment())
				es_ = tick_count::now();
		}

		virtual void push(const CancelRejectEvent &, const std::string &){
			if(EVENT_AMOUNT == events_.fetch_and_increment())
				es_ = tick_count::now();
		}
		virtual void push(const BusinessRejectEvent &, const std::string &){
			if(EVENT_AMOUNT == events_.fetch_and_increment())
				es_ = tick_count::now();
		}
		tbb::atomic<int> events_;
		tick_count es_;
		tbb::mutex lock_;
	};

}

bool testMarketOrder_NotMatched();
bool testIntegralBenchmark_OneProduct();
bool testIntegralBenchmark_2Products();

bool testIntegral()
{
	cout<< "Start test testIntegral..."<< endl;
	bool res = testMarketOrder_NotMatched()&&
			testIntegralBenchmark_OneProduct()&&
			testIntegralBenchmark_2Products();
	cout<< "Finished test testIntegral."<< endl;
	return res;
}

bool testIntegralBenchmark_2Products()
{
	cout<< "\tStart test testIntegralBenchmark_2Products..."<< endl;
	WideDataStorage::create();
	SubscriptionMgr::create();
	IdTGenerator::create();
	OrderStorage::create();

	aux::ExchLogger::instance()->setDebugOn(false);
	aux::ExchLogger::instance()->setNoteOn(false);

	StorageRecordDispatcher storeDisp;
	OrderDataStorage orderStorage;
	orderStorage.attach(&storeDisp);
	FileStorage::init();
	system("del integral.dat");
	FileStorage storage("integral.dat", &storeDisp);
	OrderBookImpl books;
	storeDisp.init(WideDataStorage::instance(), &books, &storage, &orderStorage);

	TestInQueueObserver obs;
	IncomingQueues inQueues;
	TestOutQueues outQueues;//OutgoingQueues outQueues;


	OrderBookImpl::InstrumentsT instr;
	SourceIdT instrId1 = addInstrument("aaa");
	instr.insert(instrId1);
	SourceIdT instrId2 = addInstrument("bbb");
	instr.insert(instrId2);
	books.init(instr, &storeDisp);

	TransactionMgrParams transParams(IdTGenerator::instance());
	TransactionMgr transMgr;
	transMgr.init(transParams);


	TaskManagerParams tmparams;
	{
		ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues, 
			&outQueues, &inQueues, &transMgr);
		auto_ptr<Processor> proc(new Processor());
		proc->init(params);
		tmparams.evntProcessors_.push_back(proc.release());

		auto_ptr<Processor> proc1(new Processor());
		proc1->init(params);
		tmparams.transactProcessors_.push_back(proc1.release());

		tmparams.transactMgr_ = &transMgr;
		tmparams.inQueues_ = &inQueues;
	}
	TaskManager manager(tmparams);

	std::deque<RawDataEntry> orderIds;
	tick_count bs;
	{
		bs = tick_count::now();
		for(int i = 0; i < AMOUNT/2; ++i){
			auto_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
			ord->ordType_ = LIMIT_ORDERTYPE;
			assignClOrderId(ord.get());
			orderIds.push_back(ord->clOrderId_.get());
			auto_ptr<OrderEntry> ord2(createCorrectOrder(instrId1));
			assignClOrderId(ord2.get());
			ord2->side_ = SELL_SIDE;
			ord2->ordType_ = LIMIT_ORDERTYPE;
			orderIds.push_back(ord2->clOrderId_.get());

			auto_ptr<OrderEntry> ord_1(createCorrectOrder(instrId2));
			ord_1->ordType_ = LIMIT_ORDERTYPE;
			assignClOrderId(ord_1.get());
			orderIds.push_back(ord_1->clOrderId_.get());
			auto_ptr<OrderEntry> ord2_1(createCorrectOrder(instrId2));
			assignClOrderId(ord2_1.get());
			ord2_1->side_ = SELL_SIDE;
			ord2_1->ordType_ = LIMIT_ORDERTYPE;
			orderIds.push_back(ord2_1->clOrderId_.get());

			inQueues.push("test", OrderEvent(ord.release()));
			inQueues.push("test", OrderEvent(ord_1.release()));
			inQueues.push("test", OrderEvent(ord2.release()));
			inQueues.push("test", OrderEvent(ord2_1.release()));
		}
		manager.waitUntilTransactionsFinished(6000);
		cout<< "\t\tFinished benchmark test."<< endl;
		cout<< " Events enqueued: "<< outQueues.events_<< endl;
	}
	double diff = (outQueues.es_ - bs).seconds();
	cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)2*AMOUNT/diff<< " transactions/sec"<< endl;
	cout<< "\t\t AvgLatency is "<< diff/(AMOUNT*2)<< " sec"<< endl;

	transMgr.stop();


	OrderStorage::destroy();
	SubscriptionMgr::destroy();
	WideDataStorage::destroy();
	IdTGenerator::destroy();

	cout<< "\tFinished test testIntegralBenchmark_2Products."<< endl;
	return true;
}


bool testIntegralBenchmark_OneProduct()
{
	cout<< "\tStart test testIntegralBenchmark_OneProduct..."<< endl;
	WideDataStorage::create();
	SubscriptionMgr::create();
	IdTGenerator::create();
	OrderStorage::create();

	aux::ExchLogger::instance()->setDebugOn(false);
	aux::ExchLogger::instance()->setNoteOn(false);

	StorageRecordDispatcher storeDisp;
	OrderDataStorage orderStorage;
	orderStorage.attach(&storeDisp);
	FileStorage::init();
	system("del integral.dat");
	FileStorage storage("integral.dat", &storeDisp);
	OrderBookImpl books;
	storeDisp.init(WideDataStorage::instance(), &books, &storage, &orderStorage);

	TestInQueueObserver obs;
	IncomingQueues inQueues;
	TestOutQueues outQueues;//OutgoingQueues outQueues;


	OrderBookImpl::InstrumentsT instr;
	SourceIdT instrId1 = addInstrument("aaa");
	instr.insert(instrId1);
	SourceIdT instrId2 = addInstrument("bbb");
	instr.insert(instrId2);
	books.init(instr, &storeDisp);

	TransactionMgrParams transParams(IdTGenerator::instance());
	TransactionMgr transMgr;
	transMgr.init(transParams);


	TaskManagerParams tmparams;
	{
		ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues, 
			&outQueues, &inQueues, &transMgr);
		auto_ptr<Processor> proc(new Processor());
		proc->init(params);
		tmparams.evntProcessors_.push_back(proc.release());

		auto_ptr<Processor> proc1(new Processor());
		proc1->init(params);
		tmparams.transactProcessors_.push_back(proc1.release());

		tmparams.transactMgr_ = &transMgr;
		tmparams.inQueues_ = &inQueues;
	}
	TaskManager manager(tmparams);

	std::deque<RawDataEntry> orderIds;
	tick_count bs;
	{
		bs = tick_count::now();
		for(int i = 0; i < AMOUNT; ++i){
			auto_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
			ord->ordType_ = LIMIT_ORDERTYPE;
			assignClOrderId(ord.get());
			orderIds.push_back(ord->clOrderId_.get());
			auto_ptr<OrderEntry> ord2(createCorrectOrder(instrId1));
			assignClOrderId(ord2.get());
			ord2->side_ = SELL_SIDE;
			ord2->ordType_ = LIMIT_ORDERTYPE;
			orderIds.push_back(ord2->clOrderId_.get());

			inQueues.push("test", OrderEvent(ord.release()));
			inQueues.push("test", OrderEvent(ord2.release()));
		}
		manager.waitUntilTransactionsFinished(6000);
		cout<< "\t\tFinished benchmark test."<< endl;
		cout<< " Events enqueued: "<< outQueues.events_<< endl;
	}
	double diff = (outQueues.es_ - bs).seconds();
	cout<< "\tFinishing... It takes "<< diff<< " sec, "<< (double)2*AMOUNT/diff<< " transactions/sec"<< endl;
	cout<< "\t\t AvgLatency is "<< diff/(AMOUNT*2)<< " sec"<< endl;

	transMgr.stop();


	OrderStorage::destroy();
	SubscriptionMgr::destroy();
	WideDataStorage::destroy();
	IdTGenerator::destroy();

	cout<< "\tFinished test testIntegralBenchmark_OneProduct."<< endl;
	return true;
}

bool testMarketOrder_NotMatched()
{
	cout<< "\tStart test testMarketOrder_NotMatched..."<< endl;
	WideDataStorage::create();
	SubscriptionMgr::create();
	IdTGenerator::create();
	OrderStorage::create();

	aux::ExchLogger::instance()->setDebugOn(true);
	aux::ExchLogger::instance()->setNoteOn(true);

	DummyOrderSaver saver;
	TestInQueueObserver obs;
	IncomingQueues inQueues;
	OutgoingQueues outQueues;

	OrderBookImpl books;
	OrderBookImpl::InstrumentsT instr;
	SourceIdT instrId1 = addInstrument("aaa");
	instr.insert(instrId1);
	SourceIdT instrId2 = addInstrument("bbb");
	instr.insert(instrId2);
	books.init(instr, &saver);

	TransactionMgrParams transParams(IdTGenerator::instance());
	TransactionMgr transMgr;
	transMgr.init(transParams);


	TaskManagerParams tmparams;
	{
		ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues, 
			&outQueues, &inQueues, &transMgr);
		auto_ptr<Processor> proc(new Processor());
		proc->init(params);
		tmparams.evntProcessors_.push_back(proc.release());

		auto_ptr<Processor> proc1(new Processor());
		proc1->init(params);
		tmparams.transactProcessors_.push_back(proc1.release());

		tmparams.transactMgr_ = &transMgr;
		tmparams.inQueues_ = &inQueues;
	}
	TaskManager manager(tmparams);

	{
		/// insert small limit order first
		auto_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
		RawDataEntry ordClOrdId = ord->clOrderId_.get();
		ord->ordType_ = LIMIT_ORDERTYPE;
		ord->orderQty_ = 5;
		ord->leavesQty_ = 5;
		ord->side_ = SELL_SIDE;
		inQueues.push("test", OrderEvent(ord.release()));

		{
			/// wait until order accepted or rejected (REJECTED_ORDSTATUS == order->status_)||(NEW_ORDSTATUS == order->status_)
			manager.waitUntilTransactionsFinished(5);
			OrderEntry *order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);
		}

		/// than add market order
		auto_ptr<OrderEntry> ord2(createCorrectOrder(instrId1));
		assignClOrderId(ord2.get());
		ord2->orderQty_ = 50;
		ord2->leavesQty_ = 50;
		RawDataEntry ord2ClOrdId = ord2->clOrderId_.get();
		inQueues.push("test", OrderEvent(ord2.release()));

		{
			/// wait until order canceled or rejected (REJECTED_ORDSTATUS == order->status_)||(CANCELED_ORDSTATUS == order->status_))
			manager.waitUntilTransactionsFinished(5);
			OrderEntry *order = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
			check(NULL != order);
			check(CANCELED_ORDSTATUS == order->status_);
		}
	}


	transMgr.stop();


	OrderStorage::destroy();
	SubscriptionMgr::destroy();
	WideDataStorage::destroy();
	IdTGenerator::destroy();

	cout<< "\tFinished test testMarketOrder_NotMatched."<< endl;
	return true;
}