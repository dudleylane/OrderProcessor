import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import type { Order } from '../types';
import { OrderStatus, Side } from '../types/enums';
import { order } from '../test/fixtures';
import { PositionSummary } from './PositionSummary';

// #524's cases (#548): the panel rendered with the orders as the server sends them, each body row read by its column's
// header

function renderPositions(orders: Order[]) {
  render(<PositionSummary orders={new Map(orders.map(o => [String(o.orderId), o]))} />);
}

/** Each body row, as its columns' headers to its cells' text */
function rows(): Record<string, string>[] {
  const table = screen.queryByRole('table');
  if (!table) {
    return [];
  }
  const headers = Array.from(table.querySelectorAll('thead th')).map(th => th.textContent ?? '');
  return Array.from(table.querySelectorAll('tbody tr')).map(tr =>
    Object.fromEntries(Array.from(tr.querySelectorAll('td')).map((td, i) => [headers[i], td.textContent ?? ''])),
  );
}

/** A filled order */
function filled(fields: Partial<Order>): Order {
  return order({ status: OrderStatus.FILLED, leavesQty: 0, ...fields });
}

describe('PositionSummary (#524)', () => {
  it('gives each account its own row for a trade between two of them', () => {
    renderPositions([
      filled({ orderId: 1, account: 'A', side: Side.BUY, orderQty: 100, cumQty: 100, avgPx: 10 }),
      filled({ orderId: 2, account: 'B', side: Side.SELL, orderQty: 100, cumQty: 100, avgPx: 10 }),
    ]);
    expect(rows().map(r => [r['Account'], r['Net Qty'], r['Direction']])).toEqual([
      ['A', '100', 'LONG'],
      ['B', '-100', 'SHORT'],
    ]);
  });

  it('gives the positions from the orders alone, as after a reload, with no trade reports', () => {
    renderPositions([filled({ orderId: 1, account: 'A', cumQty: 100, avgPx: 10 })]);
    expect(screen.queryByText('No positions')).toBeNull();
    expect(rows()).toHaveLength(1);
  });

  it('averages the buys and the sells apart', () => {
    renderPositions([
      filled({ orderId: 1, side: Side.BUY, orderQty: 100, cumQty: 100, avgPx: 10 }),
      filled({ orderId: 2, side: Side.BUY, orderQty: 300, cumQty: 300, avgPx: 12 }),
      filled({ orderId: 3, side: Side.SELL, orderQty: 100, cumQty: 100, avgPx: 11 }),
    ]);
    const [row] = rows();
    expect(row['Net Qty']).toBe('300');
    expect(row['Avg Buy']).toBe('11.5000');
    expect(row['Avg Sell']).toBe('11.0000');
  });

  it('counts a replaced order\'s fills once with its replacement, whichever arrives first, and alone before it does', () => {
    const original = filled({ orderId: 1, status: OrderStatus.REPLACED, orderQty: 100, cumQty: 40, avgPx: 10 });
    const replacement = order({ orderId: 2, origOrderId: 1, orderQty: 80, cumQty: 40, leavesQty: 40, avgPx: 10 });
    for (const orders of [[original, replacement], [replacement, original]]) {
      renderPositions(orders);
      expect(rows().map(r => r['Bought'])).toEqual(['40']);
      document.body.innerHTML = '';
    }
    renderPositions([original]);
    expect(rows().map(r => r['Bought'])).toEqual(['40']);
  });

  it('adds no row for an order with no fills, nor for a rejected one', () => {
    renderPositions([
      order({ orderId: 1, cumQty: 0 }),
      order({ orderId: 2, status: OrderStatus.REJECTED, cumQty: 0, leavesQty: 0 }),
    ]);
    expect(screen.getByText('No positions')).toBeTruthy();
  });
});
