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
} from '../types';
import type { ServerMessage, ClientMessage } from '../types/websocket';

const WS_URL = import.meta.env.VITE_WS_URL || 'ws://localhost:8080/ws';
const RECONNECT_DELAY = 3000;
const MAX_EXECUTIONS = 1000;

// --- State & Reducer ---

export interface OmsState {
  connected: boolean;
  instruments: Instrument[];
  accounts: Account[];
  orders: Map<string, Order>;
  executions: Execution[];
  bookData: Map<string, OrderBookSnapshot>;
}

type OmsAction =
  | { type: 'CONNECTED' }
  | { type: 'DISCONNECTED' }
  | { type: 'INSTRUMENT_LIST'; data: Instrument[] }
  | { type: 'ACCOUNT_LIST'; data: Account[] }
  | { type: 'ORDER_SNAPSHOT'; data: Order[] }
  | { type: 'ORDER_UPDATE'; data: Order }
  | { type: 'EXECUTION_REPORT'; data: ExecutionReport }
  | { type: 'BOOK_UPDATE'; data: OrderBookSnapshot };

function omsReducer(state: OmsState, action: OmsAction): OmsState {
  switch (action.type) {
    case 'CONNECTED':
      return { ...state, connected: true };
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
    default:
      return state;
  }
}

const INITIAL_STATE: OmsState = {
  connected: false,
  instruments: [],
  accounts: [],
  orders: new Map(),
  executions: [],
  bookData: new Map(),
};

// --- Hook ---

export interface OmsActions {
  sendOrder: (req: NewOrderRequest) => void;
  cancelOrder: (orderId: number, clOrderId?: string) => void;
  replaceOrder: (req: ReplaceOrderRequest) => void;
  subscribeBook: (symbol: string) => void;
  unsubscribeBook: (symbol: string) => void;
}

export function useOmsWebSocket(): OmsState & OmsActions {
  const wsRef = useRef<WebSocket | null>(null);
  const reconnectRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [state, dispatch] = useReducer(omsReducer, INITIAL_STATE);

  const send = useCallback((msg: ClientMessage) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(msg));
    }
  }, []);

  const connect = useCallback(() => {
    // Clear any pending reconnect
    if (reconnectRef.current !== null) {
      clearTimeout(reconnectRef.current);
      reconnectRef.current = null;
    }

    const ws = new WebSocket(WS_URL);

    ws.onopen = () => {
      dispatch({ type: 'CONNECTED' });
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
          case 'execution_report':
            dispatch({ type: 'EXECUTION_REPORT', data: msg.data });
            break;
          case 'book_update':
            dispatch({ type: 'BOOK_UPDATE', data: msg.data });
            break;
          case 'cancel_reject':
            console.warn('Cancel rejected:', msg.data.reason);
            break;
          case 'business_reject':
            console.warn('Business reject:', msg.data.reason);
            break;
          case 'error':
            console.error('Server error:', msg.message);
            break;
        }
      } catch (e) {
        console.error('Failed to parse WS message:', e);
      }
    };

    wsRef.current = ws;
  }, []);

  useEffect(() => {
    connect();

    return () => {
      // Fix: close WS and cancel reconnect timer on cleanup
      if (reconnectRef.current !== null) {
        clearTimeout(reconnectRef.current);
        reconnectRef.current = null;
      }
      wsRef.current?.close();
    };
  }, [connect]);

  // --- Exposed send methods ---

  const sendOrder = useCallback(
    (req: NewOrderRequest) => send({ type: 'new_order', data: req }),
    [send],
  );

  const cancelOrder = useCallback(
    (orderId: number, clOrderId?: string) =>
      send({ type: 'cancel_order', data: { orderId, clOrderId } }),
    [send],
  );

  const replaceOrder = useCallback(
    (req: ReplaceOrderRequest) => send({ type: 'replace_order', data: req }),
    [send],
  );

  const subscribeBook = useCallback(
    (symbol: string) => send({ type: 'subscribe_book', symbol }),
    [send],
  );

  const unsubscribeBook = useCallback(
    (symbol: string) => send({ type: 'unsubscribe_book', symbol }),
    [send],
  );

  return {
    ...state,
    sendOrder,
    cancelOrder,
    replaceOrder,
    subscribeBook,
    unsubscribeBook,
  };
}
