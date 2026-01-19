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
	T eventForTestStateMachineOnly(T *e, bool checkRez = true)
	{
		T evnt;	
		evnt.testStateMachine_ = true;
		evnt.testStateMachineCheckResult_ = checkRez;
		return evnt;
	}
	template<typename T, typename P>
	T eventForTestStateMachineOnly(T *e, const P &param, bool checkRez = true)
	{
		T evnt(param);	
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

		return std::unique_ptr<OrderEntry>(ptr);
	}

	void assignClOrderId(OrderEntry *order, const string &val){
		std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
		order->clOrderId_ = WideDataStorage::instance()->add(clOrd.release());
	}

	void assignOrigClOrderId(OrderEntry *order, const string &val){
		std::unique_ptr<RawDataEntry> clOrd(new RawDataEntry(STRING_RAWDATATYPE, val.c_str(), static_cast<u32>(val.size())));
		order->origClOrderId_ = WideDataStorage::instance()->add(clOrd.release());
	}

	void assignClOrderId(OrderEntry *order){
		static int id = 0;

		string val("TestClOrderId_");
		char buf[64];
		val += _itoa(++id, buf, 10);
		assignClOrderId(order, val);
	}
}

bool testRcvdNew2New_onOrderReceived();
bool testRcvdNew2Rejected_onRecvOrderRejected();
bool testRcvdNew2PendReplace_onRplOrderReceived();
bool testRcvdNew2Rejected_onRecvRplOrderRejected();
bool testRcvdNew2Rejected_onExternalOrderReject();
bool testRcvdNew2New_onExternalOrder();

bool testPendNew2New_onOrderAccepted();
bool testPendNew2Rejected_onOrderRejected();
bool testPendNew2Expired_onExpired();
bool testPendRepl2New_onReplace();
bool testPendRepl2Rejected_onRplOrderRejected();
bool testPendRepl2Expired_onRplOrderExpired();
bool testNew2PartFill_onTradeExecution();
bool testNew2Filled_onTradeExecution();
bool testNew2Rejected_onOrderRejected();
bool testNew2Expired_onExpired();
bool testNew2DoneForDay_onFinished();
bool testNew2Suspended_onSuspended();
bool testPartFill2PartFill_onTradeExecution();
bool testPartFill2Filled_onTradeExecution();
bool testPartFill2PartFill_onTradeCrctCncl();
bool testPartFill2New_onTradeCrctCncl();
bool testPartFill2Expired_onExpired();
bool testPartFill2DoneForDay_onFinished();
bool testPartFill2Suspended_onSuspended();
bool testFilled2PartFill_onTradeCrctCncl();
bool testFilled2New_onTradeCrctCncl();
bool testFilled2Expired_onExpired();
bool testFilled2DoneForDay_onFinished();
bool testFilled2Suspended_onSuspended();
bool testExpired2Expired_onTradeCrctCncl();
bool testDoneForDay2PartFill_onNewDay();
bool testDoneForDay2New_onNewDay();
bool testDoneForDay2DoneForDay_onTradeCrctCncl();
bool testDoneForDay2Suspended_onSuspended();
bool testSuspended2PartFill_onContinue();
bool testSuspended2New_onContinue();
bool testSuspended2Expired_onExpired();
bool testSuspended2DoneForDay_onFinished();
bool testSuspended2Suspended_onTradeCrctCncl();
bool testNoCnlReplace2GoingCancel_onCancelReceived();
bool testNoCnlReplace2GoingReplace_onReplaceReceived();
bool testGoingCancel2NoCnlReplace_onCancelRejected();
bool testGoingCancel2CnclReplaced_onExecCancel();
bool testGoingReplace2NoCnlReplace_onReplaceRejected();
bool testGoingReplace2CnclReplaced_onExecReplace();
bool testCnclReplaced2CnclReplaced_onTradeCrctCncl();


