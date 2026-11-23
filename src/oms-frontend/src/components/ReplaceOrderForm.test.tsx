import { fireEvent, render, screen, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { Order, ReplaceOrderRequest } from '../types';
import { OrderStatus, TimeInForce } from '../types/enums';
import { order } from '../test/fixtures';
import { ReplaceOrderForm } from './ReplaceOrderForm';

// The replace form's cases from #521 and #601 (#548), with a stubbed send

function renderForm(resting: Order, taken = true) {
  const replaceOrder = vi.fn<(req: ReplaceOrderRequest) => boolean>(() => taken);
  const onClose = vi.fn();
  render(<ReplaceOrderForm order={resting} replaceOrder={replaceOrder} onClose={onClose} />);
  return { replaceOrder, onClose };
}

/** The form's inputs in their order: the price, then the quantity, for a limit order */
function setPrice(value: string) {
  fireEvent.change(screen.getAllByRole('textbox')[0], { target: { value } });
}

function offered(): (string | null)[] {
  return within(screen.getByRole('combobox'))
    .getAllByRole('option')
    .map(option => option.textContent);
}

function sendButton(): HTMLButtonElement {
  return screen.getByRole('button', { name: 'Send replace' }) as HTMLButtonElement;
}

describe('ReplaceOrderForm', () => {
  it('stays open, saying "Not sent: not connected", when the socket didn\'t take its replace (#521)', () => {
    const { replaceOrder, onClose } = renderForm(order({ orderId: 7, clOrderId: 'cl-7' }), false);
    setPrice('11');
    fireEvent.click(sendButton());
    expect(replaceOrder).toHaveBeenCalledTimes(1);
    expect(onClose).not.toHaveBeenCalled();
    expect(screen.getByText('Not sent: not connected')).toBeTruthy();
  });

  it('closes once its replace was sent (#521)', () => {
    const { replaceOrder, onClose } = renderForm(order({ orderId: 7, clOrderId: 'cl-7' }), true);
    setPrice('11');
    fireEvent.click(sendButton());
    expect(replaceOrder).toHaveBeenCalledWith({ orderId: 7, clOrderId: 'cl-7', price: 11 });
    expect(onClose).toHaveBeenCalledTimes(1);
  });

  it.each([40, 30])(
    'sends a quantity of %i, at or below the 40 the order has filled, saying that the order ends (#544)',
    qty => {
      const partly = order({ orderId: 7, clOrderId: 'cl-7', cumQty: 40, leavesQty: 60, status: OrderStatus.PARTIAL_FILL });
      const { replaceOrder, onClose } = renderForm(partly);
      fireEvent.change(screen.getAllByRole('textbox')[1], { target: { value: String(qty) } });
      expect(screen.getByText('At or below the 40 already filled: the order ends, filled at 40')).toBeTruthy();
      expect(sendButton().disabled).toBe(false);
      fireEvent.click(sendButton());
      expect(replaceOrder).toHaveBeenCalledWith({ orderId: 7, clOrderId: 'cl-7', orderQty: qty });
      expect(onClose).toHaveBeenCalledTimes(1);
    },
  );

  it('says nothing of the order ending for a quantity above what it has filled (#544)', () => {
    renderForm(order({ cumQty: 40, leavesQty: 60, status: OrderStatus.PARTIAL_FILL }));
    fireEvent.change(screen.getAllByRole('textbox')[1], { target: { value: '41' } });
    expect(screen.queryByText(/the order ends/)).toBeNull();
    expect(sendButton().disabled).toBe(false);
  });

  it('offers the five times in force the server takes for a GTC order (#601)', () => {
    renderForm(order({ tif: TimeInForce.GTC }));
    expect(offered()).toEqual(['DAY', 'GTD', 'GTC', 'FOK', 'IOC']);
  });

  it.each([TimeInForce.OPG, TimeInForce.ATCLOSE])(
    'offers a %s order its own value first, and refuses to send a new price alone, saying why (#601)',
    tif => {
      const { replaceOrder } = renderForm(order({ tif }));
      expect(offered()).toEqual([tif, 'DAY', 'GTD', 'GTC', 'FOK', 'IOC']);
      setPrice('11');
      expect(screen.getByText(`The server no longer takes ${tif} orders: choose another time in force`)).toBeTruthy();
      expect(sendButton().disabled).toBe(true);
      fireEvent.click(sendButton());
      expect(replaceOrder).not.toHaveBeenCalled();
    },
  );

  it('clears that line once an OPG order is given GTC, and sends the change of time in force (#601)', () => {
    const { replaceOrder } = renderForm(order({ orderId: 7, clOrderId: 'cl-7', tif: TimeInForce.OPG }));
    fireEvent.change(screen.getByRole('combobox'), { target: { value: TimeInForce.GTC } });
    expect(screen.queryByText(/no longer takes/)).toBeNull();
    fireEvent.click(sendButton());
    expect(replaceOrder).toHaveBeenCalledWith({ orderId: 7, clOrderId: 'cl-7', tif: 'GTC' });
  });
});
