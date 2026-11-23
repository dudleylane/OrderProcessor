import { StrictMode } from 'react';
import { act, fireEvent, render, renderHook, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { OrderBookPanel } from '../components/OrderBookPanel';
import type { NewOrderRequest } from '../types';
import { Capacity, Currency, ExecType, OrderStatus, OrderType, Side, TimeInForce } from '../types/enums';
import { FakeWebSocket } from '../test/FakeWebSocket';
import { useOmsWebSocket } from './useOmsWebSocket';

// The hook's cases from #521, #522 and #523 (#548), on a stand-in WebSocket: a case opens it, feeds it the server's
// messages, drops it and moves the reconnect timer on.

const TOKEN = 'token-aaaaaaaaaaaaaaaa';
const OTHER_TOKEN = 'token-bbbbbbbbbbbbbbbb';
const RECONNECT_DELAY = 3000; // the hook's

const ORDER: NewOrderRequest = {
  symbol: 'AAA',
  side: Side.BUY,
  ordType: OrderType.LIMIT,
  price: 10,
  orderQty: 10,
  tif: TimeInForce.DAY,
  currency: Currency.USD,
  capacity: Capacity.AGENCY,
};

function renderOms(strict = true) {
  return renderHook(() => useOmsWebSocket(TOKEN), strict ? { wrapper: StrictMode } : {});
}

/** The socket the hook is using */
function live(): FakeWebSocket {
  return FakeWebSocket.latest();
}

function rejectReport(orderId: number, rejectReason: string) {
  return {
    type: 'execution_report',
    data: {
      execId: 0,
      orderId,
      type: ExecType.REJECT,
      orderStatus: OrderStatus.REJECTED,
      market: '',
      transactTime: 0,
      rejectReason,
    },
  };
}

function advance(ms: number) {
  act(() => {
    vi.advanceTimersByTime(ms);
  });
}

beforeEach(() => {
  vi.useFakeTimers();
  FakeWebSocket.reset();
  vi.stubGlobal('WebSocket', FakeWebSocket);
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe('notices (#521)', () => {
  it('makes a notice of each refusal, newest first, but not of a stored order\'s reject, which still reaches the executions', () => {
    // #521's first case had a business_reject too, which nothing in the server raised and master removed (142ceb9)
    const hook = renderOms();
    act(() => live().open());
    act(() => live().receive({ type: 'error', message: 'unknown symbol' }));
    act(() => live().receive({ type: 'cancel_reject', data: { orderId: 5, reason: 'too late' } }));
    act(() => live().receive(rejectReport(0, 'Replace refused: too late, the order is no longer in the book')));
    act(() => live().receive(rejectReport(9, 'Price must be above 0')));

    expect(hook.result.current.notices.map(n => [n.kind, n.text])).toEqual([
      ['reject', 'Replace refused: too late, the order is no longer in the book'],
      ['cancel_reject', 'Order 5: too late'],
      ['error', 'unknown symbol'],
    ]);
    expect(hook.result.current.executions.map(e => e.orderId)).toEqual([9, 0]);
  });

  it('keeps the newest 50 of 55 errors', () => {
    const hook = renderOms();
    act(() => live().open());
    for (let i = 1; i <= 55; i++) {
      act(() => live().receive({ type: 'error', message: `error ${i}` }));
    }
    const notices = hook.result.current.notices;
    expect(notices).toHaveLength(50);
    expect(notices[0].text).toBe('error 55');
    expect(notices[49].text).toBe('error 6');
  });

  it('dismisses one notice, leaving the others, and then all of them', () => {
    const hook = renderOms();
    act(() => live().open());
    for (const message of ['first', 'second', 'third']) {
      act(() => live().receive({ type: 'error', message }));
    }
    act(() => hook.result.current.dismissNotice(hook.result.current.notices[1].id));
    expect(hook.result.current.notices.map(n => n.text)).toEqual(['third', 'first']);
    act(() => hook.result.current.dismissNotices());
    expect(hook.result.current.notices).toEqual([]);
  });

  it('refuses a new order, a cancel and a replace while the socket is still connecting, with a notice naming each', () => {
    const hook = renderOms();
    const taken: boolean[] = [];
    act(() => {
      taken.push(hook.result.current.sendOrder(ORDER));
      taken.push(hook.result.current.cancelOrder(7, 'cl-7'));
      taken.push(hook.result.current.replaceOrder({ orderId: 8, price: 11 }));
    });
    expect(taken).toEqual([false, false, false]);
    expect(hook.result.current.notices.map(n => [n.kind, n.text])).toEqual([
      ['not_sent', 'Replace of order 8 not sent: not connected'],
      ['not_sent', 'Cancel of order 7 not sent: not connected'],
      ['not_sent', 'New order (BUY 10 AAA) not sent: not connected'],
    ]);
    expect(live().sent).toEqual([]);
  });

  it('sends a cancel on an open socket, says it was taken, and makes no notice', () => {
    const hook = renderOms();
    act(() => live().open());
    let taken = false;
    act(() => {
      taken = hook.result.current.cancelOrder(7, 'cl-7');
    });
    expect(taken).toBe(true);
    expect(live().messages()).toEqual([{ type: 'cancel_order', data: { orderId: 7, clOrderId: 'cl-7' } }]);
    expect(hook.result.current.notices).toEqual([]);
  });
});

describe('book subscriptions (#522)', () => {
  const subscribe = (symbol: string) => ({ type: 'subscribe_book', symbol });
  const unsubscribe = (symbol: string) => ({ type: 'unsubscribe_book', symbol });

  /** The connection ends, and the hook's reconnect opens the next socket */
  function reconnect() {
    act(() => live().drop());
    advance(RECONNECT_DELAY);
    act(() => live().open());
  }

  it('subscribes a symbol again on the socket a reconnect opens', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => hook.result.current.subscribeBook('AAA'));
    expect(live().messages()).toEqual([subscribe('AAA')]);
    const first = live();
    reconnect();
    expect(live()).not.toBe(first);
    expect(live().messages()).toEqual([subscribe('AAA')]);
  });

  it('sends a subscribe made while the socket is still connecting once it opens', () => {
    const hook = renderOms();
    act(() => hook.result.current.subscribeBook('AAA'));
    act(() => live().open());
    expect(live().messages()).toEqual([subscribe('AAA')]);
  });

  it('sends a subscribe and an unsubscribe on an open socket once each', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => hook.result.current.subscribeBook('AAA'));
    act(() => hook.result.current.unsubscribeBook('AAA'));
    expect(live().messages()).toEqual([subscribe('AAA'), unsubscribe('AAA')]);
  });

  it('keeps a symbol given up during an outage, and its unsubscribe, from the new session', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => hook.result.current.subscribeBook('AAA'));
    act(() => live().drop());
    act(() => hook.result.current.unsubscribeBook('AAA'));
    advance(RECONNECT_DELAY);
    act(() => live().open());
    expect(live().messages()).toEqual([]);
  });

  it('subscribes the book panel\'s symbol again on a reconnect, and shows the new session\'s snapshot', () => {
    function Panel() {
      const oms = useOmsWebSocket(TOKEN);
      return (
        <OrderBookPanel
          instruments={[{ id: 1, symbol: 'AAA', securityId: 'AAA', securityIdSource: '8' }]}
          bookData={oms.bookData}
          subscribeBook={oms.subscribeBook}
          unsubscribeBook={oms.unsubscribeBook}
        />
      );
    }
    const book = (price: number) => ({
      type: 'book_update',
      data: { symbol: 'AAA', bids: [{ price, qty: 5, orderCount: 1 }], asks: [], timestamp: Date.now() },
    });
    render(
      <StrictMode>
        <Panel />
      </StrictMode>,
    );
    act(() => live().open());
    fireEvent.click(screen.getByRole('button', { name: 'AAA' }));
    expect(live().messages()).toEqual([subscribe('AAA')]);
    act(() => live().receive(book(10)));
    expect(screen.getByText('10.0000')).toBeTruthy();

    reconnect();
    expect(live().messages()).toEqual([subscribe('AAA')]);
    act(() => live().receive(book(11)));
    expect(screen.getByText('11.0000')).toBeTruthy();
  });

  it('gives up only the symbol unsubscribed of two, and subscribes the other again on a reconnect', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => hook.result.current.subscribeBook('AAA'));
    act(() => hook.result.current.subscribeBook('BBB'));
    act(() => hook.result.current.unsubscribeBook('AAA'));
    reconnect();
    expect(live().messages()).toEqual([subscribe('BBB')]);
  });
});

