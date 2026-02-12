import { OrderStatus, ExecType, Side } from '../types/enums';

/** Display labels for all 12 order statuses */
export const ORDER_STATUS_LABELS: Record<OrderStatus, string> = {
  [OrderStatus.RECEIVED_NEW]: 'Received',
  [OrderStatus.PENDING_NEW]: 'Pending New',
  [OrderStatus.PENDING_REPLACE]: 'Pending Replace',
  [OrderStatus.NEW]: 'New',
  [OrderStatus.PARTIAL_FILL]: 'Partial Fill',
  [OrderStatus.FILLED]: 'Filled',
  [OrderStatus.EXPIRED]: 'Expired',
  [OrderStatus.DONE_FOR_DAY]: 'Done for Day',
  [OrderStatus.SUSPENDED]: 'Suspended',
  [OrderStatus.REPLACED]: 'Replaced',
  [OrderStatus.CANCELED]: 'Canceled',
  [OrderStatus.REJECTED]: 'Rejected',
};

/** Tailwind text color classes for order statuses */
export const ORDER_STATUS_COLORS: Record<OrderStatus, string> = {
  [OrderStatus.RECEIVED_NEW]: 'text-gray-400',
  [OrderStatus.PENDING_NEW]: 'text-gray-400',
  [OrderStatus.PENDING_REPLACE]: 'text-gray-400',
  [OrderStatus.NEW]: 'text-blue-400',
  [OrderStatus.PARTIAL_FILL]: 'text-amber-400',
  [OrderStatus.FILLED]: 'text-green-400',
  [OrderStatus.EXPIRED]: 'text-gray-500',
  [OrderStatus.DONE_FOR_DAY]: 'text-purple-400',
  [OrderStatus.SUSPENDED]: 'text-yellow-400',
  [OrderStatus.REPLACED]: 'text-cyan-400',
  [OrderStatus.CANCELED]: 'text-gray-500',
  [OrderStatus.REJECTED]: 'text-red-400',
};

/** Tailwind text color classes for execution types */
export const EXEC_TYPE_COLORS: Record<ExecType, string> = {
  [ExecType.NEW]: 'text-blue-400',
  [ExecType.TRADE]: 'text-green-400',
  [ExecType.DFD]: 'text-purple-400',
  [ExecType.CORRECT]: 'text-cyan-400',
  [ExecType.CANCEL]: 'text-orange-400',
  [ExecType.REJECT]: 'text-red-400',
  [ExecType.REPLACE]: 'text-cyan-400',
  [ExecType.EXPIRED]: 'text-gray-500',
  [ExecType.SUSPENDED]: 'text-yellow-400',
  [ExecType.STATUS]: 'text-gray-400',
  [ExecType.RESTATED]: 'text-gray-400',
  [ExecType.PEND_CANCEL]: 'text-orange-300',
  [ExecType.PEND_REPLACE]: 'text-gray-400',
};

export const SIDE_LABELS: Record<Side, string> = {
  [Side.BUY]: 'Buy',
  [Side.SELL]: 'Sell',
  [Side.BUY_MINUS]: 'Buy-',
  [Side.SELL_PLUS]: 'Sell+',
  [Side.SELL_SHORT]: 'Short',
  [Side.CROSS]: 'Cross',
};

export const SIDE_COLORS: Record<Side, string> = {
  [Side.BUY]: 'text-blue-400',
  [Side.SELL]: 'text-orange-400',
  [Side.BUY_MINUS]: 'text-blue-300',
  [Side.SELL_PLUS]: 'text-orange-300',
  [Side.SELL_SHORT]: 'text-red-400',
  [Side.CROSS]: 'text-gray-400',
};

/** Statuses that represent active (working) orders */
export const ACTIVE_STATUSES = new Set<OrderStatus>([
  OrderStatus.RECEIVED_NEW,
  OrderStatus.PENDING_NEW,
  OrderStatus.NEW,
  OrderStatus.PARTIAL_FILL,
  OrderStatus.SUSPENDED,
]);

/** Statuses where cancel/replace is allowed */
export const CANCELABLE_STATUSES = new Set<OrderStatus>([
  OrderStatus.NEW,
  OrderStatus.PARTIAL_FILL,
  OrderStatus.SUSPENDED,
]);