bool testStates()
{
	cout<< "Start test testStates..."<< endl;

	WideDataStorage::create();
	SubscriptionMgr::create();
	IdTGenerator::create();
	OrderStorage::create();

	testRcvdNew2New_onOrderReceived();
	testRcvdNew2Rejected_onRecvOrderRejected();
	testRcvdNew2PendReplace_onRplOrderReceived();
	testRcvdNew2Rejected_onRecvRplOrderRejected();
	testRcvdNew2Rejected_onExternalOrderReject();
	testRcvdNew2New_onExternalOrder();

	//todo: complete tests
	//testPendNew2New_onOrderAccepted();
	//testPendNew2Rejected_onOrderRejected();
	//testPendNew2Expired_onExpired();
	testPendRepl2New_onReplace();
	testPendRepl2Rejected_onRplOrderRejected();
	testPendRepl2Expired_onRplOrderExpired();
	testNew2PartFill_onTradeExecution();
	testNew2Filled_onTradeExecution();
	testNew2Rejected_onOrderRejected();
	testNew2Expired_onExpired();
	testNew2DoneForDay_onFinished();
	testNew2Suspended_onSuspended();
	testPartFill2PartFill_onTradeExecution();
	testPartFill2Filled_onTradeExecution();
	testPartFill2PartFill_onTradeCrctCncl();
	testPartFill2New_onTradeCrctCncl();
	testPartFill2Expired_onExpired();
	testPartFill2DoneForDay_onFinished();
	testPartFill2Suspended_onSuspended();
	testFilled2PartFill_onTradeCrctCncl();
	testFilled2New_onTradeCrctCncl();
	testFilled2Expired_onExpired();
	testFilled2DoneForDay_onFinished();
	testFilled2Suspended_onSuspended();
	testExpired2Expired_onTradeCrctCncl();
	testDoneForDay2PartFill_onNewDay();
	testDoneForDay2New_onNewDay();
	testDoneForDay2DoneForDay_onTradeCrctCncl();
	testDoneForDay2Suspended_onSuspended();
	testSuspended2PartFill_onContinue();
	testSuspended2New_onContinue();
	testSuspended2Expired_onExpired();
	testSuspended2DoneForDay_onFinished();
	testSuspended2Suspended_onTradeCrctCncl();
	testNoCnlReplace2GoingCancel_onCancelReceived();
	testNoCnlReplace2GoingReplace_onReplaceReceived();
	testGoingCancel2NoCnlReplace_onCancelRejected();
	testGoingCancel2CnclReplaced_onExecCancel();
	testGoingReplace2NoCnlReplace_onReplaceRejected();
	testGoingReplace2CnclReplaced_onExecReplace();
	testCnclReplaced2CnclReplaced_onTradeCrctCncl();

	OrderStorage::destroy();
	SubscriptionMgr::destroy();
	WideDataStorage::destroy();
	IdTGenerator::destroy();

	cout<< "Finished test testStates."<< endl;
	return true;
}

bool testRcvdNew2New_onOrderReceived()
{
	{// check how new order processed
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(0 == order->orderId_.id_);
		check(0 == order->orderId_.date_);
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("New", "NoCnlReplace");
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		trCntxt.clear();
		check(NEW_ORDSTATUS == ord->status_);
	}

	{// check how new order with existing ClOrdId processed
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(0 == order->orderId_.id_);
		check(0 == order->orderId_.date_);
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);

		onOrderReceived recvevnt;
		{
			recvevnt.order_ = order.get();
			recvevnt.generator_ = IdTGenerator::instance();
			recvevnt.transaction_ = &trCntxt;
			recvevnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(recvevnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!recvevnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByOrderId(order->orderId_);
		check(nullptr == ord);
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(NEW_ORDSTATUS == ord->status_);
		check(REJECTED_ORDSTATUS == order->status_);
	}

	{// check how new incorrect order (invalid side) processed
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(0 == order->orderId_.id_);
		check(0 == order->orderId_.date_);

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
		check(!recvevnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == ord->status_);
	}

	{// check how new incorrect order (empty ClOrderId) processed
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get(), "");

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(0 == order->orderId_.id_);
		check(0 == order->orderId_.date_);
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

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
		check(!recvevnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);
		ord = OrderStorage::instance()->locateByOrderId(order->orderId_);
		check(nullptr == ord);
		check(REJECTED_ORDSTATUS == order->status_);
	}

	return true;

}


