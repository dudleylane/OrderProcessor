import { useCallback, useEffect, useReducer, useRef } from 'react';
import type {
  Order,
  ExecutionReport,
  Execution,
  OrderBookSnapshot,
  Instrument,
  Account,
  NewOrderRequest,
  CancelOrderRequest,
  ReplaceOrderRequest,
  SystemMetrics,
} from '../types';
import { isRejection } from '../types';
import type { ServerMessage, ClientMessage } from '../types/websocket';
import { wsSubprotocols } from '../utils/accessToken';

// /ws on the host and port the page came from, unless a build sets VITE_WS_URL (#157). The dev server and the image's
// nginx both pass /ws on to the server. A URL fixed at build time sent a browser on another machine to its own
// loopback, and broke the page behind a remapped port.
const WS_URL =
  import.meta.env.VITE_WS_URL || `${window.location.protocol === 'https:' ? 'wss:' : 'ws:'}//${window.location.host}/ws`;
const RECONNECT_DELAY = 3000;
const MAX_EXECUTIONS = 1000;
const MAX_METRICS_HISTORY = 300; // 5 minutes at 1/sec
const MAX_NOTICES = 50;

/**
 * A refusal the server sent, or a request the socket didn't take (#521): an error, a cancel or business reject, a
 * REJECT report for an order never stored, or a request not sent. Each stays until the user dismisses it.
 */
export interface Notice {
  id: number;
  time: number;
  kind: 'error' | 'cancel_reject' | 'business_reject' | 'reject' | 'not_sent';
  text: string;
}

// --- State & Reducer ---

export interface OmsState {
  connected: boolean;
  // Whether a connection has opened since the access token was entered. Until one has, a closed connection may be the
  // server refusing the token: a browser doesn't tell the app why a WebSocket failed to open (#146).
  everConnected: boolean;
  instruments: Instrument[];
  accounts: Account[];
  orders: Map<string, Order>;
  executions: Execution[];
  bookData: Map<string, OrderBookSnapshot>;
  metricsHistory: SystemMetrics[];
  // Newest first
  notices: Notice[];
}

type OmsAction =
  | { type: 'CONNECTED' }
  | { type: 'DISCONNECTED' }
  | { type: 'INSTRUMENT_LIST'; data: Instrument[] }
  | { type: 'ACCOUNT_LIST'; data: Account[] }
  | { type: 'ORDER_SNAPSHOT'; data: Order[] }
  | { type: 'ORDER_UPDATE'; data: Order }
  | { type: 'EXECUTION_REPORT'; data: ExecutionReport }
  | { type: 'BOOK_UPDATE'; data: OrderBookSnapshot }
  | { type: 'METRICS_UPDATE'; data: SystemMetrics }
  | { type: 'NOTICE'; notice: Notice }
  | { type: 'DISMISS_NOTICE'; id: number }
  | { type: 'DISMISS_NOTICES' };

function omsReducer(state: OmsState, action: OmsAction): OmsState {
  switch (action.type) {
    case 'CONNECTED':
      return { ...state, connected: true, everConnected: true };
    case 'DISCONNECTED':
      return { ...state, connected: false };
    case 'INSTRUMENT_LIST':
      return { ...state, instruments: action.data };
    case 'ACCOUNT_LIST':
      return { ...state, accounts: action.data };
    case 'ORDER_SNAPSHOT': {
      const orders = new Map<string, Order>();
      for (const o of action.data) {
        orders.set(String(o.orderId), o);
      }
      return { ...state, orders };
    }
    case 'ORDER_UPDATE': {
      const orders = new Map(state.orders);
      orders.set(String(action.data.orderId), action.data);
      return { ...state, orders };
    }
    case 'EXECUTION_REPORT': {
      const executions = [action.data as Execution, ...state.executions]
        .slice(0, MAX_EXECUTIONS);
      return { ...state, executions };
    }
    case 'BOOK_UPDATE': {
      const bookData = new Map(state.bookData);
      bookData.set(action.data.symbol, action.data);
      return { ...state, bookData };
    }
    case 'METRICS_UPDATE': {
      const metricsHistory = [...state.metricsHistory, action.data]
        .slice(-MAX_METRICS_HISTORY);
      return { ...state, metricsHistory };
    }
    case 'NOTICE':
      return { ...state, notices: [action.notice, ...state.notices].slice(0, MAX_NOTICES) };
    case 'DISMISS_NOTICE':
      return { ...state, notices: state.notices.filter(n => n.id !== action.id) };
    case 'DISMISS_NOTICES':
      return { ...state, notices: [] };
    default:
      return state;
  }
}

