import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { Instrument, OrderBookSnapshot } from '../types';
import { OrderBookPanel } from './OrderBookPanel';

// The book panel's cases from #686, with stubbed subscriptions

const AAA: Instrument = { id: 1, symbol: 'AAA', securityId: 'AAA', securityIdSource: '8' };
const BBB: Instrument = { id: 2, symbol: 'BBB', securityId: 'BBB', securityIdSource: '8' };

function renderPanel(instruments: Instrument[]) {
  const subscribeBook = vi.fn<(symbol: string) => void>();
  const unsubscribeBook = vi.fn<(symbol: string) => void>();
  const bookData = new Map<string, OrderBookSnapshot>();
  const props = { bookData, subscribeBook, unsubscribeBook };
  const panel = render(<OrderBookPanel instruments={instruments} {...props} />);
  const rerender = (next: Instrument[]) => panel.rerender(<OrderBookPanel instruments={next} {...props} />);
  return { subscribeBook, unsubscribeBook, rerender };
}

describe('OrderBookPanel (#686)', () => {
  it('drops, and unsubscribes, an instrument a list sent again lacks', () => {
    const { subscribeBook, unsubscribeBook, rerender } = renderPanel([AAA]);
    fireEvent.click(screen.getByRole('button', { name: 'AAA' }));
    expect(subscribeBook).toHaveBeenCalledWith('AAA');
    rerender([BBB]);
    expect(unsubscribeBook).toHaveBeenCalledWith('AAA');
    expect(screen.getByText('Select an instrument')).toBeTruthy();
  });

  it('keeps an instrument a list sent again still has', () => {
    const { subscribeBook, unsubscribeBook, rerender } = renderPanel([AAA]);
    fireEvent.click(screen.getByRole('button', { name: 'AAA' }));
    rerender([AAA, BBB]);
    expect(subscribeBook).toHaveBeenCalledTimes(1);
    expect(unsubscribeBook).not.toHaveBeenCalled();
    expect(screen.getByText('Waiting for data...')).toBeTruthy();
  });
});