bool testRcvdNew2Rejected_onRecvOrderRejected()
{
	{// check how new order processed
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(0 == order->orderId_.id_);
		check(0 == order->orderId_.date_);
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onRecvOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == ord->status_);
	}

	{// check how new order (with empty ClOrderId) processed
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get(), "");
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(0 == order->orderId_.id_);
		check(0 == order->orderId_.date_);

		onRecvOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);
		ord = OrderStorage::instance()->locateByOrderId(order->orderId_);
		check(nullptr == ord);
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
	}

	{// check how new order (with duplicate ClOrderId) processed
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get(), "TestClOrderId_1");

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(0 == order->orderId_.id_);
		check(0 == order->orderId_.date_);
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);

		onRecvOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
		check(REJECTED_ORDSTATUS == ord->status_);
	}
	{// check how new order (with invalid values) processed
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(0 == order->orderId_.id_);
		check(0 == order->orderId_.date_);
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onRecvOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
			evnt.order_->side_ = INVALID_SIDE;
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == ord->status_);
	}

	return true;
}
bool testRcvdNew2PendReplace_onRplOrderReceived()
{
	{
		/*correct order should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());
		//assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onRplOrderReceived evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Pend_Replace", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(PENDINGREPLACE_ORDSTATUS == ord->status_);
	}
	{
		/*incorrect order should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onRplOrderReceived evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
			evnt.order_->side_ = INVALID_SIDE;
		}
		p.processEvent(evnt);
		p.checkStates("Pend_Replace", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(ENQUEUE_EVENT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(PENDINGREPLACE_ORDSTATUS == ord->status_);
	}
	{
		/* order with duplicate ClOrderId should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);

		onRplOrderReceived evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS != ord->status_);
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
	}
	{
		/* order with empty OrigClOrderId - should be rejected*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onRplOrderReceived evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
			evnt.order_->origClOrderId_ = IdT();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		check(REJECTED_ORDSTATUS == ord->status_);
	}

	{
		/* order with unknown OrigClOrderId - should be rejected*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());
		assignClOrderId(order.get());
		assignOrigClOrderId(order.get(), "Unknown");

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onRplOrderReceived evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(ord->orderId_.isValid());
		check(REJECTED_ORDSTATUS == ord->status_);
	}

	return true;
}
bool testRcvdNew2Rejected_onRecvRplOrderRejected()
{
	{
		/* correct order should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onRecvRplOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == ord->status_);
	}
	{
		/* incorrect order should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onRecvRplOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
			evnt.order_->side_ = INVALID_SIDE;
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == ord->status_);
	}

	{
		/* order with empty ClOrderId should be rejected*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());
		assignClOrderId(order.get(), "");

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = nullptr;

		onRecvRplOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByOrderId(order->orderId_);
		check(nullptr == ord);
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
	}
	{
		/* order with duplicate ClOrderId should be rejected*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createReplOrder());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);

		onRecvRplOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
		check(REJECTED_ORDSTATUS != ord->status_);
	}

	return true;
}
bool testRcvdNew2Rejected_onExternalOrderReject()
{
	{
		/* correct order should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onExternalOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == ord->status_);
	}

	{
		/* incorrect order should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onExternalOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
			evnt.order_->side_ = INVALID_SIDE;
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == ord->status_);
	}

	{
		/* order with empty ClORderId should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get(), "");

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onExternalOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);
		ord = OrderStorage::instance()->locateByOrderId(order->orderId_);
		check(nullptr == ord);
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
	}

	{
		/* order with duplicate ClORderId should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);

		onExternalOrderRejected evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(!evnt.order_->orderId_.isValid());
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByOrderId(order->orderId_);
		check(nullptr == ord);
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS != ord->status_);
	}

	return true;
}
bool testRcvdNew2New_onExternalOrder()
{
	{
		/* correct order should be processed*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onExternalOrder evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("New", "NoCnlReplace");
		check(3 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_EXECREPORT_TROPERATION));
		check(trCntxt.isOperationEnqueued(ADD_ORDERBOOK_TROPERATION));
		check(trCntxt.isOperationEnqueued(MATCH_ORDER_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(NEW_ORDSTATUS == ord->status_);
	}
	{
		/* incorrect order should be rejected*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onExternalOrder evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
			evnt.order_->side_ = INVALID_SIDE;
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		ord = OrderStorage::instance()->locateByOrderId(ord->orderId_);
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == ord->status_);
	}
	{
		/* order with empty ClOrderID should be rejected*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());
		assignClOrderId(order.get(), "");

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);

		onExternalOrder evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr == ord);
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
	}
	{
		/* order with duplicate ClOrderID should be rejected*/
		TestTransactionContext trCntxt;
		OrderStateWrapper p;
		std::unique_ptr<OrderEntry> order(createCorrectOrder());

		p.start();
		p.checkStates("Rcvd_New", "NoCnlReplace");
		check(!order->orderId_.isValid());
		OrderEntry *ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);

		onExternalOrder evnt;
		{
			evnt.order_ = order.get();
			evnt.generator_ = IdTGenerator::instance();
			evnt.transaction_ = &trCntxt;
			evnt.orderStorage_ = OrderStorage::instance();
		}
		p.processEvent(evnt);
		p.checkStates("Rejected", "NoCnlReplace");
		check(1 == trCntxt.op_.size());
		check(trCntxt.isOperationEnqueued(CREATE_REJECT_EXECREPORT_TROPERATION));
		trCntxt.clear();
		ord = OrderStorage::instance()->locateByClOrderId(order->clOrderId_.get());
		check(nullptr != ord);
		check(REJECTED_ORDSTATUS == evnt.order_->status_);
		check(REJECTED_ORDSTATUS != ord->status_);
	}


	return true;
}

