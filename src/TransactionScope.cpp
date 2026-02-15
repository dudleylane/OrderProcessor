/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include <utility>  // For std::swap
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

void TransactionScope::reset()
{
	// Delete all operations but keep the container capacity
	for(size_t pos = 0; pos < operations_.size(); ++pos){
		delete operations_[pos];
	}
	operations_.clear();
	// Note: std::deque doesn't have shrink_to_fit guarantee but clear() is efficient

	// Clear stage boundaries, preserving vector capacity
	stageBoundaries_.clear();

	// Reset transaction ID
	id_ = TransactionId();

	// Clear invalid reason string, preserving capacity
	invalidReason_.clear();
}

void TransactionScope::swap(TransactionScope& other) noexcept
{
	// Swap all member variables efficiently
	invalidReason_.swap(other.invalidReason_);
	operations_.swap(other.operations_);
	stageBoundaries_.swap(other.stageBoundaries_);
	std::swap(id_, other.id_);
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
	if(operations_.empty())
		return;

	// Delete the last operation and remove it from the deque
	delete operations_.back();
	operations_.pop_back();

	// If the last operation was at a stage boundary, remove that boundary too
	while(!stageBoundaries_.empty() && stageBoundaries_.back() >= operations_.size()) {
		stageBoundaries_.pop_back();
	}
}

size_t TransactionScope::startNewStage()
{
	// Record the current position as the start of a new stage
	size_t stageId = stageBoundaries_.size();
	stageBoundaries_.push_back(operations_.size());
	return stageId;
}

void TransactionScope::removeStage(const size_t &stageId)
{
	if(stageId >= stageBoundaries_.size())
		throw std::runtime_error("TransactionScope::removeStage(): invalid stage id!");

	// Get the starting index of this stage
	size_t stageStart = stageBoundaries_[stageId];

	// Delete all operations from stageStart to the end
	for(size_t i = stageStart; i < operations_.size(); ++i) {
		delete operations_[i];
	}

	// Remove operations from stageStart onwards
	operations_.erase(operations_.begin() + static_cast<std::ptrdiff_t>(stageStart), operations_.end());

	// Remove this stage and all subsequent stage boundaries
	stageBoundaries_.erase(stageBoundaries_.begin() + static_cast<std::ptrdiff_t>(stageId), stageBoundaries_.end());
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
				if(DEPENDANT_OBJECT_LIMIT == obj->size_)
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
				if(DEPENDANT_OBJECT_LIMIT == obj->size_)
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
