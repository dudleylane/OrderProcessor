import React, { useMemo } from 'react';
import type { Order } from '../types';
import { Side } from '../types/enums';
import { formatQuantity, formatPrice } from '../utils/format';

interface PositionSummaryProps {
  orders: Map<string, Order>;
}

interface Position {
  account: string;
  symbol: string;
  boughtQty: number;
  boughtCost: number;
  soldQty: number;
  soldCost: number;
}

// Each account's positions from its orders' own figures, each order's filled quantity at its average price (#524). The
// order snapshot carries them for every order the session may see, where the trade reports are only this session's.
// Corrections and trade cancels change those figures, so they need nothing here.
export function PositionSummary({ orders }: PositionSummaryProps) {
  const positions = useMemo(() => {
    // A replacement takes over its original's fills, so an original counts only until its replacement arrives
    const replaced = new Set<number>();
    for (const order of orders.values()) {
      if (order.origOrderId) replaced.add(order.origOrderId);
    }

    const posMap = new Map<string, Position>();
    for (const order of orders.values()) {
      if (order.cumQty <= 0 || replaced.has(order.orderId)) continue;

      const key = `${order.account}\u0000${order.symbol}`;
      const existing = posMap.get(key) ?? {
        account: order.account,
        symbol: order.symbol,
        boughtQty: 0,
        boughtCost: 0,
        soldQty: 0,
        soldCost: 0,
      };

      // Since #510 only BUY and SELL trade; the other sides are on orders stored before it
      if (order.side === Side.BUY || order.side === Side.BUY_MINUS) {
        existing.boughtQty += order.cumQty;
        existing.boughtCost += order.cumQty * order.avgPx;
      } else {
        existing.soldQty += order.cumQty;
        existing.soldCost += order.cumQty * order.avgPx;
      }

      posMap.set(key, existing);
    }

    return Array.from(posMap.values()).sort(
      (a, b) => a.account.localeCompare(b.account) || a.symbol.localeCompare(b.symbol),
    );
  }, [orders]);

  if (positions.length === 0) {
    return (
      <div className="flex items-center justify-center h-64 text-gray-600 text-sm">
        No positions
      </div>
    );
  }

  return (
    <div className="overflow-x-auto">
      <table className="w-full text-xs">
        <thead>
          <tr className="text-gray-500 border-b border-gray-800">
            <th className="text-left p-3">Account</th>
            <th className="text-left p-3">Symbol</th>
            <th className="text-right p-3">Net Qty</th>
            <th className="text-left p-3">Direction</th>
            <th className="text-right p-3">Bought</th>
            <th className="text-right p-3">Avg Buy</th>
            <th className="text-right p-3">Sold</th>
            <th className="text-right p-3">Avg Sell</th>
          </tr>
        </thead>
        <tbody>
          {positions.map(pos => {
            const netQty = pos.boughtQty - pos.soldQty;
            const isLong = netQty > 0;
            const isFlat = netQty === 0;

            return (
              <tr
                key={`${pos.account}\u0000${pos.symbol}`}
                className="border-b border-gray-800/50 hover:bg-gray-900/50"
              >
                <td className="p-3 text-gray-400">
                  {pos.account}
                </td>
                <td className="p-3 font-mono font-medium">
                  {pos.symbol}
                </td>
                <td className={`p-3 text-right font-mono font-medium ${
                  isFlat ? 'text-gray-500' :
                  isLong ? 'text-blue-400' : 'text-orange-400'
                }`}>
                  {formatQuantity(netQty)}
                </td>
                <td className={`p-3 font-medium ${
                  isFlat ? 'text-gray-500' :
                  isLong ? 'text-blue-400' : 'text-orange-400'
                }`}>
                  {isFlat ? 'FLAT' : isLong ? 'LONG' : 'SHORT'}
                </td>
                <td className="p-3 text-right font-mono">
                  {formatQuantity(pos.boughtQty)}
                </td>
                <td className="p-3 text-right font-mono">
                  {pos.boughtQty > 0 ? formatPrice(pos.boughtCost / pos.boughtQty) : '--'}
                </td>
                <td className="p-3 text-right font-mono">
                  {formatQuantity(pos.soldQty)}
                </td>
                <td className="p-3 text-right font-mono">
                  {pos.soldQty > 0 ? formatPrice(pos.soldCost / pos.soldQty) : '--'}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
