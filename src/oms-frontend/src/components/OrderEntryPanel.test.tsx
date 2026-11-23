import { fireEvent, render, screen, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { NewOrderRequest } from '../types';
import { OrderEntryPanel } from './OrderEntryPanel';

// The order entry panel's cases from #521 and #601 (#548), with a stubbed send

const INSTRUMENTS = [{ id: 1, symbol: 'AAA', securityId: 'AAA', securityIdSource: '8' }];

function renderPanel(taken: boolean) {
  const sendOrder = vi.fn<(req: NewOrderRequest) => boolean>(() => taken);
  render(<OrderEntryPanel instruments={INSTRUMENTS} accounts={[]} sendOrder={sendOrder} connected={true} />);
  return sendOrder;
}

/** A limit buy of 10 AAA at 10, submitted */
function submitOrder() {
  fireEvent.change(screen.getAllByRole('combobox')[0], { target: { value: 'AAA' } });
  fireEvent.change(screen.getByPlaceholderText('0.0000'), { target: { value: '10' } });
  fireEvent.change(screen.getByPlaceholderText('0'), { target: { value: '10' } });
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
