/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "TransactionScope.h"
#include "TrOperations.h"

using namespace std;
using namespace COP::ACID;

TransactionScope::TransactionScope(void)
{
}

TransactionScope::~TransactionScope(void)
{
	for(size_t pos = 0; pos < operations_.size(); ++pos){
		delete operations_[pos];
	}
}

const TransactionId &TransactionScope::transactionId()const
{
	return id_;
}

void TransactionScope::addOperation(std::unique_ptr<Operation> &op)
{
	operations_.push_back(op.release());
}

void TransactionScope::removeLastOperation()
{
	throw std::runtime_error("Not implemented!");
}

size_t TransactionScope::startNewStage()
{
	throw std::runtime_error("Not implemented!");
//	return 0;
}

void TransactionScope::removeStage(const size_t &)
{
	throw std::runtime_error("Not implemented!");
}

void TransactionScope::setTransactionId(const TransactionId &id)
{
	id_ = id;
}

void TransactionScope::getRelatedObjects(ObjectsInTransactionT *obj)const
{
	assert(nullptr != obj);
	obj->size_ = 0;
	for(OperationsT::const_iterator it = operations_.begin(); it != operations_.end(); ++it){
		const IdT &id = (*it)->getObjectId();
		assert(id.isValid());
		if(id.isValid()){
			bool duplicateId = false;
			for(size_t i = 0; i < obj->size_; ++i){
				duplicateId = (id == obj->list_[i].id_);
				if(duplicateId)
					break;
			}
			if(!duplicateId){
				/// todo: should be redesigned
				if(DEPENDANT_OBJECT_LIMIT - 1 == obj->size_)
					throw std::runtime_error("getRelatedObjects() failed to fill ObjectsInTransactionT, transaction use too many objects!");
				obj->list_[(obj->size_)++] = ObjectInTransaction(order_ObjectType, id);
			}
		}
		const IdT &rid = (*it)->getRelatedId();
		if(rid.isValid()){
			bool duplicateId = false;
			for(size_t i = 0; i < obj->size_; ++i){
				duplicateId = (rid == obj->list_[i].id_);
				if(duplicateId)
					break;
			}
			if(!duplicateId){
				/// todo: should be redesigned
				if(DEPENDANT_OBJECT_LIMIT - 1 == obj->size_)
					throw std::runtime_error("getRelatedObjects() failed to fill ObjectsInTransactionT, transaction use too many objects!");
				obj->list_[(obj->size_)++] = ObjectInTransaction(instrument_ObjectType, rid);
			}
		}
	}
}

bool TransactionScope::executeTransaction(const Context &cnxt)
{
	if(operations_.empty())
		return true;

	size_t pos = 0;
	// execute commit
	try{
		for(;pos < operations_.size(); ++pos){
			operations_[pos]->execute(cnxt);
		}
		return true;
	}catch(const std::exception &){
	}catch(...){
	}

	// commit failed - execute rollback
	try{
		// pos points to failed operation, rollback from there to beginning
		// Use do-while to handle unsigned wraparound safely
		do {
			operations_[pos]->rollback(cnxt);
		} while (pos-- > 0);
	}catch(const std::exception &){
	}catch(...){
	}
	return false;
}
