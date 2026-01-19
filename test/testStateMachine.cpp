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

using namespace std;
using namespace COP;
using namespace COP::Store;
using namespace COP::OrdState;
using namespace COP::SubscrMgr;
using namespace COP::ACID;
using namespace test;

namespace{
	template<typename T>
	T eventForTestStateMachineOnly(T *, bool checkRez = true)
	{
		T evnt;	
		evnt.testStateMachine_ = true;
		evnt.testStateMachineCheckResult_ = checkRez;
		return evnt;
	}
	template<typename T, typename P>
	T eventForTestStateMachineOnly(T *, const P &param, bool checkRez = true)
	{
		T evnt(param);	
		evnt.testStateMachine_ = true;
		evnt.testStateMachineCheckResult_ = checkRez;
		return evnt;
	}
	template<typename T, typename P, typename P1>
	T eventForTestStateMachineOnly(T *, const P &param, const P1 &param2, bool checkRez = true)
	{
		T evnt(param, param2);	
		evnt.testStateMachine_ = true;
		evnt.testStateMachineCheckResult_ = checkRez;
		return evnt;
	}

	std::unique_ptr<OrderEntry> createCorrectOrder(){
		SourceIdT srcId, destId, accountId, clearingId, instrument, clOrdId, origClOrderID, execList;

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

			std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
			instr->symbol_ = "AAASymbl";
			instr->securityId_ = "AAA";
			instr->securityIdSource_ = "AAASrc";
			instrument = WideDataStorage::instance()->add(instr.release());

			std::unique_ptr<ExecutionsT> execLst(new ExecutionsT());
			execList = WideDataStorage::instance()->add(execLst.release());
		}

		std::unique_ptr<OrderEntry> ptr(
			new OrderEntry(srcId, destId, clOrdId, origClOrderID, instrument, accountId, clearingId, execList));
		
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

		//OrderStorage::instance()->save(*ptr.get());
		return std::unique_ptr<OrderEntry>(ptr);
	}

	std::unique_ptr<TradeExecEntry> createTradeExec(const OrderEntry &order, const IdT &execId){
		std::unique_ptr<TradeExecEntry> ptr(new TradeExecEntry);
		
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
		return std::unique_ptr<TradeExecEntry>(ptr);
	}

	std::unique_ptr<ExecCorrectExecEntry> createCorrectExec(const OrderEntry &order, const IdT &execId){
		std::unique_ptr<ExecCorrectExecEntry> ptr(new ExecCorrectExecEntry);

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
		return std::unique_ptr<ExecCorrectExecEntry>(ptr);
	}

	std::unique_ptr<OrderEntry> createReplOrder(){
		SourceIdT srcId, destId, accountId, clearingId, instrument, clOrdId, origClOrderID, execList;

		{
			srcId = WideDataStorage::instance()->add(new StringT("CLNT1"));
			destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));
			std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestReplClOrderId", 17));
			clOrdId = WideDataStorage::instance()->add(clOrd.release());
			std::unique_ptr<RawDataEntry> origclOrd(new RawDataEntry(STRING_RAWDATATYPE, "TestClOrderId", 13));
			origClOrderID = WideDataStorage::instance()->add(origclOrd.release());

			std::unique_ptr<AccountEntry> account(new AccountEntry());
			account->type_ = PRINCIPAL_ACCOUNTTYPE;
			account->firm_ = "ACTFirm";
			account->account_ = "ACT";
			account->id_ = IdT();
			accountId = WideDataStorage::instance()->add(account.release());

			std::unique_ptr<ClearingEntry> clearing(new ClearingEntry());
			clearing->firm_ = "CLRFirm";
			clearingId = WideDataStorage::instance()->add(clearing.release());

			std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
			instr->symbol_ = "AAASymbl";
			instr->securityId_ = "AAA";
			instr->securityIdSource_ = "AAASrc";
			instrument = WideDataStorage::instance()->add(instr.release());

			std::unique_ptr<ExecutionsT> execLst(new ExecutionsT());
			execList = WideDataStorage::instance()->add(execLst.release());
		}

		std::unique_ptr<OrderEntry> ptr(
			new OrderEntry(srcId, destId, clOrdId, origClOrderID, instrument, accountId, clearingId, execList));
		
		ptr->creationTime_ = 130;
		ptr->lastUpdateTime_ = 135;
		ptr->expireTime_ = 185;
		ptr->settlDate_ = 225;
		ptr->price_ = 2.46;

		ptr->status_ = RECEIVEDNEW_ORDSTATUS;
		ptr->side_ = BUY_SIDE;
		ptr->ordType_ = LIMIT_ORDERTYPE;
		ptr->tif_ = DAY_TIF;
		ptr->settlType_ = _3_SETTLTYPE;
		ptr->capacity_ = PRINCIPAL_CAPACITY;
		ptr->currency_ = USD_CURRENCY;
		ptr->orderQty_ = 300;

		//OrderStorage::instance()->save(*ptr.get());
		return std::unique_ptr<OrderEntry>(ptr);
	}

	void assignClOrderId(OrderEntry *order){
		static int id = 0;

		string val("TestClOrderId_");
		char buf[64];
		val += _itoa(++id, buf, 10);
		std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
		order->clOrderId_ = WideDataStorage::instance()->add(clOrd.release());
	}

	class TestOrderBook: public OrderBook{
	public:
		void add(const OrderEntry&){}
		void remove(const OrderEntry&){}
		IdT find(const OrderFunctor &)const{return IdT();}
		void findAll(const OrderFunctor &, OrdersT *)const{}
		IdT getTop(const SourceIdT &, const Side &)const{return IdT();}
		void restore(const OrderEntry&){}
	};

}

