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
  /** A STOP or STOPLIMIT order is held until a trade reaches its stop price, then released (#223) */
  stopTriggered: boolean;
  avgPx: number;
  orderQty: number;
  cumQty: number;
  leavesQty: number;
  minQty: number;
  /** The business day's figures (#244): what the order had left to trade when the day began, and the day's fills */
  dayOrderQty: number;
  dayCumQty: number;
  dayAvgPx: number;
  /** When that day began, 17:00 New York, ms since the epoch; 0 for none */
  dayStart: number;
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
  /** Only on an FX swap: its near leg's settlement date (ms since the epoch) */
  settlDate?: number;
  /** Only on an FX swap: its far leg's price */
  farPrice?: number;
  /** Only on an FX swap: its far leg's settlement date (ms since the epoch) */
  farSettlDate?: number;
}

/**
 * Maps to C++ ExecParams (base execution report). The server sends the fields of the report's C++ class, not of its
 * type (#179): a report's type alone doesn't tell a refused replace from a replacement, or an order cancel from a trade
 * cancel. A plain ExecutionEntry carries only these fields: a new order's NEW, an order cancel's CANCEL, an expiry and
 * the rest. The is* guards below tell the classes apart by the fields each adds.
 */
export interface ExecutionReport {
  execId: number;
  orderId: number;
  type: ExecType;
  orderStatus: OrderStatus;
  market: string;
  transactTime: number;
  /** Only on an FX swap's fill: the leg it reports */
  execLegType?: 'NEAR' | 'FAR';
}

/** Maps to C++ TradeExecEntry */
export interface TradeExecution extends ExecutionReport {
  type: ExecType.TRADE;
  lastQty: number;
  lastPx: number;
  currency: Currency;
  /** The business day the trade counts in, YYYYMMDD: the date it ends on, 17:00 New York (#258) */
  tradeDate: number;
}

/**
 * Maps to C++ RejectExecEntry: a refused order is type REJECT, and a refused replace type REPLACE (#56). PEND_REPLACE
 * comes only from the legacy replace path, which the server no longer raises (#74).
 */
export interface RejectExecution extends ExecutionReport {
  type: ExecType.REJECT | ExecType.REPLACE | ExecType.PEND_REPLACE;
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
  /** The business day the trade counts in, YYYYMMDD: the date it ends on, 17:00 New York (#258) */
  tradeDate: number;
  origOrderId: number;
  execRefId: number;
}

/** Maps to C++ TradeCancelExecEntry. An order cancel has the same type, CANCEL, but no execRefId: it's a plain report. */
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

/** A refused order or replace: any report with a reject reason (#179) */
export function isRejection(exec: Execution): exec is RejectExecution {
  return 'rejectReason' in exec;
}

export function isTrade(exec: Execution): exec is TradeExecution {
  return exec.type === ExecType.TRADE && 'lastQty' in exec;
}

/** A replacement, as opposed to a refused replace, which has the same type (#179) */
export function isReplacement(exec: Execution): exec is ReplaceExecution {
  return exec.type === ExecType.REPLACE && 'origOrderId' in exec;
}

export function isCorrection(exec: Execution): exec is CorrectExecution {
  return exec.type === ExecType.CORRECT && 'execRefId' in exec;
}

/** A trade cancel, as opposed to an order cancel, which has the same type (#179) */
export function isTradeCancel(exec: Execution): exec is TradeCancelExecution {
  return exec.type === ExecType.CANCEL && 'execRefId' in exec;
}

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
  /** A GTD order's expiry, in milliseconds since the epoch, required for GTD and refused for any other (#240) */
  expireTime?: number;
  account?: string;
  // Required by the server, which refuses an order without them (#166)
  currency: Currency;
  capacity: Capacity;
}

/** An FX swap: buy (or sell) at the near price, and the reverse at the far price */
export interface NewSwapOrderRequest {
  symbol: string;
  side: Side;
  nearPrice: number;
  farPrice: number;
  /** the near leg's settlement date, ms since the epoch */
  settlDate: number;
  /** the far leg's settlement date, ms since the epoch; after the near one */
  farSettlDate: number;
  orderQty: number;
  account?: string;
  // Required by the server, which refuses a swap without them (#166)
  currency: Currency;
  capacity: Capacity;
  tif?: TimeInForce;
  /** a GTD swap's expiry, ms since the epoch and ahead of it (#240); no other swap has one */
  expireTime?: number;
}

export interface CancelOrderRequest {
  orderId: number;
  clOrderId?: string;
}

export interface ReplaceOrderRequest {
  orderId: number;
  clOrderId?: string;
  price?: number;
  /** A held STOP or STOPLIMIT order's new stop (#238) */
  stopPx?: number;
  orderQty?: number;
  tif?: TimeInForce;
  /** A GTD order's new expiry, in milliseconds since the epoch (#240) */
  expireTime?: number;
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

/** System metrics from C++ MetricsPublisher */
export interface SystemMetrics {
  eventsCreated: number;
  eventsProcessed: number;
  eventsFinished: number;
  transactionsCreated: number;
  transactionsProcessed: number;
  transactionsFinished: number;
  availableEventProcessors: number;
  totalEventProcessors: number;
  availableTransactProcessors: number;
  totalTransactProcessors: number;
  queueDepth: number;
  poolSize: number;
  poolCacheMisses: number;
  poolArenaSize: number;
  activeSessions: number;
  activeOrders: number;
  /** LMDB's map in bytes, which doubles when a write finds it full (#148) */
  storageMapSize: number;
  /** bytes of the map in use: the high-water mark of its pages */
  storageMapUsed: number;
  timestamp: number;
}
