import {
  OrderType,
  Side,
  TimeInForce,
  OrderStatus,
  ExecType,
  Currency,
  Capacity,
  AccountType,
} from './enums';

export * from './enums';

/** Maps to C++ InstrumentEntry */
export interface Instrument {
  id: number;
  symbol: string;
  securityId: string;
  securityIdSource: string;
}

/** Maps to C++ AccountEntry */
export interface Account {
  id: number;
  account: string;
  firm: string;
  type: AccountType;
}

/** Maps to C++ OrderParams / OrderEntry */
export interface Order {
  orderId: number;
  origOrderId: number;
  clOrderId: string;
  origClOrderId: string;
  symbol: string;
  side: Side;
  ordType: OrderType;
  price: number;
  stopPx: number;
  avgPx: number;
  orderQty: number;
  cumQty: number;
  leavesQty: number;
  minQty: number;
  status: OrderStatus;
  tif: TimeInForce;
  capacity: Capacity;
  currency: Currency;
  account: string;
  destination: string;
  source: string;
  creationTime: number;
  lastUpdateTime: number;
  expireTime: number;
}

/** Maps to C++ ExecParams (base execution report) */
export interface ExecutionReport {
  execId: number;
  orderId: number;
  type: ExecType;
  orderStatus: OrderStatus;
  market: string;
  transactTime: number;
}

/** Maps to C++ TradeExecEntry */
export interface TradeExecution extends ExecutionReport {
  type: ExecType.TRADE;
  lastQty: number;
  lastPx: number;
  currency: Currency;
  tradeDate: number;
}

/** Maps to C++ RejectExecEntry */
export interface RejectExecution extends ExecutionReport {
  type: ExecType.REJECT;
  rejectReason: string;
}

/** Maps to C++ ReplaceExecEntry */
export interface ReplaceExecution extends ExecutionReport {
  type: ExecType.REPLACE;
  origOrderId: number;
}

/** Maps to C++ ExecCorrectExecEntry */
export interface CorrectExecution extends ExecutionReport {
  type: ExecType.CORRECT;
  cumQty: number;
  leavesQty: number;
  lastQty: number;
  lastPx: number;
  currency: Currency;
  tradeDate: number;
  origOrderId: number;
  execRefId: number;
}

/** Maps to C++ TradeCancelExecEntry */
export interface TradeCancelExecution extends ExecutionReport {
  type: ExecType.CANCEL;
  execRefId: number;
}

export type Execution =
  | TradeExecution
  | RejectExecution
  | ReplaceExecution
  | CorrectExecution
  | TradeCancelExecution
  | ExecutionReport;

/** Client submission types */
export interface NewOrderRequest {
  symbol: string;
  side: Side;
  ordType: OrderType;
  price?: number;
  stopPx?: number;
  orderQty: number;
  minQty?: number;
  tif: TimeInForce;
  account?: string;
  currency?: Currency;
  capacity?: Capacity;
}

export interface CancelOrderRequest {
  orderId: number;
  clOrderId?: string;
}

export interface ReplaceOrderRequest {
  orderId: number;
  price?: number;
  orderQty?: number;
  tif?: TimeInForce;
}

/** Order book depth */
export interface BookLevel {
  price: number;
  qty: number;
  orderCount: number;
}

export interface OrderBookSnapshot {
  symbol: string;
  bids: BookLevel[];
  asks: BookLevel[];
  timestamp: number;
}

/** Client-side computed position */
export interface Position {
  symbol: string;
  netQty: number;
  avgPx: number;
  tradeCount: number;
  lastTradeTime: number;
}
