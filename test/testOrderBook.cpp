/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TestAux.h"
#include "DataModelDef.h"
#include "OrderBookImpl.h"
#include "IdTGenerator.h"
#include "WideDataStorage.h"
#include <iostream>

using namespace std;
using namespace COP;
using namespace COP::Store;
using namespace test;

namespace {

	SourceIdT addInstrument(const std::string &name){
		std::unique_ptr<InstrumentEntry> instr(new InstrumentEntry());
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
		virtual bool match(const IdT &, bool *)const{return res_;}
		void setSide(Side side){side_ = side;}
	public:
		SourceIdT instr_;
		bool res_;
	};

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
		ptr->orderQty_ = 77;

		return std::unique_ptr<OrderEntry>(ptr);
	}

}

bool testOrderBook()
{
	{
		DummyOrderSaver saver;
		OrderBookImpl books;
		OrderBookImpl::InstrumentsT instr;
		books.init(instr, &saver);
	}
	{
		DummyOrderSaver saver;
		WideDataStorage::create();
		OrderBookImpl books;
		OrderBookImpl::InstrumentsT instr;
		SourceIdT instrId = addInstrument("aaa");
		instr.insert(instrId);
		instr.insert(addInstrument("bbb"));
		books.init(instr, &saver);
		TestOrderFunctor funct(SourceIdT(), BUY_SIDE, true);
		try{
			books.find(funct);
			check(false);
		}catch(const std::exception &){
		}
		try{
			OrderBookImpl::OrdersT ords;
			books.findAll(funct, &ords);
			check(false);
		}catch(const std::exception &){
		}
		try{
			funct.instr_ = instrId;
			funct.setSide(INVALID_SIDE);
			books.find(funct);
			check(false);
		}catch(const std::exception &){
		}
		try{
			OrderBookImpl::OrdersT ords;
			funct.instr_ = instrId;
			funct.setSide(INVALID_SIDE);
			books.findAll(funct, &ords);
			check(false);
		}catch(const std::exception &){
		}
		funct.setSide(BUY_SIDE);
		check(!books.find(funct).isValid());
		{
			OrderBookImpl::OrdersT ords;
			books.findAll(funct, &ords);
			check(ords.empty());
		}

		try{
			books.getTop(SourceIdT(), BUY_SIDE);
			check(false);
		}catch(const std::exception &){
		}
		try{
			books.getTop(instrId, INVALID_SIDE);
			check(false);
		}catch(const std::exception &){
		}
		check(!books.getTop(instrId, BUY_SIDE).isValid());

		WideDataStorage::destroy();
	}
	{
		WideDataStorage::create();
		DummyOrderSaver saver;
		OrderBookImpl books;
		OrderBookImpl::InstrumentsT instr;
		SourceIdT instrId1 = addInstrument("aaa");
		instr.insert(instrId1);
		SourceIdT instrId2 = addInstrument("bbb");
		instr.insert(instrId2);
		books.init(instr, &saver);

		{
			SourceIdT instrId3 = addInstrument("ccc");
			std::unique_ptr<OrderEntry> ord(createCorrectOrder(instrId3));
			ord->orderId_ = IdT(2, 1);
			try{
				books.add(*(ord.get()));
			}catch(const std::exception &){}
		}
		std::unique_ptr<OrderEntry> ord(createCorrectOrder(instrId1));
		ord->orderId_ = IdT(1, 1);
		ord->price_ = 5.0;
		ord->side_ = BUY_SIDE;
		books.add(*(ord.get()));
		ord->orderId_ = IdT(1, 2);
		ord->price_ = 5.0001;
		books.add(*(ord.get()));
		ord->orderId_ = IdT(1, 3);
		ord->price_ = 5.00011;
		books.add(*(ord.get()));

		TestOrderFunctor funct(SourceIdT(), BUY_SIDE, true);
		try{
			books.find(funct);
			check(false);
		}catch(const std::exception &){
		}
		try{
			OrderBookImpl::OrdersT ords;
			books.findAll(funct, &ords);
			check(false);
		}catch(const std::exception &){
		}
		try{
			funct.instr_ = instrId1;
			funct.setSide(INVALID_SIDE);
			books.find(funct);
			check(false);
		}catch(const std::exception &){
		}
		try{
			OrderBookImpl::OrdersT ords;
			funct.instr_ = instrId1;
			funct.setSide(INVALID_SIDE);
			books.findAll(funct, &ords);
			check(false);
		}catch(const std::exception &){
		}
		funct.setSide(SELL_SIDE);
		check(!books.find(funct).isValid());
		funct.setSide(BUY_SIDE);
		check(books.find(funct).isValid());
		{
			OrderBookImpl::OrdersT ords;
			funct.setSide(SELL_SIDE);
			books.findAll(funct, &ords);
			check(ords.empty());
		}
		{
			OrderBookImpl::OrdersT ords;
			funct.setSide(BUY_SIDE);
			books.findAll(funct, &ords);
			check(3 == ords.size());
			check(IdT(1, 3) == ords.at(0));
			check(IdT(1, 2) == ords.at(1));
			check(IdT(1, 1) == ords.at(2));
		}

		try{
			books.getTop(SourceIdT(), BUY_SIDE);
			check(false);
		}catch(const std::exception &){
		}
		try{
			books.getTop(instrId1, INVALID_SIDE);
			check(false);
		}catch(const std::exception &){
		}
		check(IdT(1, 3) == books.getTop(instrId1, BUY_SIDE));
		{
			ord->orderId_ = IdT(1, 2);
			ord->price_ = 5.0001;
			books.remove(*(ord.get()));

			OrderBookImpl::OrdersT ords;
			funct.setSide(BUY_SIDE);
			books.findAll(funct, &ords);
			check(2 == ords.size());
			check(IdT(1, 3) == ords.at(0));
			check(IdT(1, 1) == ords.at(1));
			check(IdT(1, 3) == books.getTop(instrId1, BUY_SIDE));

			ord->orderId_ = IdT(1, 3);
			ord->price_ = 5.00011;
			books.remove(*(ord.get()));
			check(IdT(1, 1) == books.getTop(instrId1, BUY_SIDE));
			ords.clear();
			books.findAll(funct, &ords);
			check(1 == ords.size());
			check(IdT(1, 1) == ords.at(0));

			ord->orderId_ = IdT(1, 3);
			ord->price_ = 5.0;
			try{
				books.remove(*(ord.get()));
			}catch(const std::exception &){}
			ord->orderId_ = IdT(1, 1);
			books.remove(*(ord.get()));
			check(!books.getTop(instrId1, BUY_SIDE).isValid());
			ords.clear();
			books.findAll(funct, &ords);
			check(0 == ords.size());
		}


		ord->orderId_ = IdT(1, 4);
		ord->price_ = 5.0;
		ord->side_ = SELL_SIDE;
		books.add(*(ord.get()));
		ord->orderId_ = IdT(1, 5);
		ord->price_ = 5.0001;
		books.add(*(ord.get()));
		ord->orderId_ = IdT(1, 6);
		ord->price_ = 5.00011;
		books.add(*(ord.get()));
		{
			OrderBookImpl::OrdersT ords;
			funct.setSide(SELL_SIDE);
			books.findAll(funct, &ords);
			check(3 == ords.size());
			check(IdT(1, 6) == ords.at(2));
			check(IdT(1, 5) == ords.at(1));
			check(IdT(1, 4) == ords.at(0));
		}
		check(IdT(1, 4) == books.getTop(instrId1, SELL_SIDE));
		{
			ord->orderId_ = IdT(1, 5);
			ord->price_ = 5.0001;
			books.remove(*(ord.get()));

			OrderBookImpl::OrdersT ords;
			funct.setSide(SELL_SIDE);
			books.findAll(funct, &ords);
			check(2 == ords.size());
			check(IdT(1, 4) == ords.at(0));
			check(IdT(1, 6) == ords.at(1));
			check(IdT(1, 4) == books.getTop(instrId1, SELL_SIDE));

			ord->orderId_ = IdT(1, 6);
			ord->price_ = 5.00011;
			books.remove(*(ord.get()));
			check(IdT(1, 4) == books.getTop(instrId1, SELL_SIDE));
			ords.clear();
			books.findAll(funct, &ords);
			check(1 == ords.size());
			check(IdT(1, 4) == ords.at(0));

			ord->orderId_ = IdT(1, 6);
			ord->price_ = 5.0;
			try{
				books.remove(*(ord.get()));
			}catch(const std::exception &){}
			ord->orderId_ = IdT(1, 4);
			books.remove(*(ord.get()));
			check(!books.getTop(instrId1, SELL_SIDE).isValid());
			ords.clear();
			books.findAll(funct, &ords);
			check(0 == ords.size());
		}

		WideDataStorage::destroy();
	}

	return true;
}