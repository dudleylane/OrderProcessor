/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <list>
#include "QueuesDef.h"
#include "StateMachineDef.h"
#include "TransactionDef.h"
#include "OrderMatcher.h"
#include "DeferedEvents.h"

namespace COP{
	
	class IdTValueGenerator;
	class OrderBook;

	namespace Store{
		class OrderDataStorage;
	}

	namespace ACID{
		class TransactionManager;
	}

namespace Proc{

	struct ProcessorParams{
		IdTValueGenerator *generator_;
		Store::OrderDataStorage *orderStorage_;
		OrderBook *orderBook_;
		Queues::InQueues *inQueues_;
		Queues::OutQueues *outQueues_;
		Queues::InQueuesContainer *inQueue_;
		ACID::TransactionManager *transactMgr_;

		/// for the test purpose
		bool testStateMachine_;
		bool testStateMachineCheckResult_;	

		ProcessorParams(): generator_(nullptr), orderStorage_(nullptr), orderBook_(nullptr),
				inQueues_(nullptr), outQueues_(nullptr), inQueue_(nullptr), transactMgr_(nullptr), 
				testStateMachine_(false), testStateMachineCheckResult_(false)
		{}
		ProcessorParams(IdTValueGenerator *generator, Store::OrderDataStorage *orderStorage,
				OrderBook *orderBook, Queues::InQueues *inQueues, Queues::OutQueues *outQueues,
				Queues::InQueuesContainer *inQueue, ACID::TransactionManager *transactMgr, 
				bool testStateMachine = false, bool testStateMachineCheckResult = false): 
			generator_(generator), orderStorage_(orderStorage), orderBook_(orderBook), inQueues_(inQueues), 
			outQueues_(outQueues), inQueue_(inQueue), transactMgr_(transactMgr), 
			testStateMachine_(testStateMachine), testStateMachineCheckResult_(testStateMachineCheckResult)
		{}

	};

	class Processor: public Queues::InQueueProcessor, public DeferedEventContainer, 
		public DeferedEventFunctor, public ACID::TransactionProcessor
	{
	public:
		Processor(void);
		~Processor(void);

		void init(const ProcessorParams &params);

	public:
		/// reimplemented from DeferedEventContainer
		virtual void addDeferedEvent(DeferedEventBase *evnt);

	public:
		/// reimplemented from TransactionProcessor
		virtual void process(const ACID::TransactionId &id, ACID::Transaction *tr);

	public:
		/// reimplemented from DeferedEventFunctor
		virtual void process(OrdState::onTradeExecution &evnt, OrderEntry *order, const ACID::Context &cnxt);
		virtual void process(OrdState::onInternalCancel &evnt, OrderEntry *order, const ACID::Context &cnxt);

	public:
		/// reimplemented from InQueueProcessor
		/// retrives new event from inQueues, process it in transaction
		virtual bool process();
		virtual void onEvent(const std::string &source, const Queues::OrderEvent &evnt);
		virtual void onEvent(const std::string &source, const Queues::OrderCancelEvent &evnt);
		virtual void onEvent(const std::string &source, const Queues::OrderReplaceEvent &evnt);
		virtual void onEvent(const std::string &source, const Queues::OrderChangeStateEvent &evnt);
		virtual void onEvent(const std::string &source, const Queues::ProcessEvent &evnt);
		virtual void onEvent(const std::string &source, const Queues::TimerEvent &evnt);

		void onEvent(DeferedEventBase *evnt);

	private:
		void processDeferedEvent();
	private:
		IdTValueGenerator *generator_;
		Store::OrderDataStorage *orderStorage_;
		OrderBook *orderBook_;
		Queues::InQueues *inQueues_;
		Queues::OutQueues *outQueues_;
		Queues::InQueuesContainer *inQueue_;
		ACID::TransactionManager *transactMgr_;

		/// for the test purpose
		bool testStateMachine_;
		bool testStateMachineCheckResult_;

		OrdState::OrderState *stateMachine_;
		OrdState::OrderStatePersistence initialSMState_;

		OrderMatcher matcher_;

		typedef std::list<DeferedEventBase *> DeferedEventsT;
		DeferedEventsT events_;
	};

}
}