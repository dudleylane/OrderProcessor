import React, { useMemo } from 'react';
import type { Execution, Order, TradeExecution, CorrectExecution, TradeCancelExecution } from '../types';
import { ExecType, Side } from '../types/enums';
import { formatQuantity, formatPrice, formatTime } from '../utils/format';

interface PositionSummaryProps {
  executions: Execution[];
  orders: Map<string, Order>;
}

interface Position {
  symbol: string;
  netQty: number;
  avgPx: number;
  tradeCount: number;
  lastTradeTime: number;
  totalCost: number;
  totalQty: number;
}

export function PositionSummary({ executions, orders }: PositionSummaryProps) {
  const positions = useMemo(() => {
    const posMap = new Map<string, Position>();

    for (const exec of executions) {
      const order = orders.get(String(exec.orderId));
      if (!order) continue;
      const symbol = order.symbol;

      const existing = posMap.get(symbol) ?? {
        symbol,
        netQty: 0,
        avgPx: 0,
        tradeCount: 0,
        lastTradeTime: 0,
        totalCost: 0,
        totalQty: 0,
      };

      if (exec.type === ExecType.TRADE) {
        const trade = exec as TradeExecution;
        // Fix: use order side instead of hardcoded "BANK_DEMO"
        const direction =
          order.side === Side.BUY || order.side === Side.BUY_MINUS ? 1 : -1;
        const signedQty = trade.lastQty * direction;

        existing.netQty += signedQty;
        existing.tradeCount += 1;
        existing.totalCost += trade.lastPx * trade.lastQty;
        existing.totalQty += trade.lastQty;
        existing.lastTradeTime = Math.max(existing.lastTradeTime, trade.transactTime);
      } else if (exec.type === ExecType.CORRECT) {
        // Correct adjusts position
        const corr = exec as CorrectExecution;
        const direction =
          order.side === Side.BUY || order.side === Side.BUY_MINUS ? 1 : -1;
        const signedQty = corr.lastQty * direction;

        existing.netQty += signedQty;
        existing.totalCost += corr.lastPx * corr.lastQty;
        existing.totalQty += corr.lastQty;
      } else if (exec.type === ExecType.CANCEL) {
        // Trade cancel reverses the original trade
        const cancel = exec as TradeCancelExecution;
        // We don't have the original trade qty on the cancel exec itself,
        // so this is a placeholder -- real impl would look up the ref exec
        void cancel;
      }

      // VWAP avg price
      if (existing.totalQty > 0) {
        existing.avgPx = existing.totalCost / existing.totalQty;
      }

      posMap.set(symbol, existing);
    }

    return Array.from(posMap.values()).sort((a, b) =>
      a.symbol.localeCompare(b.symbol),
    );
  }, [executions, orders]);

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
            <th className="text-left p-3">Symbol</th>
            <th className="text-right p-3">Net Qty</th>
            <th className="text-left p-3">Direction</th>
            <th className="text-right p-3">Avg Px</th>
            <th className="text-right p-3">Trades</th>
            <th className="text-right p-3">Last Trade</th>
          </tr>
        </thead>
        <tbody>
          {positions.map(pos => {
            const isLong = pos.netQty > 0;
            const isFlat = pos.netQty === 0;

            return (
              <tr
                key={pos.symbol}
                className="border-b border-gray-800/50 hover:bg-gray-900/50"
              >
                <td className="p-3 font-mono font-medium">
                  {pos.symbol}
                </td>
                <td className={`p-3 text-right font-mono font-medium ${
                  isFlat ? 'text-gray-500' :
                  isLong ? 'text-blue-400' : 'text-orange-400'
                }`}>
                  {formatQuantity(pos.netQty)}
                </td>
                <td className={`p-3 font-medium ${
                  isFlat ? 'text-gray-500' :
                  isLong ? 'text-blue-400' : 'text-orange-400'
                }`}>
                  {isFlat ? 'FLAT' : isLong ? 'LONG' : 'SHORT'}
                </td>
                <td className="p-3 text-right font-mono">
                  {pos.avgPx > 0 ? formatPrice(pos.avgPx) : '--'}
                </td>
                <td className="p-3 text-right text-gray-400">
                  {pos.tradeCount}
                </td>
                <td className="p-3 text-right text-gray-500 font-mono">
                  {pos.lastTradeTime > 0 ? formatTime(pos.lastTradeTime) : '--'}
                </td>
              </tr>
            );
          })}
        </tbody>

        {/* Totals row */}
        <tfoot>
          <tr className="border-t border-gray-700 text-gray-400">
            <td className="p-3 font-medium">Total</td>
            <td className="p-3 text-right font-mono font-medium">
              {formatQuantity(positions.reduce((s, p) => s + Math.abs(p.netQty), 0))}
              <span className="text-gray-600 ml-1">gross</span>
            </td>
            <td className="p-3" />
            <td className="p-3" />
            <td className="p-3 text-right font-mono">
              {positions.reduce((s, p) => s + p.tradeCount, 0)}
            </td>
            <td className="p-3" />
          </tr>
        </tfoot>
      </table>
    </div>
  );
}