bool testStateMachineOnly();

bool testStateMachine()
{
	if(!testStateMachineOnly())
		return false;

	cout<< "Start test testStateMachine..."<< endl;

	WideDataStorage::create();
	SubscriptionMgr::create();
	IdTGenerator::create();
	OrderStorage::create();
	std::unique_ptr<OrderEntry> order(createCorrectOrder());
	TestOrderBook books;

	{//Rcvd_New->Pend_New->Rejected
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
			recvevnt.order_->side_ = INVALID_SIDE;
		}
		p.processEvent(recvevnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(CREATE_REJECT_EXECREPORT_TROPERATION == trCntxt.op_.front()->type());
	}

	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->Filled->New(Crct)->Rejected
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
			recvevnt.order_->side_ = BUY_SIDE;
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(101, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
			check(nullptr != ord);
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 50;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		{
			tradeevnt.orderId_ = order->orderId_;
			OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
			check(nullptr != ord);
			ord->leavesQty_ = 50;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 20;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		{
			tradeevnt.orderId_ = order->orderId_;
			OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
			check(nullptr != ord);
			ord->leavesQty_ = 30;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 30;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(100, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
			check(nullptr != ord);
			ord->leavesQty_ = 0;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 20;
			correctParams1->cumQty_ = order->cumQty_;
			correctParams1->leavesQty_ = 10;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(10 == OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get())->leavesQty_);
		trCntxt.clear();

		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 30;
			correctParams1->leavesQty_ = 40;
			OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
			check(nullptr != ord);
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(40 == OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get())->leavesQty_);
		trCntxt.clear();

		{
			tradeCrctevnt.orderId_ = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get())->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			check(nullptr != ord);
			correctParams1->lastQty_ = 30;
			correctParams1->cumQty_ = 0;
			correctParams1->leavesQty_ = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get())->orderQty_;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("New", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(ord->orderQty_ == ord->leavesQty_);
		check(0 == OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get())->cumQty_);
		trCntxt.clear();

		{
			tradeevnt.orderId_ = order->orderId_;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = ord->leavesQty_;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		check(nullptr != ord);
		check(0 == ord->leavesQty_);
		check(ord->orderQty_ == ord->cumQty_);
		trCntxt.clear();

		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 0;
			correctParams1->cumQty_ = 0;
			correctParams1->leavesQty_ = ord->orderQty_;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(ord->orderQty_ == ord->leavesQty_);
		check(0 == ord->cumQty_);
		trCntxt.clear();

		onOrderRejected reject;
		{
			reject.orderId_ = order->orderId_;
			reject.generator_ = IdTGenerator::instance();
			reject.transaction_ = &trCntxt;
			reject.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(reject); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}

	{//orig: Rcvd_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->Filled->New(Crct)->Expired
	 //simpl: Rcvd_New->New->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
			recvevnt.order_->side_ = BUY_SIDE;
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//orig: Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->
	 //		New(Crct)->Filled->New(Crct)->DoneForDay->New->Suspended->New->Expired
	 //smpl: Rcvd_New->Pend_New->New->Filled->New(Crct)->DoneForDay->New->Suspended->New->Expired

		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
			recvevnt.order_->side_ = BUY_SIDE;
			recvevnt.order_->leavesQty_ = recvevnt.order_->orderQty_;
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(106, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = ord->leavesQty_;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		check(0 == ord->leavesQty_);
		check(ord->orderQty_ == ord->cumQty_);
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(107, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 0;
			correctParams1->cumQty_ = 0;
			correctParams1->leavesQty_ = ord->orderQty_;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(ord->orderQty_ == ord->leavesQty_);
		check(0 == ord->cumQty_);
		trCntxt.clear();

		onFinished dfd;
		{
			dfd.orderId_ = order->orderId_;
			dfd.generator_ = IdTGenerator::instance();
			dfd.transaction_ = &trCntxt;
			dfd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(dfd); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

		onNewDay nd;
		{
			nd.orderId_ = order->orderId_;
			nd.generator_ = IdTGenerator::instance();
			nd.transaction_ = &trCntxt;
			nd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(nd); 
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();

		onSuspended susp;
		{
			susp.orderId_ = order->orderId_;
			susp.generator_ = IdTGenerator::instance();
			susp.transaction_ = &trCntxt;
			susp.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(susp); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

		onContinue cont;
		{
			cont.orderId_ = order->orderId_;
			cont.generator_ = IdTGenerator::instance();
			cont.transaction_ = &trCntxt;
			cont.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cont); 
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//orig: Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//		Filled->New(Crct)->DoneForDay->New->Suspended->Expired
	//smpl: Rcvd_New->Pend_New->New->Suspended->Expired

		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
			recvevnt.order_->side_ = BUY_SIDE;
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onSuspended susp;
		{
			susp.orderId_ = order->orderId_;
			susp.generator_ = IdTGenerator::instance();
			susp.transaction_ = &trCntxt;
			susp.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(susp); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}

	{//orig: Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//		Filled->New(Crct)->DoneForDay->New->Suspended->DoneForDay->Suspended->Expired
	//smpl: Rcvd_New->Pend_New->New->Suspended->DoneForDay->Suspended->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
			recvevnt.order_->side_ = BUY_SIDE;
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onSuspended susp;
		{
			susp.orderId_ = order->orderId_;
			susp.generator_ = IdTGenerator::instance();
			susp.transaction_ = &trCntxt;
			susp.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(susp); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

		onFinished dfd;
		{
			dfd.orderId_ = order->orderId_;
			dfd.generator_ = IdTGenerator::instance();
			dfd.transaction_ = &trCntxt;
			dfd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(dfd); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		{
			susp.orderId_ = order->orderId_;
			susp.generator_ = IdTGenerator::instance();
			susp.transaction_ = &trCntxt;
			susp.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(susp); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//orig: Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//		Filled->New(Crct)->DoneForDay->New->Rejected
		//smpl: Rcvd_New->Pend_New->New->DoneForDay->New->Rejected
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
			recvevnt.order_->side_ = BUY_SIDE;
			recvevnt.order_->leavesQty_ = 100;
			recvevnt.order_->orderQty_ = 100;
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onFinished dfd;
		{
			dfd.orderId_ = order->orderId_;
			dfd.generator_ = IdTGenerator::instance();
			dfd.transaction_ = &trCntxt;
			dfd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(dfd); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

		onNewDay nd;
		{
			nd.orderId_ = order->orderId_;
			nd.generator_ = IdTGenerator::instance();
			nd.transaction_ = &trCntxt;
			nd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(nd); 
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();

		onOrderRejected reject;
		{
			reject.orderId_ = order->orderId_;
			reject.generator_ = IdTGenerator::instance();
			reject.transaction_ = &trCntxt;
			reject.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(reject); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}





	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//	Filled->Expired
	//smpl: Rcvd_New->Pend_New->New->Filled->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(111, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 100;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//	Filled->DoneForDay
	//smpl: Rcvd_New->Pend_New->New->Filled->DoneForDay
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(121, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 100;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onFinished dfd;
		{
			dfd.orderId_ = order->orderId_;
			dfd.generator_ = IdTGenerator::instance();
			dfd.transaction_ = &trCntxt;
			dfd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(dfd); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//	Filled->DoneForDay->DoneForDay(Crct)->New
	//smpl: Rcvd_New->Pend_New->New->Filled->DoneForDay->DoneForDay(Crct)->New
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(131, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 100;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		check(0 == ord->leavesQty_);
		trCntxt.clear();

		onFinished dfd;
		{
			dfd.orderId_ = order->orderId_;
			dfd.generator_ = IdTGenerator::instance();
			dfd.transaction_ = &trCntxt;
			dfd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(dfd); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(137, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 0;
			correctParams1->cumQty_ = 0;
			correctParams1->leavesQty_ = ord->orderQty_;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(ord->orderQty_ == ord->leavesQty_);
		check(0 == order->cumQty_);
		trCntxt.clear();

		onNewDay nd;
		{
			nd.orderId_ = order->orderId_;
			nd.generator_ = IdTGenerator::instance();
			nd.transaction_ = &trCntxt;
			nd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(nd); 
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//		Filled->DoneForDay->DoneForDay(Crct)->PartFill
	//smpl: Rcvd_New->Pend_New->New->Filled->DoneForDay->DoneForDay(Crct)->PartFill
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(133, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 100;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onFinished dfd;
		{
			dfd.orderId_ = order->orderId_;
			dfd.generator_ = IdTGenerator::instance();
			dfd.transaction_ = &trCntxt;
			dfd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(dfd); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(138, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 30;
			correctParams1->leavesQty_ = 40;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(40 == ord->leavesQty_);
		trCntxt.clear();

		onNewDay nd;
		{
			nd.orderId_ = order->orderId_;
			nd.generator_ = IdTGenerator::instance();
			nd.transaction_ = &trCntxt;
			nd.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(nd); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();
	}

	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
		//	Filled->Suspended
	//smpl: Rcvd_New->Pend_New->New->Filled->Suspended
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(233, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 100;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onSuspended susp;
		{
			susp.orderId_ = order->orderId_;
			susp.generator_ = IdTGenerator::instance();
			susp.transaction_ = &trCntxt;
			susp.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(susp); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	 //	Filled->Suspended->Suspended(Crct)->PartFill
	//smpl: Rcvd_New->Pend_New->New->Filled->Suspended->Suspended(Crct)->PartFill
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(136, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 100;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onSuspended susp;
		{
			susp.orderId_ = order->orderId_;
			susp.generator_ = IdTGenerator::instance();
			susp.transaction_ = &trCntxt;
			susp.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(susp); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(151, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = ord->orderQty_ - 10;
			correctParams1->cumQty_ = 10;
			correctParams1->leavesQty_ = 10;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(10 == ord->leavesQty_);
		check(10 == ord->cumQty_);
		trCntxt.clear();

		onContinue cont;
		{
			cont.orderId_ = order->orderId_;
			cont.generator_ = IdTGenerator::instance();
			cont.transaction_ = &trCntxt;
			cont.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cont); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//	Filled->Suspended->Suspended(Crct)->New
	//smpl: Rcvd_New->Pend_New->New->Filled->Suspended->Suspended(Crct)->New
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(333, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 100;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onSuspended susp;
		{
			susp.orderId_ = order->orderId_;
			susp.generator_ = IdTGenerator::instance();
			susp.transaction_ = &trCntxt;
			susp.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(susp); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(251, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 0;
			correctParams1->cumQty_ = ord->orderQty_;
			correctParams1->leavesQty_ = ord->orderQty_;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(ord->orderQty_ == ord->leavesQty_);
		check(ord->orderQty_ == ord->cumQty_);
		trCntxt.clear();

		onContinue cont;
		{
			cont.orderId_ = order->orderId_;
			cont.generator_ = IdTGenerator::instance();
			cont.transaction_ = &trCntxt;
			cont.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cont); 
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//Filled->Suspended->Suspended(Crct)->Expired
	//smpl: Rcvd_New->Pend_New->New->Filled->Suspended->Suspended(Crct)->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
			recvevnt.order_->side_ = BUY_SIDE;
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(433, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 100;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onSuspended susp;
		{
			susp.orderId_ = order->orderId_;
			susp.generator_ = IdTGenerator::instance();
			susp.transaction_ = &trCntxt;
			susp.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(susp); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(351, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 0;
			correctParams1->cumQty_ = ord->orderQty_;
			correctParams1->leavesQty_ = ord->orderQty_;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("Suspended", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(ord->orderQty_ == ord->leavesQty_);
		check(ord->orderQty_ == ord->cumQty_);
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}

	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->Rejected
	//smpl: Rcvd_New->Pend_New->New->Rejected
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onOrderRejected reject;
		{
			reject.orderId_ = order->orderId_;
			reject.generator_ = IdTGenerator::instance();
			reject.transaction_ = &trCntxt;
			reject.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(reject); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->Expired
	//smpl: Rcvd_New->Pend_New->New->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->Expired
	//smpl: Rcvd_New->Pend_New->New->PartFill->PartFill(Crct)->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(161, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 50;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(167, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 20;
			correctParams1->cumQty_ = 20;
			correctParams1->leavesQty_ = 80;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(80 == ord->leavesQty_);
		check(20 == ord->cumQty_);
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->DoneForDay->
	//		PartFill->Suspended->PartFill->Expired
	//smpl: Rcvd_New->Pend_New->New->PartFill->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(171, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 50;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_New->New->PartFill->Filled->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(181, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 50;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams2(createTradeExec(*order.get(), IdT(182, 5)));
		onTradeExecution tradeevnt1(tradeParams2.get());
		{
			tradeevnt1.orderId_ = order->orderId_;
			tradeevnt1.generator_ = IdTGenerator::instance();
			tradeevnt1.transaction_ = &trCntxt;
			tradeevnt1.orderStorage_ = OrderStorage::instance();
			tradeParams2->lastQty_ = 50;
			tradeevnt1.orderBook_ = &books;
		}
		p.processEvent(tradeevnt1); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_New->New->PartFill->Filled->Expired->Expired(Crct)
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(281, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 50;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams2(createTradeExec(*order.get(), IdT(282, 5)));
		onTradeExecution tradeevnt1(tradeParams2.get());
		{
			tradeevnt1.orderId_ = order->orderId_;
			tradeevnt1.generator_ = IdTGenerator::instance();
			tradeevnt1.transaction_ = &trCntxt;
			tradeevnt1.orderStorage_ = OrderStorage::instance();
			tradeParams2->lastQty_ = 50;
			tradeevnt1.orderBook_ = &books;
		}
		p.processEvent(tradeevnt1); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(283, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 20;
			correctParams1->cumQty_ = ord->cumQty_;
			correctParams1->leavesQty_ = 10;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("Expired", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(10 == ord->leavesQty_);
		trCntxt.clear();

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill(Crct)->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(381, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 50;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(383, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 20;
			correctParams1->cumQty_ = ord->cumQty_;
			correctParams1->leavesQty_ = 80;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("PartFill", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(80 == ord->leavesQty_);
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

	}
	{//Rcvd_New->Pend_New->New->Filled
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(192, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = ord->leavesQty_;
			tradeevnt.orderBook_ = &books;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("Filled", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_New->New->Rejected
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onOrderRejected reject;
		{
			reject.orderId_ = order->orderId_;
			reject.generator_ = IdTGenerator::instance();
			reject.transaction_ = &trCntxt;
			reject.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(reject); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_New->New->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->New->Rejected
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onOrderRejected reject;
		{
			reject.orderId_ = order->orderId_;
			reject.generator_ = IdTGenerator::instance();
			reject.transaction_ = &trCntxt;
			reject.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(reject); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->New->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start();	
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Rejected
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onRecvOrderRejected reject;
		{
			reject.order_ = order.get();
			reject.generator_ = IdTGenerator::instance();
			reject.transaction_ = &trCntxt;
			reject.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(reject); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Rejected(Extern)
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onExternalOrder externOrd;
		{
			externOrd.order_ = order.get();
			externOrd.generator_ = IdTGenerator::instance();
			externOrd.transaction_ = &trCntxt;
			externOrd.orderStorage_ = OrderStorage::instance();
			externOrd.order_->side_ = INVALID_SIDE;
		}
		p.processEvent(externOrd); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->New(Extern)->Expired
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onExternalOrder externOrd;
		{
			externOrd.order_ = order.get();
			externOrd.generator_ = IdTGenerator::instance();
			externOrd.transaction_ = &trCntxt;
			externOrd.orderStorage_ = OrderStorage::instance();
			externOrd.order_->side_ = BUY_SIDE;
		}
		p.processEvent(externOrd); 
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);

		onExpired expire;
		{
			expire.orderId_ = order->orderId_;
			expire.generator_ = IdTGenerator::instance();
			expire.transaction_ = &trCntxt;
			expire.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(expire); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Rejected(PendRepl)
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onRecvRplOrderRejected reject;
		{
			reject.order_ = order.get();
			reject.generator_ = IdTGenerator::instance();
			reject.transaction_ = &trCntxt;
			reject.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(reject); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_Replace->Rejected
		std::unique_ptr<OrderEntry> orderRepl(createReplOrder());
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(orderRepl.get());
		OrderEntry *origOrd = OrderStorage::instance()->locateByClOrderId(orderRepl->origClOrderId_.get());
		check(nullptr != origOrd);
		orderRepl->origOrderId_ = origOrd->orderId_;
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onRplOrderReceived repl;
		{
			repl.order_ = orderRepl.get();
			repl.generator_ = IdTGenerator::instance();
			repl.transaction_ = &trCntxt;
			repl.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(repl); 
		p.checkStates("Pend_Replace", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(orderRepl->clOrderId_.get());
		check(nullptr != ord);

		onRplOrderRejected reject;
		{
			reject.orderId_ = ord->orderId_;
			reject.generator_ = IdTGenerator::instance();
			reject.transaction_ = &trCntxt;
			reject.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(reject); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_Replace->New
		std::unique_ptr<OrderEntry> orderRepl(createReplOrder());
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(orderRepl.get());
		OrderEntry *origOrd = OrderStorage::instance()->locateByClOrderId(orderRepl->origClOrderId_.get());
		check(nullptr != origOrd);
		orderRepl->origOrderId_ = origOrd->orderId_;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onRplOrderReceived repl;
		{
			repl.order_ = orderRepl.get();
			repl.generator_ = IdTGenerator::instance();
			repl.transaction_ = &trCntxt;
			repl.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(repl); 
		p.checkStates("Pend_Replace", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(orderRepl->clOrderId_.get());
		check(nullptr != ord);

		onReplace acceptEvnt;
		{
			acceptEvnt.origOrderId_ = orderRepl->origOrderId_;
			acceptEvnt.orderId_ = ord->orderId_;
			acceptEvnt.generator_ = IdTGenerator::instance();
			acceptEvnt.transaction_ = &trCntxt;
			ord->side_ = BUY_SIDE;
			acceptEvnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(acceptEvnt); 
		p.checkStates("New", "NoCnlReplace");
		check(4 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_REPLACE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_Replace->Rejected(replReject)
		std::unique_ptr<OrderEntry> orderRepl(createReplOrder());
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(orderRepl.get());
		OrderEntry *origOrd = OrderStorage::instance()->locateByClOrderId(orderRepl->origClOrderId_.get());
		check(nullptr != origOrd);
		orderRepl->origOrderId_ = origOrd->orderId_;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onRplOrderReceived repl;
		{
			repl.order_ = orderRepl.get();
			repl.generator_ = IdTGenerator::instance();
			repl.transaction_ = &trCntxt;
			repl.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(repl); 
		p.checkStates("Pend_Replace", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(orderRepl->clOrderId_.get());
		check(nullptr != ord);

		onReplace rejctEvnt;
		{
			rejctEvnt.origOrderId_ = order->orderId_;
			rejctEvnt.orderId_ = orderRepl->orderId_;
			rejctEvnt.generator_ = IdTGenerator::instance();
			rejctEvnt.transaction_ = &trCntxt;
			ord->side_ = INVALID_SIDE;
			rejctEvnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(rejctEvnt); 
		p.checkStates("Rejected", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
	}
	{//Rcvd_New->Pend_Replace->Expired
		std::unique_ptr<OrderEntry> orderRepl(createReplOrder());
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(orderRepl.get());
		OrderEntry *origOrd = OrderStorage::instance()->locateByClOrderId(orderRepl->origClOrderId_.get());
		check(nullptr != origOrd);
		orderRepl->origOrderId_ = origOrd->orderId_;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onRplOrderReceived repl;
		{
			repl.order_ = orderRepl.get();
			repl.generator_ = IdTGenerator::instance();
			repl.transaction_ = &trCntxt;
			repl.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(repl); 
		p.checkStates("Pend_Replace", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(orderRepl->clOrderId_.get());
		check(nullptr != ord);

		onRplOrderExpired exprEvnt;
		{
			exprEvnt.orderId_ = orderRepl->orderId_;
			exprEvnt.generator_ = IdTGenerator::instance();
			exprEvnt.transaction_ = &trCntxt;
			ord->side_ = BUY_SIDE;
			exprEvnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(exprEvnt); 
		p.checkStates("Expired", "NoCnlReplace");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
	}
	{//NoCnlReplace->GoingCancel->NoCnlReplace
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onCancelReceived cncl;
		{
			cncl.orderId_ = order->orderId_;
			cncl.generator_ = IdTGenerator::instance();
			cncl.transaction_ = &trCntxt;
			cncl.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cncl); 
		p.checkStates("New", "GoingCancel");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onCancelRejected cnclRjct;
		{
			cnclRjct.orderId_ = order->orderId_;
			cnclRjct.generator_ = IdTGenerator::instance();
			cnclRjct.transaction_ = &trCntxt;
			cnclRjct.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cnclRjct); 
		p.checkStates("New", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CANCEL_REJECT_TROPERATION));
		trCntxt.clear();
	}
	{//NoCnlReplace->GoingCancel->CnclReplaced->CnclReplaced(Crct)
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		trCntxt.clear();

		onCancelReceived cncl;
		{
			cncl.orderId_ = order->orderId_;
			cncl.generator_ = IdTGenerator::instance();
			cncl.transaction_ = &trCntxt;
			cncl.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cncl); 
		p.checkStates("New", "GoingCancel");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(581, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 50;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "GoingCancel");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onExecCancel cnclRjct;
		{
			cnclRjct.orderId_ = order->orderId_;
			cnclRjct.generator_ = IdTGenerator::instance();
			cnclRjct.transaction_ = &trCntxt;
			cnclRjct.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cnclRjct); 
		p.checkStates("PartFill", "CnclReplaced");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(583, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 20;
			correctParams1->cumQty_ = ord->cumQty_;
			correctParams1->leavesQty_ = 80;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("PartFill", "CnclReplaced");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(80 == ord->leavesQty_);
		trCntxt.clear();
	}
	{//NoCnlReplace->GoingReplace->NoCnlReplace
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());

		onReplaceReceived cncl(IdT(1, 5));
		{
			cncl.orderId_ = order->orderId_;
			cncl.generator_ = IdTGenerator::instance();
			cncl.transaction_ = &trCntxt;
			cncl.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cncl); 
		p.checkStates("New", "GoingReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onReplaceRejected cnclRjct(IdT(2, 5));
		{
			cnclRjct.orderId_ = order->orderId_;
			cnclRjct.generator_ = IdTGenerator::instance();
			cnclRjct.transaction_ = &trCntxt;
			cnclRjct.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cnclRjct); 
		p.checkStates("New", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CANCEL_REJECT_TROPERATION));
		trCntxt.clear();
	}
	{//NoCnlReplace->GoingReplace->CnclReplaced->CnclReplaced(Crct)
		TestTransactionContext trCntxt;
		OrderStateWrapper p;

		assignClOrderId(order.get());
		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(ord->orderId_.isValid());

		onReplaceReceived cncl(IdT(3, 5));
		{
			cncl.orderId_ = order->orderId_;
			cncl.generator_ = IdTGenerator::instance();
			cncl.transaction_ = &trCntxt;
			cncl.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cncl); 
		p.checkStates("New", "GoingReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<TradeExecEntry> tradeParams1(createTradeExec(*order.get(), IdT(681, 5)));
		onTradeExecution tradeevnt(tradeParams1.get());
		{
			tradeevnt.orderId_ = order->orderId_;
			ord->leavesQty_ = 100;
			ord->orderQty_ = 100;
			tradeevnt.generator_ = IdTGenerator::instance();
			tradeevnt.transaction_ = &trCntxt;
			tradeevnt.orderStorage_ = OrderStorage::instance();
			tradeParams1->lastQty_ = 50;
		}
		p.processEvent(tradeevnt); 
		p.checkStates("PartFill", "GoingReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_TRADE_EXECREPORT_TROPERATION));
		trCntxt.clear();

		onExecReplace cnclRjct(IdT(5, 5));
		{
			cnclRjct.orderId_ = order->orderId_;
			cnclRjct.generator_ = IdTGenerator::instance();
			cnclRjct.transaction_ = &trCntxt;
			cnclRjct.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(cnclRjct); 
		p.checkStates("PartFill", "CnclReplaced");
		check(2 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REPLACE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(REMOVE_ORDERBOOK_TROPERATION));
		trCntxt.clear();

		std::unique_ptr<ExecCorrectExecEntry> correctParams1(createCorrectExec(*order.get(), IdT(683, 5)));
		onTradeCrctCncl tradeCrctevnt(correctParams1.get());
		{
			tradeCrctevnt.orderId_ = order->orderId_;
			tradeCrctevnt.generator_ = IdTGenerator::instance();
			tradeCrctevnt.transaction_ = &trCntxt;
			tradeCrctevnt.orderStorage_ = OrderStorage::instance();
			correctParams1->lastQty_ = 20;
			correctParams1->cumQty_ = ord->cumQty_;
			correctParams1->leavesQty_ = 80;
		}
		p.processEvent(tradeCrctevnt); 
		p.checkStates("PartFill", "CnclReplaced");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_CORRECT_EXECREPORT_TROPERATION));
		check(80 == ord->leavesQty_);
		trCntxt.clear();

	}

	OrderStorage::destroy();
	SubscriptionMgr::destroy();
	WideDataStorage::destroy();
	IdTGenerator::destroy();

	cout<< "Finished test testStateMachine."<< endl;
	return true;
}

bool testStateMachineOnly()
{
	cout<< "Start test testStateMachineOnly..."<< endl;

	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->Filled->New(Crct)->Rejected
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderRejected>(nullptr)); 
		p.checkStates("Rejected", "NoCnlReplace");
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->Filled->New(Crct)->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->
	 //		New(Crct)->Filled->New(Crct)->DoneForDay->New->Suspended->New->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onNewDay>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onContinue>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//		Filled->New(Crct)->DoneForDay->New->Suspended->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onNewDay>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");
	}
// is Suspended->DoneForDay->Suspended->DoneForDay correct?
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//		Filled->New(Crct)->DoneForDay->New->Suspended->DoneForDay->Suspended->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onNewDay>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//		Filled->New(Crct)->DoneForDay->New->Rejected
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onNewDay>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderRejected>(nullptr));
		p.checkStates("Rejected", "NoCnlReplace");
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//	Filled->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");
		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");
	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//	Filled->DoneForDay
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
		//	Filled->DoneForDay->DoneForDay(Crct)->New

		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onNewDay>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
		//		Filled->DoneForDay->DoneForDay(Crct)->PartFill
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onNewDay>(nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

	}

//is Suspended->Filled required?
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
		//	Filled->Suspended
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	 //	Filled->Suspended->Suspended(Crct)->PartFill
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onContinue>(nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");


	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//	Filled->Suspended->Suspended(Crct)->New
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onContinue>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->
	//Filled->Suspended->Suspended(Crct)->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}

	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->Rejected
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderRejected>(nullptr)); 
		p.checkStates("Rejected", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->New(Crct)->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill->Filled->PartFill(Crct)->PartFill(Crct)->DoneForDay->
	//		PartFill->Suspended->PartFill->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onFinished>(nullptr)); 
		p.checkStates("DoneForDay", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onNewDay>(nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onSuspended>(nullptr)); 
		p.checkStates("Suspended", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onContinue>(nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->Filled->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->Filled->Expired->Expired(Crct)
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->PartFill->PartFill(Crct)->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->Filled
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr)); 
		p.checkStates("Filled", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->Rejected
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderRejected>(nullptr)); 
		p.checkStates("Rejected", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_New->New->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->New->Rejected
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderRejected>(nullptr)); 
		p.checkStates("Rejected", "NoCnlReplace");

	}
	{//Rcvd_New->New->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->Rejected
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRecvOrderRejected>(nullptr)); 
		p.checkStates("Rejected", "NoCnlReplace");

	}
	{//Rcvd_New->Rejected(Extern)
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExternalOrderRejected>(nullptr, false)); 
		p.checkStates("Rejected", "NoCnlReplace");

	}
	{//Rcvd_New->New(Extern)->Expired

		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExternalOrder>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}
	{//Rcvd_New->Rejected(PendRepl)
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRecvRplOrderRejected>(nullptr)); 
		p.checkStates("Rejected", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_Replace->Rejected
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRplOrderReceived>(nullptr)); 
		p.checkStates("Pend_Replace", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRplOrderRejected>(nullptr)); 
		p.checkStates("Rejected", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_Replace->New
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRplOrderReceived>(nullptr)); 
		p.checkStates("Pend_Replace", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onReplace>(nullptr)); 
		p.checkStates("New", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_Replace->Rejected(replReject)
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRplOrderReceived>(nullptr)); 
		p.checkStates("Pend_Replace", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRplOrderRejected>(nullptr)); 
		p.checkStates("Rejected", "NoCnlReplace");

	}
	{//Rcvd_New->Pend_Replace->Expired
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRplOrderReceived>(nullptr)); 
		p.checkStates("Pend_Replace", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onRplOrderExpired>(nullptr)); 
		p.checkStates("Expired", "NoCnlReplace");

	}

	{//NoCnlReplace->GoingCancel->NoCnlReplace
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onCancelReceived>(nullptr)); 
		p.checkStates("Rcvd_New", "GoingCancel");

		p.processEvent(eventForTestStateMachineOnly<onCancelRejected>(nullptr)); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

	}
	{//NoCnlReplace->GoingCancel->CnclReplaced->CnclReplaced(Crct)
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onCancelReceived>(nullptr)); 
		p.checkStates("Rcvd_New", "GoingCancel");

		p.processEvent(eventForTestStateMachineOnly<onExecCancel>(nullptr)); 
		p.checkStates("Rcvd_New", "CnclReplaced");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("Rcvd_New", "CnclReplaced");
	}
	{//NoCnlReplace->GoingReplace->NoCnlReplace
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onReplaceReceived>(nullptr, IdT())); 
		p.checkStates("Rcvd_New", "GoingReplace");

		p.processEvent(eventForTestStateMachineOnly<onReplaceRejected>(nullptr, IdT())); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

	}

	{//NoCnlReplace->GoingReplace->NoCnlReplace
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onReplaceReceived>(nullptr, IdT())); 
		p.checkStates("Rcvd_New", "GoingReplace");

		p.processEvent(eventForTestStateMachineOnly<onOrderReceived>(nullptr));
		p.checkStates("New", "GoingReplace");

		p.processEvent(eventForTestStateMachineOnly<onTradeExecution>(nullptr, (TradeExecEntry *)nullptr, false)); 
		p.checkStates("PartFill", "GoingReplace");

		p.processEvent(eventForTestStateMachineOnly<onReplaceRejected>(nullptr, IdT())); 
		p.checkStates("PartFill", "NoCnlReplace");

	}

	{//NoCnlReplace->GoingReplace->CnclReplaced->CnclReplaced(Crct)
		OrderStateWrapper p;

		p.start(); 
		p.checkStates("Rcvd_New", "NoCnlReplace");

		p.processEvent(eventForTestStateMachineOnly<onReplaceReceived>(nullptr, IdT())); 
		p.checkStates("Rcvd_New", "GoingReplace");

		p.processEvent(eventForTestStateMachineOnly<onExecReplace>(nullptr, IdT())); 
		p.checkStates("Rcvd_New", "CnclReplaced");

		p.processEvent(eventForTestStateMachineOnly<onTradeCrctCncl>(nullptr, (ExecCorrectExecEntry *)nullptr)); 
		p.checkStates("Rcvd_New", "CnclReplaced");
	}

	cout<< "Finished test testStateMachineOnly."<< endl;
	return true;
}
