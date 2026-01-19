/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <stdexcept>
#include "OrderStorage.h"
#include "DataModelDef.h"
#include "IdTGenerator.h"
#include "Logger.h"

using namespace COP;
using namespace tbb;
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
	OrdersByIDT tmp;
	{
		tbb::mutex::scoped_lock lock(orderLock_);
		std::swap(tmp, ordersById_);
		ordersByClId_.clear();
	}
	for(OrdersByIDT::iterator it = tmp.begin(); it != tmp.end(); ++it)
		delete it->second;

	ExecByIDT execTmp;
	{
		tbb::mutex::scoped_lock lock(execLock_);
		std::swap(execTmp, executionsById_);
	}
	for(ExecByIDT::iterator it = execTmp.begin(); it != execTmp.end(); ++it)
		delete it->second;
	aux::ExchLogger::instance()->note("OrderDataStorage destroyed");
}

OrderEntry *OrderDataStorage::locateByClOrderId(const RawDataEntry &clOrderId)const
{
	tbb::mutex::scoped_lock lock(orderLock_);
	OrdersByClientIDT::const_iterator it = ordersByClId_.find(clOrderId);
	if(ordersByClId_.end() == it)
		return nullptr;
	return it->second;
}

OrderEntry *OrderDataStorage::locateByOrderId(const IdT &orderId)const
{
	/// changed for concurrent_hash_map
	tbb::mutex::scoped_lock lock(orderLock_);
	OrdersByIDT::const_iterator it = ordersById_.find(orderId);
	if(ordersById_.end() == it)
		return nullptr;
	return it->second;
}

OrderEntry *OrderDataStorage::save(const OrderEntry &order, IdTValueGenerator *idGenerator)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OrderDataStorage saving order");

	assert(nullptr != idGenerator);
	{
		tbb::mutex::scoped_lock lock(orderLock_);
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
			if(nullptr != saver_)
				saver_->save(*cp.get());
			return cp.release();
		}catch(...){
			switch(st){
			case 2:
				ordersByClId_.erase(order.clOrderId_.get());
			case 1:
				ordersById_.erase(order.orderId_);
			}
			throw;
		}
	}
}

void OrderDataStorage::restore(OrderEntry *order)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OrderDataStorage restoring order");

	{
		tbb::mutex::scoped_lock lock(orderLock_);
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
			if(nullptr != saver_)
				saver_->save(*order);
			return;
		}catch(...){
			switch(st){
			case 2:
				ordersByClId_.erase(order->clOrderId_.get());
			case 1:
				ordersById_.erase(order->orderId_);
			}
			throw;
		}
	}
}

ExecutionEntry *OrderDataStorage::locateByExecId(const IdT &execId)const
{
	tbb::mutex::scoped_lock lock(execLock_);
	ExecByIDT::const_iterator it = executionsById_.find(execId);
	if(executionsById_.end() == it)
		return nullptr;
	return it->second;
}

void OrderDataStorage::save(const ExecutionEntry *exec)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OrderDataStorage saving execution");

	tbb::mutex::scoped_lock lock(execLock_);
	if(executionsById_.end() != executionsById_.find(exec->execId_))
		throw std::runtime_error("Unable to save execution - execution with same ExecId already exists.");
	executionsById_.insert(ExecByIDT::value_type(exec->execId_, exec->clone()));
	//assert(nullptr != saver_);
	//saver_->save(*cp.get());
}

ExecutionEntry *OrderDataStorage::save(const ExecutionEntry &exec, IdTValueGenerator *idGenerator)
{
	if(aux::ExchLogger::instance()->isNoteOn())
		aux::ExchLogger::instance()->note("OrderDataStorage saving execution 2");

	assert(nullptr != idGenerator);
	tbb::mutex::scoped_lock lock(execLock_);
	if((exec.execId_.isValid())&&(executionsById_.end() != executionsById_.find(exec.execId_)))
		throw std::runtime_error("Unable to save execution - execution with same ExecId already exists.");

	std::unique_ptr<ExecutionEntry> cp(exec.clone());
	if(!cp->execId_.isValid())
		cp->execId_ = idGenerator->getId();
	executionsById_.insert(ExecByIDT::value_type(cp->execId_, cp.get()));
	//assert(nullptr != saver_);
	//saver_->save(*cp.get());
	return cp.release();
}
