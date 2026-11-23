import type { Order, SystemMetrics } from '../types';
import { Capacity, Currency, OrderStatus, OrderType, Side, TimeInForce } from '../types/enums';

/** A metrics sample with every figure 0 but those given */
export function sample(figures: Partial<SystemMetrics> = {}): SystemMetrics {
  return {
    eventsCreated: 0,
    eventsProcessed: 0,
    eventsFinished: 0,
    transactionsCreated: 0,
    transactionsProcessed: 0,
    transactionsFinished: 0,
    transactionsFailed: 0,
    availableEventProcessors: 0,
    totalEventProcessors: 0,
    availableTransactProcessors: 0,
    totalTransactProcessors: 0,
    queueDepth: 0,
    pendingTransactions: 0,
    poolSize: 0,
    poolCacheMisses: 0,
    poolAllocations: 0,
    poolArenaSize: 0,
    activeSessions: 0,
    storedOrders: 0,
    storageMapSize: 0,
    storageMapUsed: 0,
    serverStart: 0,
    timestamp: 0,
    ...figures,
  };
}

/** A resting limit buy of 100 AAA at 10 on account ACC, with what the case gives */
export function order(fields: Partial<Order> = {}): Order {
  return {
    orderId: 1,
    origOrderId: 0,
    clOrderId: 'cl-1',
    origClOrderId: '',
    symbol: 'AAA',
    side: Side.BUY,
    ordType: OrderType.LIMIT,
    price: 10,
    stopPx: 0,
    stopTriggered: false,
    avgPx: 0,
    orderQty: 100,
    cumQty: 0,
    leavesQty: 100,
    minQty: 0,
    dayOrderQty: 100,
    dayCumQty: 0,
    dayAvgPx: 0,
    dayStart: 0,
    status: OrderStatus.NEW,
    tif: TimeInForce.DAY,
    capacity: Capacity.AGENCY,
    currency: Currency.USD,
    account: 'ACC',
    destination: '',
    source: '',
    creationTime: 0,
    lastUpdateTime: 0,
    expireTime: 0,
    ...fields,
  };
}
