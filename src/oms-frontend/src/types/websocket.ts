import type {
  Order,
  ExecutionReport,
  OrderBookSnapshot,
  Instrument,
  Account,
  SystemMetrics,
} from './index';

/** Server → Client messages (discriminated union on `type`) */
export type ServerMessage =
  | { type: 'connected' }
  | { type: 'order_snapshot'; data: Order[] }
  /** requestId: on an order's first update to the session that sent its new_order, the id the request gave (#790) */
  | { type: 'order_update'; data: Order; requestId?: string }
  | { type: 'execution_report'; data: ExecutionReport }
  | { type: 'book_update'; data: OrderBookSnapshot }
  | { type: 'instrument_list'; data: Instrument[] }
  | { type: 'account_list'; data: Account[] }
  | { type: 'cancel_reject'; data: { orderId: number; reason: string } }
  | { type: 'metrics_update'; data: SystemMetrics }
  | { type: 'error'; message: string; requestId?: string };

/** Client → Server messages */
export type ClientMessage =
  | { type: 'new_order'; data: import('./index').NewOrderRequest }
  | { type: 'cancel_order'; data: import('./index').CancelOrderRequest }
  | { type: 'replace_order'; data: import('./index').ReplaceOrderRequest }
  | { type: 'subscribe_book'; symbol: string }
  | { type: 'unsubscribe_book'; symbol: string };
