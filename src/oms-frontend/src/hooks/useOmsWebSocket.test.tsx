import { StrictMode } from 'react';
import { act, fireEvent, render, renderHook, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { OrderBookPanel } from '../components/OrderBookPanel';
import type { NewOrderRequest, NewSwapOrderRequest } from '../types';
import { Capacity, Currency, ExecType, OrderStatus, OrderType, Side, TimeInForce } from '../types/enums';
import { FakeWebSocket } from '../test/FakeWebSocket';
import { order } from '../test/fixtures';
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

// FX: a buy swap of 100 AAA
const SWAP: NewSwapOrderRequest = {
  symbol: 'AAA',
  side: Side.BUY,
  nearPrice: 1.1,
  farPrice: 1.2,
  settlDate: Date.UTC(2026, 11, 1),
  farSettlDate: Date.UTC(2027, 0, 4),
  orderQty: 100,
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

  it('refuses a swap while the socket is still connecting, with a notice naming it (FX)', () => {
    const hook = renderOms();
    let taken = true;
    act(() => {
      taken = hook.result.current.sendSwapOrder(SWAP);
    });
    expect(taken).toBe(false);
    expect(hook.result.current.notices.map(n => [n.kind, n.text])).toEqual([
      ['not_sent', 'New swap (BUY 100 AAA) not sent: not connected'],
    ]);
    expect(live().sent).toEqual([]);
  });

  it('sends a swap on an open socket, says it was taken, and makes no notice (FX)', () => {
    const hook = renderOms();
    act(() => live().open());
    let taken = false;
    act(() => {
      taken = hook.result.current.sendSwapOrder(SWAP);
    });
    expect(taken).toBe(true);
    expect(live().messages()).toEqual([
      { type: 'new_swap_order', data: { ...SWAP, requestId: expect.stringMatching(REQUEST_ID) } },
    ]);
    expect(hook.result.current.notices).toEqual([]);
  });

  it('sends a cancel on an open socket, says it was taken, and makes no notice', () => {
    const hook = renderOms();
    act(() => live().open());
    let taken = false;
    act(() => {
      taken = hook.result.current.cancelOrder(7, 'cl-7');
    });
    expect(taken).toBe(true);
    expect(live().messages()).toEqual([
      { type: 'cancel_order', data: { orderId: 7, clOrderId: 'cl-7', requestId: expect.stringMatching(REQUEST_ID) } },
    ]);
    expect(hook.result.current.notices).toEqual([]);
  });
});

/** What the hook's request ids look like (#683): 12 of A-Z a-z 0-9 _ - */
const REQUEST_ID = /^[A-Za-z0-9_-]{12}$/;

/** The requestId of each message the hook sent, in order */
function requestIds(): string[] {
  return live()
    .messages()
    .map(m => (m as { data: { requestId: string } }).data.requestId);
}

