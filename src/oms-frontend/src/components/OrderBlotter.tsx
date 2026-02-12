import React from 'react';
import type { Order } from '../types';
import { formatQuantity, formatPrice, formatTime, formatId } from '../utils/format';
import {
  ORDER_STATUS_LABELS,
  ORDER_STATUS_COLORS,
  SIDE_LABELS,
  SIDE_COLORS,
  ACTIVE_STATUSES,
  CANCELABLE_STATUSES,
} from '../utils/constants';

interface OrderBlotterProps {
  orders: Map<string, Order>;
  cancelOrder: (orderId: number) => void;
  replaceOrder: (req: { orderId: number }) => void;
}

export function OrderBlotter({ orders, cancelOrder, replaceOrder }: OrderBlotterProps) {
  const sortedOrders = Array.from(orders.values())
    .sort((a, b) => b.lastUpdateTime - a.lastUpdateTime);

  if (sortedOrders.length === 0) {
    return (
      <div className="flex items-center justify-center h-64 text-gray-600 text-sm">
        No orders
      </div>
    );
  }

  return (
    <div className="overflow-x-auto">
      <table className="w-full text-xs">
        <thead>
          <tr className="text-gray-500 border-b border-gray-800">
            <th className="text-left p-3">Time</th>
            <th className="text-left p-3">ID</th>
            <th className="text-left p-3">Symbol</th>
            <th className="text-left p-3">Side</th>
            <th className="text-left p-3">Type</th>
            <th className="text-right p-3">Price</th>
            <th className="text-right p-3">Qty</th>
            <th className="text-right p-3">CumQty</th>
            <th className="text-right p-3">Leaves</th>
            <th className="text-right p-3">AvgPx</th>
            <th className="text-left p-3">Status</th>
            <th className="text-left p-3">TIF</th>
            <th className="text-center p-3">Actions</th>
          </tr>
        </thead>
        <tbody>
          {sortedOrders.map(order => {
            const isActive = ACTIVE_STATUSES.has(order.status);
            const canCancel = CANCELABLE_STATUSES.has(order.status);

            return (
              <tr
                key={order.orderId}
                className={`border-b border-gray-800/50 hover:bg-gray-900/50
                  ${!isActive ? 'opacity-60' : ''}`}
              >
                <td className="p-3 text-gray-500 font-mono">
                  {formatTime(order.creationTime)}
                </td>
                <td className="p-3 font-mono text-gray-400" title={String(order.orderId)}>
                  {formatId(order.orderId)}
                </td>
                <td className="p-3 font-mono font-medium">
                  {order.symbol}
                </td>
                <td className={`p-3 font-medium ${SIDE_COLORS[order.side] ?? 'text-gray-400'}`}>
                  {SIDE_LABELS[order.side] ?? order.side}
                </td>
                <td className="p-3 text-gray-400">
                  {order.ordType}
                </td>
                <td className="p-3 text-right font-mono">
                  {order.price > 0 ? formatPrice(order.price) : '--'}
                </td>
                <td className="p-3 text-right font-mono">
                  {formatQuantity(order.orderQty)}
                </td>
                <td className="p-3 text-right font-mono text-green-400">
                  {order.cumQty > 0 ? formatQuantity(order.cumQty) : '--'}
                </td>
                <td className="p-3 text-right font-mono">
                  {formatQuantity(order.leavesQty)}
                </td>
                <td className="p-3 text-right font-mono">
                  {order.avgPx > 0 ? formatPrice(order.avgPx) : '--'}
                </td>
                <td className={`p-3 font-medium ${ORDER_STATUS_COLORS[order.status] ?? 'text-gray-400'}`}>
                  {/* Fix: use replaceAll instead of replace for multi-underscore statuses */}
                  {ORDER_STATUS_LABELS[order.status] ?? order.status.replace(/_/g, ' ')}
                </td>
                <td className="p-3 text-gray-500">{order.tif}</td>
                <td className="p-3 text-center">
                  {canCancel && (
                    <span className="flex gap-2 justify-center">
                      {/* Fix: onClick wired to cancelOrder via WS hook */}
                      <button
                        onClick={() => cancelOrder(order.orderId)}
                        className="text-red-400 hover:text-red-300 text-xs"
                      >
                        Cancel
                      </button>
                      <button
                        onClick={() => replaceOrder({ orderId: order.orderId })}
                        className="text-cyan-400 hover:text-cyan-300 text-xs"
                      >
                        Replace
                      </button>
                    </span>
                  )}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
