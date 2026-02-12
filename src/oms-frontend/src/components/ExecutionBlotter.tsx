import React from 'react';
import type { Execution, Order, TradeExecution, RejectExecution, CorrectExecution, TradeCancelExecution, ReplaceExecution } from '../types';
import { ExecType } from '../types/enums';
import { formatQuantity, formatPrice, formatTime, formatId } from '../utils/format';
import { EXEC_TYPE_COLORS, ORDER_STATUS_LABELS, ORDER_STATUS_COLORS } from '../utils/constants';
import { downloadCsv } from '../utils/csv';

interface ExecutionBlotterProps {
  executions: Execution[];
  orders: Map<string, Order>;
}

function execDetails(exec: Execution): string {
  switch (exec.type) {
    case ExecType.TRADE: {
      const t = exec as TradeExecution;
      return `${formatQuantity(t.lastQty)} @ ${formatPrice(t.lastPx)}`;
    }
    case ExecType.REJECT: {
      const r = exec as RejectExecution;
      return r.rejectReason || 'Rejected';
    }
    case ExecType.CORRECT: {
      const c = exec as CorrectExecution;
      return `Ref: ${formatId(c.execRefId)} | ${formatQuantity(c.lastQty)} @ ${formatPrice(c.lastPx)}`;
    }
    case ExecType.CANCEL: {
      const tc = exec as TradeCancelExecution;
      return tc.execRefId ? `Ref: ${formatId(tc.execRefId)}` : '';
    }
    case ExecType.REPLACE: {
      const rp = exec as ReplaceExecution;
      return rp.origOrderId ? `Orig: ${formatId(rp.origOrderId)}` : '';
    }
    default:
      return '';
  }
}

function handleExport(executions: Execution[], orders: Map<string, Order>) {
  const headers = [
    'Time', 'ExecID', 'Type', 'Symbol', 'OrderID', 'Side', 'LastQty',
    'LastPx', 'OrdStatus', 'Market', 'Details',
  ];

  const rows = executions.map(exec => {
    const order = orders.get(String(exec.orderId));
    const trade = exec.type === ExecType.TRADE ? exec as TradeExecution : null;
    return [
      new Date(exec.transactTime).toISOString(),
      exec.execId,
      exec.type,
      order?.symbol ?? '',
      exec.orderId,
      order?.side ?? '',
      trade?.lastQty ?? '',
      trade?.lastPx ?? '',
      exec.orderStatus,
      exec.market,
      execDetails(exec),
    ];
  });

  downloadCsv(
    `executions-${new Date().toISOString().slice(0, 10)}.csv`,
    headers,
    rows as (string | number | undefined)[][],
  );
}

export function ExecutionBlotter({ executions, orders }: ExecutionBlotterProps) {
  if (executions.length === 0) {
    return (
      <div className="flex items-center justify-center h-64 text-gray-600 text-sm">
        No executions
      </div>
    );
  }

  return (
    <div className="overflow-x-auto">
      <table className="w-full text-xs">
        <thead>
          <tr className="text-gray-500 border-b border-gray-800">
            <th className="text-left p-3">Time</th>
            <th className="text-left p-3">ExecID</th>
            <th className="text-left p-3">Type</th>
            <th className="text-left p-3">Symbol</th>
            <th className="text-left p-3">OrderID</th>
            <th className="text-left p-3">Side</th>
            <th className="text-right p-3">LastQty</th>
            <th className="text-right p-3">LastPx</th>
            <th className="text-left p-3">OrdStatus</th>
            <th className="text-left p-3">Market</th>
            <th className="text-left p-3">Details</th>
          </tr>
        </thead>
        <tbody>
          {executions.map(exec => {
            const order = orders.get(String(exec.orderId));
            const trade = exec.type === ExecType.TRADE ? exec as TradeExecution : null;

            return (
              <tr
                key={exec.execId}
                className="border-b border-gray-800/50 hover:bg-gray-900/50"
              >
                <td className="p-3 text-gray-500 font-mono">
                  {formatTime(exec.transactTime)}
                </td>
                <td className="p-3 font-mono text-gray-400" title={String(exec.execId)}>
                  {formatId(exec.execId)}
                </td>
                <td className={`p-3 font-medium ${EXEC_TYPE_COLORS[exec.type] ?? 'text-gray-400'}`}>
                  {exec.type.replace(/_/g, ' ')}
                </td>
                <td className="p-3 font-mono font-medium">
                  {order?.symbol ?? '--'}
                </td>
                <td className="p-3 font-mono text-gray-400" title={String(exec.orderId)}>
                  {formatId(exec.orderId)}
                </td>
                <td className="p-3 text-gray-400">
                  {order?.side ?? '--'}
                </td>
                <td className="p-3 text-right font-mono">
                  {trade ? formatQuantity(trade.lastQty) : '--'}
                </td>
                <td className="p-3 text-right font-mono text-green-400">
                  {trade ? formatPrice(trade.lastPx) : '--'}
                </td>
                <td className={`p-3 font-medium ${ORDER_STATUS_COLORS[exec.orderStatus] ?? 'text-gray-400'}`}>
                  {ORDER_STATUS_LABELS[exec.orderStatus] ?? exec.orderStatus}
                </td>
                <td className="p-3 text-gray-500">
                  {exec.market}
                </td>
                <td className="p-3 text-gray-500 max-w-[200px] truncate" title={execDetails(exec)}>
                  {execDetails(exec)}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>

      {/* Export button with proper CSV escaping */}
      <div className="p-3 border-t border-gray-800 flex justify-end">
        <button
          onClick={() => handleExport(executions, orders)}
          className="text-xs text-gray-500 hover:text-gray-300 px-3 py-1.5 border border-gray-700 rounded"
        >
          Export CSV
        </button>
      </div>
    </div>
  );
}
