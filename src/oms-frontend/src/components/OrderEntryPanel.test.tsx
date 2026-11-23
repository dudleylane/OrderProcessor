import { fireEvent, render, screen, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { NewOrderRequest, NewSwapOrderRequest } from '../types';
import { OrderEntryPanel } from './OrderEntryPanel';

// The order entry panel's cases from #521 and #601 (#548), with a stubbed send

const INSTRUMENTS = [{ id: 1, symbol: 'AAA', securityId: 'AAA', securityIdSource: '8' }];

/** The panel with stubbed sends, the order's and, on FX, the swap's, each answering taken */
function renderPanels(taken: boolean) {
  const sendOrder = vi.fn<(req: NewOrderRequest) => boolean>(() => taken);
  const sendSwapOrder = vi.fn<(req: NewSwapOrderRequest) => boolean>(() => taken);
  render(
    <OrderEntryPanel
      instruments={INSTRUMENTS}
      accounts={[]}
      sendOrder={sendOrder}
      sendSwapOrder={sendSwapOrder}
      connected={true}
    />,
  );
  return { sendOrder, sendSwapOrder };
}

function renderPanel(taken: boolean) {
  return renderPanels(taken).sendOrder;
}

/** A limit buy of 10 AAA at 10, submitted */
function submitOrder() {
  fireEvent.change(screen.getAllByRole('combobox')[0], { target: { value: 'AAA' } });
  fireEvent.change(screen.getByPlaceholderText('0.0000'), { target: { value: '10' } });
  fireEvent.change(screen.getByPlaceholderText('0'), { target: { value: '10' } });
  fireEvent.click(screen.getByRole('button', { name: 'Submit BUY Order' }));
}

/** FX: a buy swap of 100 AAA, the near leg at 1.1 settling 2026-12-01 and the far leg at 1.2 settling 2027-01-04,
 * submitted */
function submitSwap() {
  fireEvent.change(screen.getAllByRole('combobox')[0], { target: { value: 'AAA' } });
  fireEvent.click(screen.getByRole('button', { name: 'FXSWAP' }));
  const [near, far] = screen.getAllByPlaceholderText('0.0000');
  fireEvent.change(near, { target: { value: '1.1' } });
  fireEvent.change(far, { target: { value: '1.2' } });
  const [nearDate, farDate] = Array.from(document.querySelectorAll('input[type="date"]'));
  fireEvent.change(nearDate, { target: { value: '2026-12-01' } });
  fireEvent.change(farDate, { target: { value: '2027-01-04' } });
  fireEvent.change(screen.getByPlaceholderText('0'), { target: { value: '100' } });
  fireEvent.click(screen.getByRole('button', { name: 'Submit BUY Order' }));
}

describe('OrderEntryPanel', () => {
  it('says "Sent", not "Order submitted", when the socket took its order (#521)', () => {
    const sendOrder = renderPanel(true);
    submitOrder();
    expect(sendOrder).toHaveBeenCalledTimes(1);
    expect(screen.getByText('Sent')).toBeTruthy();
    expect(screen.queryByText('Order submitted')).toBeNull();
  });

  it('says "Not sent: not connected" when the socket didn\'t take its order (#521)', () => {
    renderPanel(false);
    submitOrder();
    expect(screen.getByText('Not sent: not connected')).toBeTruthy();
    expect(screen.queryByText('Sent')).toBeNull();
  });

  it('says "Sent" for a swap the socket took, styled as a success (FX, #521)', () => {
    const { sendOrder, sendSwapOrder } = renderPanels(true);
    submitSwap();
    expect(sendOrder).not.toHaveBeenCalled();
    expect(sendSwapOrder).toHaveBeenCalledTimes(1);
    const swap = sendSwapOrder.mock.calls[0][0];
    expect([swap.symbol, swap.orderQty, swap.nearPrice, swap.farPrice]).toEqual(['AAA', 100, 1.1, 1.2]);
    expect(screen.getByText('Sent').className).toContain('text-green-400');
  });

  it('says "Not sent: not connected" for a swap the socket didn\'t take, styled as an error (FX, #521)', () => {
    renderPanels(false);
    submitSwap();
    expect(screen.getByText('Not sent: not connected').className).toContain('text-red-400');
    expect(screen.queryByText('Sent')).toBeNull();
  });

  it('offers DAY, GTD, GTC, FOK and IOC, in that order, the times in force the server takes (#601)', () => {
    renderPanel(true);
    const tif = screen.getAllByRole('combobox').find(select => within(select).queryByRole('option', { name: 'DAY' }));
    expect(tif).toBeDefined();
    const offered = within(tif as HTMLElement)
      .getAllByRole('option')
      .map(option => option.textContent);
    expect(offered).toEqual(['DAY', 'GTD', 'GTC', 'FOK', 'IOC']);
  });
});