bool testPendNew2New_onOrderAccepted()
{
	return true;
}
bool testPendNew2Rejected_onOrderRejected()
{
	return true;
}

bool testPendNew2Expired_onExpired()
{
	return true;
}

bool testPendRepl2New_onReplace()
{
	return true;
}

bool testPendRepl2Rejected_onRplOrderRejected()
{
	return true;
}

bool testPendRepl2Expired_onRplOrderExpired()
{
	return true;
}

bool testNew2PartFill_onTradeExecution()
{
	return true;
}

bool testNew2Filled_onTradeExecution()
{
	return true;
}

bool testNew2Rejected_onOrderRejected()
{
	return true;
}

bool testNew2Expired_onExpired()
{
	return true;
}

bool testNew2DoneForDay_onFinished()
{
	return true;
}

bool testNew2Suspended_onSuspended()
{
	return true;
}

bool testPartFill2PartFill_onTradeExecution()
{
	return true;
}

bool testPartFill2Filled_onTradeExecution()
{
	return true;
}

bool testPartFill2PartFill_onTradeCrctCncl()
{
	return true;
}

bool testPartFill2New_onTradeCrctCncl()
{
	return true;
}

bool testPartFill2Expired_onExpired()
{
	return true;
}

bool testPartFill2DoneForDay_onFinished()
{
	return true;
}

bool testPartFill2Suspended_onSuspended()
{
	return true;
}

bool testFilled2PartFill_onTradeCrctCncl()
{
	return true;
}

bool testFilled2New_onTradeCrctCncl()
{
	return true;
}

bool testFilled2Expired_onExpired()
{
	return true;
}

bool testFilled2DoneForDay_onFinished()
{
	return true;
}

bool testFilled2Suspended_onSuspended()
{
	return true;
}

bool testExpired2Expired_onTradeCrctCncl()
{
	return true;
}

bool testDoneForDay2PartFill_onNewDay()
{
	return true;
}

bool testDoneForDay2New_onNewDay()
{
	return true;
}

bool testDoneForDay2DoneForDay_onTradeCrctCncl()
{
	return true;
}

bool testDoneForDay2Suspended_onSuspended()
{
	return true;
}

bool testSuspended2PartFill_onContinue()
{
	return true;
}

bool testSuspended2New_onContinue()
{
	return true;
}

bool testSuspended2Expired_onExpired()
{
	return true;
}

bool testSuspended2DoneForDay_onFinished()
{
	return true;
}

bool testSuspended2Suspended_onTradeCrctCncl()
{
	return true;
}

bool testNoCnlReplace2GoingCancel_onCancelReceived()
{
	return true;
}

bool testNoCnlReplace2GoingReplace_onReplaceReceived()
{
	return true;
}

bool testGoingCancel2NoCnlReplace_onCancelRejected()
{
	return true;
}

bool testGoingCancel2CnclReplaced_onExecCancel()
{
	return true;
}

bool testGoingReplace2NoCnlReplace_onReplaceRejected()
{
	return true;
}

bool testGoingReplace2CnclReplaced_onExecReplace()
{
	return true;
}

bool testCnclReplaced2CnclReplaced_onTradeCrctCncl()
{
	return true;
}
