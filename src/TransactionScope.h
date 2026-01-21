/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <string>
#include <memory>
#include <deque>
#include <vector>
#include "TransactionDef.h"

namespace COP{

	namespace ACID{
		class TransactOperation;

class TransactionScope final : public Scope, public Transaction
{
public:
	TransactionScope(void);
	~TransactionScope(void);

	/**
	 * Reset the TransactionScope for reuse without deallocation.
	 * Clears all operations and stage boundaries while preserving
	 * allocated capacity for better performance.
	 */
	void reset();

	/**
	 * Swap the contents of this TransactionScope with another.
	 * Used by the pool to transfer data from a pooled scope to a heap-allocated one.
	 */
	void swap(TransactionScope& other) noexcept;

public:
	/// reimplemented from Scope
	virtual void addOperation(std::unique_ptr<Operation> &op);
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

	// Stage boundaries - each entry is the starting index of a stage in operations_
	typedef std::vector<size_t> StageBoundariesT;
	StageBoundariesT stageBoundaries_;

	TransactionId id_;
private:
	TransactionScope(const TransactionScope&);
	const TransactionScope& operator=(const TransactionScope&);
};

	}
}