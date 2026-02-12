import type {
  Order,
  ExecutionReport,
  OrderBookSnapshot,
  Instrument,
  Account,
} from './index';

/** Server → Client messages (discriminated union on `type`) */
export type ServerMessage =
  | { type: 'connected' }
  | { type: 'order_snapshot'; data: Order[] }
  | { type: 'order_update'; data: Order }
  | { type: 'execution_report'; data: ExecutionReport }
  | { type: 'book_update'; data: OrderBookSnapshot }
  | { type: 'instrument_list'; data: Instrument[] }
  | { type: 'account_list'; data: Account[] }
  | { type: 'cancel_reject'; data: { orderId: number; reason: string } }
  | { type: 'business_reject'; data: { refId: number; reason: string } }
  | { type: 'error'; message: string };

/** Client → Server messages */
export type ClientMessage =
  | { type: 'new_order'; data: import('./index').NewOrderRequest }
  | { type: 'cancel_order'; data: import('./index').CancelOrderRequest }
  | { type: 'replace_order'; data: import('./index').ReplaceOrderRequest }
  | { type: 'subscribe_book'; symbol: string }
  | { type: 'unsubscribe_book'; symbol: string };