describe('request ids (#683)', () => {
  it('gives each request an id of its own, and names the request in the notice of a refusal that echoes it', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      hook.result.current.sendOrder(ORDER);
      hook.result.current.cancelOrder(7);
      hook.result.current.replaceOrder({ orderId: 8, price: 11 });
    });
    const [order, cancel, replace] = requestIds();
    for (const id of [order, cancel, replace]) expect(id).toMatch(REQUEST_ID);
    expect(new Set([order, cancel, replace]).size).toBe(3);

    act(() => live().receive({ type: 'error', message: 'Unknown instrument: AAA', requestId: order }));
    const reject = rejectReport(0, 'Replace refused: too late');
    act(() => live().receive({ ...reject, data: { ...reject.data, requestId: replace } }));
    act(() => live().receive({ type: 'error', message: 'More than one order has number 7', requestId: cancel }));
    expect(hook.result.current.notices.map(n => [n.kind, n.text])).toEqual([
      ['error', 'Cancel of order 7: More than one order has number 7'],
      ['reject', 'Replace of order 8: Replace refused: too late'],
      ['error', 'New order (BUY 10 AAA): Unknown instrument: AAA'],
    ]);
  });

  it('gives a swap an id too, and names it in the notice of a refusal that echoes it (FX)', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      hook.result.current.sendSwapOrder(SWAP);
    });
    const [swap] = requestIds();
    expect(swap).toMatch(REQUEST_ID);
    act(() =>
      live().receive({ type: 'error', message: 'Order refused: farSettlDate must follow settlDate', requestId: swap }),
    );
    expect(hook.result.current.notices.map(n => [n.kind, n.text])).toEqual([
      ['error', 'New swap (BUY 100 AAA): Order refused: farSettlDate must follow settlDate'],
    ]);
  });

  it("names no request for an id it didn't send, nor for one already answered", () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      hook.result.current.sendOrder(ORDER);
    });
    const [order] = requestIds();
    act(() => live().receive({ type: 'error', message: 'first', requestId: order }));
    act(() => live().receive({ type: 'error', message: 'again', requestId: order }));
    act(() => live().receive({ type: 'error', message: "another tab's", requestId: 'zzzzzzzzzzzz' }));
    expect(hook.result.current.notices.map(n => n.text)).toEqual([
      "another tab's",
      'again',
      'New order (BUY 10 AAA): first',
    ]);
  });

  it("names a cancel in the engine's cancel_reject, which names its order, and forgets one the engine accepted", () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      hook.result.current.cancelOrder(7);
      hook.result.current.cancelOrder(9);
    });
    act(() => live().receive({ type: 'cancel_reject', data: { orderId: 7, reason: 'Cancel rejected: too late' } }));
    act(() =>
      live().receive({
        type: 'execution_report',
        data: { execId: 3, orderId: 9, type: ExecType.CANCEL, orderStatus: OrderStatus.CANCELED, market: '', transactTime: 0 },
      }),
    );
    act(() => live().receive({ type: 'cancel_reject', data: { orderId: 9, reason: 'Cancel rejected: too late' } }));
    expect(hook.result.current.notices.map(n => n.text)).toEqual([
      'Order 9: Cancel rejected: too late',
      'Cancel of order 7: Cancel rejected: too late',
    ]);
  });

  it('forgets every request when its socket closes: nothing answers one on the next connection', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      hook.result.current.sendOrder(ORDER);
    });
    const [order] = requestIds();
    act(() => live().drop());
    advance(RECONNECT_DELAY);
    act(() => live().open());
    act(() => live().receive({ type: 'error', message: 'late', requestId: order }));
    expect(hook.result.current.notices.map(n => n.text)).toEqual(['late']);
  });

  it('remembers the newest 100 requests, none older than ten minutes', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      for (let i = 0; i < 101; i++) hook.result.current.cancelOrder(i + 1);
    });
    const ids = requestIds();
    advance(9 * 60 * 1000);
    act(() => {
      hook.result.current.sendOrder(ORDER);
    });
    advance(2 * 60 * 1000); // the cancels are 11 minutes old, the order 2
    act(() => {
      hook.result.current.cancelOrder(500);
    });
    const [order, last] = requestIds().slice(101);
    // The first cancel went when the 101st came, the third when the cancels were over ten minutes old
    act(() => live().receive({ type: 'error', message: 'first cancel', requestId: ids[0] }));
    act(() => live().receive({ type: 'error', message: 'third cancel', requestId: ids[2] }));
    act(() => live().receive({ type: 'error', message: 'order', requestId: order }));
    act(() => live().receive({ type: 'error', message: 'last', requestId: last }));
    expect(hook.result.current.notices.map(n => n.text)).toEqual([
      'Cancel of order 500: last',
      'New order (BUY 10 AAA): order',
      'third cancel',
      'first cancel',
    ]);
  });
});

describe('answers matched by the order a request made or named (#790)', () => {
  it("names a new order the engine stored and rejected, by the request id its first update carries", () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      hook.result.current.sendOrder(ORDER);
    });
    const [id] = requestIds();
    act(() => live().receive(rejectReport(5, 'Order refused: no market to trade with')));
    act(() =>
      live().receive({
        type: 'order_update',
        data: order({ orderId: 5, clOrderId: 'WS-1', status: OrderStatus.REJECTED }),
        requestId: id,
      }),
    );
    expect(hook.result.current.notices.map(n => [n.kind, n.text])).toEqual([
      ['reject', 'New order (BUY 10 AAA): Order refused: no market to trade with'],
    ]);
  });

  it("forgets a new order the engine accepted, and a replace once the replacement's report names its order", () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      hook.result.current.sendOrder(ORDER);
      hook.result.current.replaceOrder({ orderId: 8, price: 11 });
    });
    const [orderRequest, replaceRequest] = requestIds();
    act(() =>
      live().receive({
        type: 'order_update',
        data: order({ orderId: 6, clOrderId: 'WS-2', status: OrderStatus.NEW }),
        requestId: orderRequest,
      }),
    );
    act(() =>
      live().receive({
        type: 'execution_report',
        data: {
          execId: 7,
          orderId: 9,
          origOrderId: 8,
          type: ExecType.REPLACE,
          orderStatus: OrderStatus.NEW,
          market: '',
          transactTime: 0,
        },
      }),
    );
    expect(hook.result.current.notices).toEqual([]);
    // Both are answered, so an error that echoes either id later names no request
    act(() => live().receive({ type: 'error', message: 'late', requestId: orderRequest }));
    act(() => live().receive({ type: 'error', message: 'later', requestId: replaceRequest }));
    expect(hook.result.current.notices.map(n => n.text)).toEqual(['later', 'late']);
  });

  it('gives no notice for a rejected order whose update names no request of this tab', () => {
    const hook = renderOms();
    act(() => live().open());
    act(() => {
      hook.result.current.sendOrder(ORDER);
    });
    act(() => live().receive(rejectReport(5, 'Order refused: no market to trade with')));
    // Another tab's order, and an update that names no request: neither answers this tab's request
    act(() =>
      live().receive({
        type: 'order_update',
        data: order({ orderId: 5, status: OrderStatus.REJECTED }),
        requestId: 'another-tab',
      }),
    );
    act(() => live().receive({ type: 'order_update', data: order({ orderId: 5, status: OrderStatus.REJECTED }) }));
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
