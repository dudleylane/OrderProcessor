/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/


#ifndef __TEST_AUX__H_
#define __TEST_AUX__H_

#include <string>
#include <deque>

#include "DataModelDef.h"
#include "TransactionDef.h"

namespace test{

	void check(bool rez, const std::string &error = "");

	class TestTransactionContext: public COP::ACID::Scope{
	public:
		~TestTransactionContext();
		virtual void addOperation(std::unique_ptr<COP::ACID::Operation> &op);
		virtual const std::string &invalidReason()const;
		virtual void setInvalidReason(const std::string &reason);
		virtual bool executeTransaction(const COP::ACID::Context &cnxt);

		virtual void removeLastOperation();
		virtual size_t startNewStage();
		virtual void removeStage(const size_t &id);

		void clear();
		bool isOperationEnqueued(COP::ACID::OperationType type)const;
	public:
		std::string reason_;
		std::deque<COP::ACID::Operation *> op_;
	};

	class DummyOrderSaver: public COP::OrderSaver{
	public:
		DummyOrderSaver(){}
		~DummyOrderSaver(){}

		virtual void save(const COP::OrderEntry&){}
	};

}

#endif //__TEST_AUX__H_