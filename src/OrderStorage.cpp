/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "OrderStorage.h"
#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "Logger.h"

using namespace COP;
using namespace std;
using namespace COP::Store;

OrderDataStorage::OrderDataStorage(): saver_(nullptr)
{
	aux::ExchLogger::instance()->note("OrderDataStorage created");
}

void OrderDataStorage::attach(OrderSaver *saver){
	assert(nullptr == saver_);
	saver_ = saver;
}

OrderDataStorage::~OrderDataStorage(void)
{
	aux::ExchLogger::instance()->note("OrderDataStorage destroying");

	// Clean up orders with exclusive write lock
	OrdersByIDT tmp;
	{
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
		std::swap(tmp, ordersById_);
		ordersByClId_.clear();
	}
	for(OrdersByIDT::iterator it = tmp.begin(); it != tmp.end(); ++it)
		delete it->second;

	// Clean up executions - iterate through concurrent_hash_map
	// No lock needed for iteration during destruction (single-threaded)
	for(ExecByIDT::iterator it = executionsById_.begin(); it != executionsById_.end(); ++it)
		delete it->second;
	executionsById_.clear();

	aux::ExchLogger::instance()->note("OrderDataStorage destroyed");
}

// ============================================================================
// Order operations - use reader-writer lock for concurrent reads
// ============================================================================

OrderEntry *OrderDataStorage::locateByClOrderId(const RawDataEntry &clOrderId)const
{
	// Shared read lock - allows concurrent lookups
	oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
	OrdersByClientIDT::const_iterator it = ordersByClId_.find(clOrderId);
	if(ordersByClId_.end() == it) [[unlikely]]
		return nullptr;
	return it->second;
}

OrderEntry *OrderDataStorage::locateByOrderId(const IdT &orderId)const
{
	// Shared read lock - allows concurrent lookups
	oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, false);
	OrdersByIDT::const_iterator it = ordersById_.find(orderId);
	if(ordersById_.end() == it) [[unlikely]]
		return nullptr;
	return it->second;
}

OrderEntry *OrderDataStorage::save(const OrderEntry &order, IdTValueGenerator *idGenerator)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OrderDataStorage saving order");

	assert(nullptr != idGenerator);

	OrderEntry *result = nullptr;
	{
		// Exclusive write lock - atomic dual-map insert
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
		if((order.orderId_.isValid())&&(ordersById_.end() != ordersById_.find(order.orderId_)))
			throw std::runtime_error("Unable to save order - order with same OrderId already exists.");

		if(0 == order.clOrderId_.get().length_)
			throw std::runtime_error("Unable to save order - order contains empty ClOrderId.");
		if(ordersByClId_.end() != ordersByClId_.find(order.clOrderId_.get()))
			throw std::runtime_error("Unable to save order - order with same ClOrderId already exists.");

		std::unique_ptr<OrderEntry> cp(order.clone());
		if(!cp->orderId_.isValid())
			cp->orderId_ = idGenerator->getId();
		int st = 0;
		try{
			ordersById_.insert(OrdersByIDT::value_type(cp->orderId_, cp.get()));
			st = 1;
			ordersByClId_.insert(OrdersByClientIDT::value_type(cp->clOrderId_.get(), cp.get()));
			st = 2;
			result = cp.release();
		}catch(...){
			switch(st){
			case 2:
				ordersByClId_.erase(order.clOrderId_.get());
				[[fallthrough]];
			case 1:
				ordersById_.erase(cp->orderId_);
			}
			throw;
		}
	}
	// Call saver outside of lock to avoid potential deadlock
	if(nullptr != saver_ && nullptr != result)
		saver_->save(*result);
	return result;
}

void OrderDataStorage::restore(OrderEntry *order)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OrderDataStorage restoring order");

	bool shouldSave = false;
	{
		// Exclusive write lock - atomic dual-map insert
		oneapi::tbb::spin_rw_mutex::scoped_lock lock(orderRwLock_, true);
		if((order->orderId_.isValid())&&(ordersById_.end() != ordersById_.find(order->orderId_)))
			throw std::runtime_error("Unable to restore order - order with same OrderId already exists.");
		if(0 == order->clOrderId_.get().length_)
			throw std::runtime_error("Unable to restore order - order contains empty ClOrderId.");
		if(ordersByClId_.end() != ordersByClId_.find(order->clOrderId_.get()))
			throw std::runtime_error("Unable to restore order - order with same ClOrderId already exists.");

		int st = 0;
		try{
			ordersById_.insert(OrdersByIDT::value_type(order->orderId_, order));
			st = 1;
			ordersByClId_.insert(OrdersByClientIDT::value_type(order->clOrderId_.get(), order));
			st = 2;
			shouldSave = true;
		}catch(...){
			switch(st){
			case 2:
				ordersByClId_.erase(order->clOrderId_.get());
				[[fallthrough]];
			case 1:
				ordersById_.erase(order->orderId_);
			}
			throw;
		}
	}
	// Call saver outside of lock to avoid potential deadlock
	if(nullptr != saver_ && shouldSave)
		saver_->save(*order);
}

// ============================================================================
// Execution operations - use concurrent_hash_map for fine-grained locking
// ============================================================================

ExecutionEntry *OrderDataStorage::locateByExecId(const IdT &execId)const
{
	// Lock-free lookup using concurrent_hash_map accessor
	ExecByIDT::const_accessor accessor;
	if(executionsById_.find(accessor, execId))
		return accessor->second;
	return nullptr;
}

void OrderDataStorage::save(const ExecutionEntry *exec)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OrderDataStorage saving execution");

	// Use accessor to check and insert atomically
	ExecByIDT::accessor accessor;
	if(!executionsById_.insert(accessor, exec->execId_)) {
		// Key already exists
		throw std::runtime_error("Unable to save execution - execution with same ExecId already exists.");
	}
	accessor->second = exec->clone();
}

ExecutionEntry *OrderDataStorage::save(const ExecutionEntry &exec, IdTValueGenerator *idGenerator)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OrderDataStorage saving execution 2");

	assert(nullptr != idGenerator);

	std::unique_ptr<ExecutionEntry> cp(exec.clone());
	if(!cp->execId_.isValid())
		cp->execId_ = idGenerator->getId();

	// Use accessor to check and insert atomically
	ExecByIDT::accessor accessor;
	if(!executionsById_.insert(accessor, cp->execId_)) {
		// Key already exists
		throw std::runtime_error("Unable to save execution - execution with same ExecId already exists.");
	}
	accessor->second = cp.get();
	return cp.release();
}
