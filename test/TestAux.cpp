/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "TestAux.h"

using namespace std;
using namespace test;
using namespace COP::ACID;

void test::check(bool rez, const std::string &error)
{
	if(!rez)
		throw std::logic_error(error);
}

TestTransactionContext::~TestTransactionContext()
{
	clear();
}

void TestTransactionContext::clear()
{
	for(size_t pos = 0; pos < op_.size(); ++pos){
		delete op_[pos];
	}
	op_.clear();
}

bool TestTransactionContext::isOperationEnqueued(COP::ACID::OperationType type)const
{
	for(size_t pos = 0; pos < op_.size(); ++pos){
		if(type == op_[pos]->type())
			return true;
	}	
	return false;
}

void TestTransactionContext::addOperation(std::unique_ptr<Operation> &op)
{
	op_.push_back(op.release());
}

void TestTransactionContext::removeLastOperation()
{

}

size_t TestTransactionContext::startNewStage()
{
	return 0;
}

void TestTransactionContext::removeStage(const size_t &)
{

}


const std::string &TestTransactionContext::invalidReason()const
{
	return reason_;
}

void TestTransactionContext::setInvalidReason(const std::string &reason)
{
	reason_ = reason;
}

bool TestTransactionContext::executeTransaction(const Context &)
{
	return false;
}
