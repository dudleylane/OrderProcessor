/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include "TransactionDef.h"
#include "DataModelDef.h"
#include "OrderStateEvents.h"

namespace COP{

	namespace ACID{
	
		class CreateExecReportTrOperation: public Operation{
		public:
			CreateExecReportTrOperation(OrderStatus status, ExecType execType, const OrderEntry &order);
			~CreateExecReportTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			OrderStatus status_;
			ExecType execType_;
		};

		class CreateTradeExecReportTrOperation: public Operation{
		public:
			CreateTradeExecReportTrOperation(const TradeExecEntry *trade, OrderStatus status, const OrderEntry &order);
			~CreateTradeExecReportTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			QuantityT lastQty_;
			PriceT lastPx_;
			DateTimeT tradeDate_;
			Currency currency_;
			OrderStatus status_;
		};

		class CreateRejectExecReportTrOperation: public Operation{
		public:
			CreateRejectExecReportTrOperation(const std::string &reason, OrderStatus status, const OrderEntry &order);
			~CreateRejectExecReportTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			std::string reason_;
			OrderStatus status_;
		};

		class CreateReplaceExecReportTrOperation: public Operation{
		public:
			CreateReplaceExecReportTrOperation(const IdT &origOrderId, OrderStatus status, const OrderEntry &order);
			~CreateReplaceExecReportTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			IdT origOrderId_;
			OrderStatus status_;
		};

		class CreateCorrectExecReportTrOperation: public Operation{
		public:
			CreateCorrectExecReportTrOperation(const ExecCorrectExecEntry *correct, OrderStatus status, const OrderEntry &order);
			~CreateCorrectExecReportTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			QuantityT cumQty_;
			QuantityT leavesQty_;
			QuantityT lastQty_;
			PriceT lastPx_;
			Currency currency_;
			DateTimeT tradeDate_;
			IdT origOrderId_;
			IdT execRefId_;

			OrderStatus status_;
		};

		class AddToOrderBookTrOperation: public Operation{
		public:
			AddToOrderBookTrOperation(const OrderEntry &order, const IdT &instrId);
			~AddToOrderBookTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			AddToOrderBookTrOperation(const AddToOrderBookTrOperation &);
			AddToOrderBookTrOperation &operator=(const AddToOrderBookTrOperation &);
			const OrderEntry &order_;
		};

		class RemoveFromOrderBookTrOperation: public Operation{
		public:
			RemoveFromOrderBookTrOperation(const OrderEntry &order, const IdT &instrId);
			~RemoveFromOrderBookTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			RemoveFromOrderBookTrOperation(const RemoveFromOrderBookTrOperation &);
			RemoveFromOrderBookTrOperation &operator=(const RemoveFromOrderBookTrOperation &);

			const OrderEntry &order_;
		};

		void executeEnqueueOrderEvent(const OrdState::onReplaceReceived &event_, const Context &cnxt);
		void rollbackEnqueueOrderEvent(const OrdState::onReplaceReceived &event_, const Context &cnxt);
		void executeEnqueueOrderEvent(const OrdState::onOrderAccepted &event_, const Context &cnxt);
		void rollbackEnqueueOrderEvent(const OrdState::onOrderAccepted &event_, const Context &cnxt);
		void executeEnqueueOrderEvent(const OrdState::onExecReplace &event_, const Context &cnxt);
		void rollbackEnqueueOrderEvent(const OrdState::onExecReplace &event_, const Context &cnxt);
		void executeEnqueueOrderEvent(const OrdState::onReplaceRejected &event_, const Context &cnxt);
		void rollbackEnqueueOrderEvent(const OrdState::onReplaceRejected &event_, const Context &cnxt);

		template<typename T>
		class EnqueueOrderEventTrOperation: public Operation{
		public:
			EnqueueOrderEventTrOperation(const OrderEntry &order, const T& evnt):
				Operation(ENQUEUE_EVENT_TROPERATION, order.orderId_), event_(evnt){}
			EnqueueOrderEventTrOperation(const OrderEntry &order, const T& evnt, const IdT &instrId):
				Operation(ENQUEUE_EVENT_TROPERATION, order.orderId_, instrId), event_(evnt){}

			~EnqueueOrderEventTrOperation(){}

			virtual void execute(const Context &cnxt){
				executeEnqueueOrderEvent(event_, cnxt);
			}
			virtual void rollback(const Context &cnxt){
				rollbackEnqueueOrderEvent(event_, cnxt);
			}
		private:
			T event_;
		};

		class CancelRejectTrOperation: public Operation{
		public:
			CancelRejectTrOperation(OrderStatus status, const OrderEntry &order);
			~CancelRejectTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			OrderStatus status_;
		};

		class MatchOrderTrOperation: public Operation{
		public:
			MatchOrderTrOperation(OrderEntry *order);
			~MatchOrderTrOperation();

			virtual void execute(const Context &cnxt);
			virtual void rollback(const Context &cnxt);
		private:
			OrderEntry *order_;
		};

	}
}