const INITIAL_STATE: OmsState = {
  connected: false,
  everConnected: false,
  instruments: [],
  accounts: [],
  orders: new Map(),
  executions: [],
  bookData: new Map(),
  metricsHistory: [],
  notices: [],
};

// --- Hook ---

export interface OmsActions {
  // Each says whether the socket took the request; one it didn't take also gets a notice (#521)
  sendOrder: (req: NewOrderRequest) => boolean;
  cancelOrder: (orderId: number, clOrderId?: string) => boolean;
  replaceOrder: (req: ReplaceOrderRequest) => boolean;
  subscribeBook: (symbol: string) => void;
  unsubscribeBook: (symbol: string) => void;
  dismissNotice: (id: number) => void;
  dismissNotices: () => void;
}

// token: the server's access token, offered on every connection, or '' for a server that asks for none (#146)
export function useOmsWebSocket(token: string): OmsState & OmsActions {
  const wsRef = useRef<WebSocket | null>(null);
  const reconnectRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  // The symbols whose books the app follows. The server keeps a session's subscriptions, and a reconnect's session
  // starts with none, so each connection is given them as it opens (#522).
  const bookSymbolsRef = useRef(new Set<string>());
  const noticeSeq = useRef(0);
  const [state, dispatch] = useReducer(omsReducer, INITIAL_STATE);

  const notify = useCallback((kind: Notice['kind'], text: string) => {
    noticeSeq.current += 1;
    dispatch({ type: 'NOTICE', notice: { id: noticeSeq.current, time: Date.now(), kind, text } });
  }, []);

  // Whether the socket took the message: one sent while it is closed, or still opening, is lost
  const send = useCallback((msg: ClientMessage): boolean => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(msg));
      return true;
    }
    return false;
  }, []);

  const connect = useCallback(() => {
    // Clear any pending reconnect
    if (reconnectRef.current !== null) {
      clearTimeout(reconnectRef.current);
      reconnectRef.current = null;
    }

    let ws: WebSocket;
    try {
      ws = new WebSocket(WS_URL, wsSubprotocols(token));
    } catch (e) {
      // A URL or subprotocol the browser won't use: retrying wouldn't help
      console.error('Cannot open a WebSocket to', WS_URL, e);
      return;
    }

    ws.onopen = () => {
      dispatch({ type: 'CONNECTED' });
      for (const symbol of bookSymbolsRef.current) {
        const msg: ClientMessage = { type: 'subscribe_book', symbol };
        ws.send(JSON.stringify(msg));
      }
    };

    ws.onclose = () => {
      dispatch({ type: 'DISCONNECTED' });
      // Fix: store timeout ID in ref so cleanup can cancel it
      reconnectRef.current = setTimeout(connect, RECONNECT_DELAY);
    };

    ws.onerror = () => {
      ws.close();
    };

    ws.onmessage = (event) => {
      try {
        const msg: ServerMessage = JSON.parse(event.data);

        switch (msg.type) {
          case 'connected':
            break;
          case 'instrument_list':
            dispatch({ type: 'INSTRUMENT_LIST', data: msg.data });
            break;
          case 'account_list':
            dispatch({ type: 'ACCOUNT_LIST', data: msg.data });
            break;
          case 'order_snapshot':
            dispatch({ type: 'ORDER_SNAPSHOT', data: msg.data });
            break;
          case 'order_update':
            dispatch({ type: 'ORDER_UPDATE', data: msg.data });
            break;
          case 'execution_report': {
            dispatch({ type: 'EXECUTION_REPORT', data: msg.data });
            // An order or a replacement refused before it was stored: the report comes to its sender alone, and the
            // order never reaches the order blotter. Its reason says which ("Order refused: ...", "Replace refused:
            // ..."). A stored order's REJECT shows in its row there.
            const exec = msg.data as Execution;
            if (exec.orderId === 0 && isRejection(exec)) {
              notify('reject', exec.rejectReason || 'Order refused');
            }
            break;
          }
          case 'book_update':
            dispatch({ type: 'BOOK_UPDATE', data: msg.data });
            break;
          case 'metrics_update':
            dispatch({ type: 'METRICS_UPDATE', data: msg.data });
            break;
          case 'cancel_reject':
            // To every session that may see the order's account, so the notice names the order
            notify('cancel_reject', msg.data.orderId ? `Order ${msg.data.orderId}: ${msg.data.reason}` : msg.data.reason);
            break;
          case 'business_reject':
            notify('business_reject', `${msg.data.reason} (reference ${msg.data.refId})`);
            break;
          case 'error':
            notify('error', msg.message);
            break;
        }
      } catch (e) {
        console.error('Failed to parse WS message:', e);
      }
    };

    wsRef.current = ws;
  }, [token, notify]);

  useEffect(() => {
    connect();

    return () => {
      // Fix: close WS and cancel reconnect timer on cleanup
      if (reconnectRef.current !== null) {
        clearTimeout(reconnectRef.current);
        reconnectRef.current = null;
      }
      // A browser fires the close event later, and the socket's onclose would reconnect with this effect's token, as
      // a socket of its own beside the next effect's: so the socket let go of here says nothing more (#523)
      const ws = wsRef.current;
      wsRef.current = null;
      if (ws) {
        ws.onopen = null;
        ws.onmessage = null;
        ws.onerror = null;
        ws.onclose = null;
        ws.close();
      }
      dispatch({ type: 'DISCONNECTED' });
    };
  }, [connect]);

  // --- Exposed send methods ---

  // A request the socket didn't take gets a notice naming it: the blotter's cancel button has no line of its own
  const sendOrder = useCallback(
    (req: NewOrderRequest) => {
      const sent = send({ type: 'new_order', data: req });
      if (!sent) notify('not_sent', `New order (${req.side} ${req.orderQty} ${req.symbol}) not sent: not connected`);
      return sent;
    },
    [send, notify],
  );

  const cancelOrder = useCallback(
    (orderId: number, clOrderId?: string) => {
      const sent = send({ type: 'cancel_order', data: { orderId, clOrderId } });
      if (!sent) notify('not_sent', `Cancel of order ${orderId} not sent: not connected`);
      return sent;
    },
    [send, notify],
  );

  const replaceOrder = useCallback(
    (req: ReplaceOrderRequest) => {
      const sent = send({ type: 'replace_order', data: req });
      if (!sent) notify('not_sent', `Replace of order ${req.orderId} not sent: not connected`);
      return sent;
    },
    [send, notify],
  );

  // A symbol subscribed while the socket is closed is sent when it opens
  const subscribeBook = useCallback(
    (symbol: string) => {
      bookSymbolsRef.current.add(symbol);
      send({ type: 'subscribe_book', symbol });
    },
    [send],
  );

  const unsubscribeBook = useCallback(
    (symbol: string) => {
      bookSymbolsRef.current.delete(symbol);
      send({ type: 'unsubscribe_book', symbol });
    },
    [send],
  );

  const dismissNotice = useCallback((id: number) => dispatch({ type: 'DISMISS_NOTICE', id }), []);
  const dismissNotices = useCallback(() => dispatch({ type: 'DISMISS_NOTICES' }), []);

  return {
    ...state,
    sendOrder,
    cancelOrder,
    replaceOrder,
    subscribeBook,
    unsubscribeBook,
    dismissNotice,
    dismissNotices,
  };
}
