/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TestAux.h"
#include "DataModelDef.h"
#include "Processor.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "IdTGenerator.h"
#include "OrderStorage.h"
#include "OrderBookImpl.h"
#include <iostream>

using namespace std;
using namespace COP;
using namespace COP::Queues;
using namespace COP::Proc;
using namespace COP::Store;
using namespace test;

namespace {
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

	class TestOrderFunctor: public OrderFunctor
	{
	public:
		TestOrderFunctor(const SourceIdT &instr, Side side, bool res):
			instr_(instr), res_(res)
		{
			side_ = side; 	
		}
		virtual SourceIdT instrument()const{return instr_;}
		virtual bool match(const IdT &)const{return res_;}
	public:
		SourceIdT instr_;
		bool res_;
	};

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
		ptr->ordType_ = LIMIT_ORDERTYPE;
		ptr->tif_ = DAY_TIF;
		ptr->settlType_ = _3_SETTLTYPE;
		ptr->capacity_ = PRINCIPAL_CAPACITY;
		ptr->currency_ = USD_CURRENCY;
		ptr->orderQty_ = 77;

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

	class TestTransactionManager: public ACID::TransactionManager{
	public:
		virtual void attach(ACID::TransactionObserver *)
		{}
		virtual ACID::TransactionObserver *detach()
		{
			return NULL;
		}
		virtual void addTransaction(std::auto_ptr<ACID::Transaction> &tr)
		{
			tr->setTransactionId(ACID::TransactionId(1, 1));
			proc_->process(tr->transactionId(), tr.get());
		}
		virtual bool removeTransaction(const ACID::TransactionId &, ACID::Transaction *tr)
		{
			return false;
		}
		virtual bool getParentTransactions(const ACID::TransactionId &, ACID::TransactionIdsT *)const
		{
			return false;
		}
		virtual bool getRelatedTransactions(const ACID::TransactionId &, ACID::TransactionIdsT *)const
		{
			return false;
		}
		virtual ACID::TransactionIterator *iterator()
		{
			return NULL;
		}
		Processor *proc_;
	};
}

bool testProcessor()
{
	{
		DummyOrderSaver saver;
		TestInQueueObserver obs;
		IncomingQueues inQueues;
		OutgoingQueues outQueues;
		TestTransactionManager transMgr;

		WideDataStorage::create();
		IdTGenerator::create();
		OrderStorage::create();
		OrderBookImpl books;
		OrderBookImpl::InstrumentsT instr;
		SourceIdT instrId1 = addInstrument("aaa");
		instr.insert(instrId1);
		SourceIdT instrId2 = addInstrument("bbb");
		instr.insert(instrId2);
		books.init(instr, &saver);

		ProcessorParams params(IdTGenerator::instance(), OrderStorage::instance(), &books, &inQueues, 
			&outQueues, &inQueues, &transMgr);
		Processor proc;
		proc.init(params);
		transMgr.proc_ = &proc;
		{
			auto_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
			RawDataEntry ordClOrdId = ord->clOrderId_.get();
			inQueues.push("test", OrderEvent(ord.release()));
			auto_ptr<OrderEntry> ord2(createCorrectOrder(instrId1));
			assignClOrderId(ord2.get());
			RawDataEntry ord2ClOrdId = ord2->clOrderId_.get();
			inQueues.push("test", OrderEvent(ord2.release()));

			proc.process();
			OrderEntry *order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);
			order = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
			check(NULL == order);

			proc.process();
			order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);
			order = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);

			proc.process();
			order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);
			order = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);

			proc.process();
			order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);
			order = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);

		}

		{
			auto_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
			assignClOrderId(ord.get());
			ord->side_ = SELL_SIDE;
			ord->leavesQty_ = 100;
			ord->price_ = 10.0;
			ord->ordType_ = LIMIT_ORDERTYPE;
			RawDataEntry ordClOrdId = ord->clOrderId_.get();
			inQueues.push("test", OrderEvent(ord.release()));

			proc.process();
			OrderEntry *order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);

			proc.process();
			order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
			check(NULL != order);
			check(NEW_ORDSTATUS == order->status_);

			auto_ptr<OrderEntry> ord2(createCorrectOrder(instrId1));
			assignClOrderId(ord2.get());
			ord2->side_ = BUY_SIDE;
			ord2->leavesQty_ = 50;
			ord2->price_ = 20.0;
			ord2->ordType_ = LIMIT_ORDERTYPE;
			RawDataEntry ord2ClOrdId = ord2->clOrderId_.get();
			inQueues.push("test", OrderEvent(ord2.release()));

			proc.process();
			order = OrderStorage::instance()->locateByClOrderId(ord2ClOrdId);
			check(NULL != order);
			check(FILLED_ORDSTATUS == order->status_);
			order = OrderStorage::instance()->locateByClOrderId(ordClOrdId);
			check(NULL != order);
			check(PARTFILL_ORDSTATUS == order->status_);
		}
		OrderStorage::destroy();
		IdTGenerator::destroy();
		WideDataStorage::destroy();
	}
	return true;
}