/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <memory>
#include <deque>
#include "TransactionDef.h"

namespace COP{

	namespace ACID{
		class TransactOperation;

class TransactionScope: public Scope, public Transaction
{
public:
	TransactionScope(void);
	~TransactionScope(void);

public:
	/// reimplemented from Scope
	virtual void addOperation(std::auto_ptr<Operation> &op);
	virtual void removeLastOperation();
	virtual size_t startNewStage();
	virtual void removeStage(const size_t &id);


public:
	/// reimplemented from Transaction
	virtual const TransactionId &transactionId()const;
	virtual void setTransactionId(const TransactionId &id);
	virtual void getRelatedObjects(ObjectsInTransactionT *obj)const;
	virtual bool executeTransaction(const Context &cnxt);

private:
	std::string invalidReason_;

	typedef std::deque<Operation *> OperationsT;
	OperationsT operations_;

	TransactionId id_;
private:
	TransactionScope(const TransactionScope&);
	const TransactionScope& operator=(const TransactionScope&);
};

	}
}