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
#include <vector>
#include <set>

#include "TypesDef.h"

namespace COP{

	class OrderBook;
	class IdTValueGenerator;

	namespace Store{
		class OrderDataStorage;
	}
	namespace Queues{
		class InQueues;
		class OutQueues;
	}

	namespace Proc{
		class OrderMatcher;
	}

	namespace ACID{

		typedef IdT TransactionId;
		typedef std::set<TransactionId> TransactionIdsT;

		enum ObjectType{
			invalid_ObjectType = 0,
			instrument_ObjectType,
			order_ObjectType,
			execution_ObjectType
		};

		struct ObjectInTransaction{
			IdT id_;
			ObjectType type_;

			ObjectInTransaction():type_(invalid_ObjectType), id_(){}
			ObjectInTransaction(const ObjectType &type, const IdT &id):type_(type), id_(id){}
		};
		inline bool operator< (const ObjectInTransaction &left, const ObjectInTransaction &right)	{
			if(left.type_ == right.type_)
				return left.id_ < right.id_;
			return left.type_ < right.type_;
		}

		inline bool operator== (const ObjectInTransaction &left, const ObjectInTransaction &right){
			return (left.type_ == right.type_)&& (left.id_ == right.id_);
		}

		const size_t DEPENDANT_OBJECT_LIMIT = 10;
		struct ObjectsInTransaction{
			ObjectInTransaction list_[DEPENDANT_OBJECT_LIMIT];
			unsigned char size_;

			ObjectsInTransaction(): size_(0){}
		};


		typedef ObjectsInTransaction ObjectsInTransactionT;

		typedef enum OperationType{
			INVALID_TROPERATION = 0,
			CREATE_EXECREPORT_TROPERATION, // creates and enqueues ExecutionReport for the changed order
			ADD_ORDERBOOK_TROPERATION, // adds order into orderBook
			REMOVE_ORDERBOOK_TROPERATION, // removes order from orderBook
			ENQUEUE_EVENT_TROPERATION, // enqueues event for processing
			CANCEL_REJECT_TROPERATION,
			MATCH_ORDER_TROPERATION,
			CREATE_TRADE_EXECREPORT_TROPERATION, // creates and enqueues Trade ExecutionReport for the changed order
			CREATE_REJECT_EXECREPORT_TROPERATION, // creates and enqueues Reject ExecutionReport for the changed order
			CREATE_REPLACE_EXECREPORT_TROPERATION, // creates and enqueues replace ExecutionReport for the changed order
			CREATE_CORRECT_EXECREPORT_TROPERATION // creates and enqueues Correct ExecutionReport for the changed order
		};

		struct Context{
			Store::OrderDataStorage *orderStorage_;
			OrderBook *orderBook_;
			Queues::InQueues *inQueues_;
			Queues::OutQueues *outQueues_;
			Proc::OrderMatcher *orderMatch_;
			IdTValueGenerator *idGenerator_;

			Context(): 
				orderStorage_(nullptr), orderBook_(nullptr), inQueues_(nullptr), outQueues_(nullptr), 
				orderMatch_(nullptr), idGenerator_(nullptr)
			{}

			Context(Store::OrderDataStorage *orderStorage, OrderBook *orderBook,
					Queues::InQueues *inQueues, Queues::OutQueues *outQueues, 
					Proc::OrderMatcher *orderMatch, IdTValueGenerator *idGenerator): 
				orderStorage_(orderStorage), orderBook_(orderBook), inQueues_(inQueues), 
				outQueues_(outQueues), orderMatch_(orderMatch), idGenerator_(idGenerator)
			{}
		};

		class Operation{
		public:
			explicit Operation(OperationType type, const IdT &id): type_(type), id_(id), relatedId_(){}
			explicit Operation(OperationType type, const IdT &id, const IdT &relId): 
						type_(type), id_(id), relatedId_(relId){}
			virtual ~Operation(){}
			virtual void execute(const Context &cnxt) = 0;
			virtual void rollback(const Context &cnxt) = 0;

			OperationType type()const{return type_;}
			const IdT &getObjectId()const{return id_;}
			const IdT &getRelatedId()const{return relatedId_;}
		protected:
			OperationType type_;
			IdT id_;
			IdT relatedId_;
		};

		class Scope{
		public:
			~Scope(){}

			/// add operation into the transaction
			virtual void addOperation(std::unique_ptr<Operation> &op) = 0;

			/// removes last operation from transaction scope
			virtual void removeLastOperation() = 0;

			/// stage is a set of operations - sub transaction
			/// add new stage - all operations before next startNewStage() will be related to the same stage
			virtual size_t startNewStage() = 0;

			/// removes operation's related to the passed stage from transaction
			virtual void removeStage(const size_t &id) = 0;
		};

		class Transaction{
		public:
			virtual ~Transaction(){}

			/// returns id of the transaction
			virtual const TransactionId &transactionId()const = 0;
			virtual void setTransactionId(const TransactionId &id) = 0;
			virtual void getRelatedObjects(ObjectsInTransactionT *obj)const = 0;

			/// executes all operations of the transaction. 
			/// returns false if transaction was rolledback
			virtual bool executeTransaction(const Context &cnxt) = 0;
		};

		class TransactionObserver{
		public:
			virtual ~TransactionObserver(){}

			virtual void onReadyToExecute() = 0;
		};

		class TransactionIterator{
		public:
			virtual ~TransactionIterator(){}

			virtual bool next(TransactionId *id, Transaction **tr) = 0;
			virtual bool get(TransactionId *id, Transaction **tr)const = 0;
			virtual bool isValid()const = 0;
		};

		class TransactionProcessor{
		public:
			virtual ~TransactionProcessor(){}

			virtual void process(const TransactionId &id, Transaction *tr) = 0;
		};
		typedef std::vector<TransactionProcessor *> ProcessorPoolT;

		class TransactionManager{
		public:
			virtual ~TransactionManager(){}

			/// attaches event observer 
			virtual void attach(TransactionObserver *obs) = 0;

			/// dettaches event observer 
			virtual TransactionObserver *detach() = 0;

			/// add new transaction for execution
			virtual void addTransaction(std::unique_ptr<Transaction> &tr) = 0;

			/// remove transaction, do not execute it
			virtual bool removeTransaction(const TransactionId &id, Transaction *t) = 0;

			/// returns ids of all transactions that this depends on
			virtual bool getParentTransactions(const TransactionId &id, TransactionIdsT *parent)const = 0;

			/// returns ids of all transactions that blocked by this one
			virtual bool getRelatedTransactions(const TransactionId &id, TransactionIdsT *related)const = 0;

			virtual TransactionIterator *iterator() = 0;
		};

	}
}