describe('the socket\'s cleanup (#523)', () => {
  it('closes its socket at unmount, and nothing reconnects', () => {
    const hook = renderOms(false);
    act(() => live().open());
    const socket = live();
    hook.unmount();
    expect([FakeWebSocket.CLOSING, FakeWebSocket.CLOSED]).toContain(socket.readyState);
    advance(RECONNECT_DELAY * 2);
    expect(FakeWebSocket.instances).toHaveLength(1);
  });

  it('shows disconnected when the token changes, until the new token\'s socket opens, which alone stays', () => {
    const hook = renderHook(({ token }: { token: string }) => useOmsWebSocket(token), {
      initialProps: { token: TOKEN },
    });
    act(() => live().open());
    expect(hook.result.current.connected).toBe(true);

    hook.rerender({ token: OTHER_TOKEN });
    expect(hook.result.current.connected).toBe(false);
    expect(live().protocols).toContain(`oms-token.${OTHER_TOKEN}`);
    advance(0); // the old socket's close event
    expect(hook.result.current.connected).toBe(false);
    act(() => live().open());
    expect(hook.result.current.connected).toBe(true);
    advance(RECONNECT_DELAY * 2);
    expect(FakeWebSocket.instances).toHaveLength(2);
    expect(hook.result.current.connected).toBe(true);
  });

  it('keeps one socket live under StrictMode, where a message makes one notice', () => {
    const hook = renderOms();
    expect(FakeWebSocket.instances).toHaveLength(2); // StrictMode mounts the effect twice
    advance(0); // the first mount's socket closes
    advance(RECONNECT_DELAY * 2);
    expect(FakeWebSocket.instances).toHaveLength(2);
    act(() => live().open());
    act(() => live().receive({ type: 'error', message: 'unknown symbol' }));
    expect(hook.result.current.notices).toHaveLength(1);
  });

  it('stays disconnected under StrictMode if the first mount\'s socket opens after all, and takes none of its messages', () => {
    const hook = renderOms();
    const first = FakeWebSocket.instances[0];
    act(() => first.open());
    act(() => first.receive({ type: 'error', message: 'unknown symbol' }));
    expect(hook.result.current.connected).toBe(false);
    expect(hook.result.current.notices).toEqual([]);
  });

  it('reconnects a socket that closed by itself 3 s later, with the same token', () => {
    renderOms(false);
    act(() => live().open());
    act(() => live().drop());
    advance(RECONNECT_DELAY - 1);
    expect(FakeWebSocket.instances).toHaveLength(1);
    advance(1);
    expect(FakeWebSocket.instances).toHaveLength(2);
    expect(live().protocols).toContain(`oms-token.${TOKEN}`);
  });
});
