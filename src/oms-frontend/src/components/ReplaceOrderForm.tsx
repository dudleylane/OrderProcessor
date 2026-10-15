import React, { useState } from 'react';
import { OrderType, TimeInForce } from '../types/enums';
import type { Order, ReplaceOrderRequest } from '../types';

interface ReplaceOrderFormProps {
  order: Order;
  replaceOrder: (req: ReplaceOrderRequest) => void;
  onClose: () => void;
}

/** Milliseconds since the epoch, as a datetime-local input's value in local time */
function toLocalInput(ms: number): string {
  if (!(ms > 0)) {
    return '';
  }
  const d = new Date(ms);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/**
 * Replaces an order: its price, stop price, quantity and time in force, prefilled from the order. Only the fields that
 * change are sent: the server builds the replacement from the changes the request carries, and refuses one with none
 * (#84). A stop order's stop can move only while the order is held: once a trade has reached it, the server refuses a
 * new one (#238).
 */
export function ReplaceOrderForm({ order, replaceOrder, onClose }: ReplaceOrderFormProps) {
  const isLimit = order.ordType === OrderType.LIMIT || order.ordType === OrderType.STOPLIMIT;
  const isHeldStop =
    (order.ordType === OrderType.STOP || order.ordType === OrderType.STOPLIMIT) && !order.stopTriggered;
  const [price, setPrice] = useState(String(order.price));
  const [stopPx, setStopPx] = useState(String(order.stopPx));
  const [orderQty, setOrderQty] = useState(String(order.orderQty));
  const [tif, setTif] = useState<TimeInForce>(order.tif);
  // A GTD order's expiry, as a local date and time; the order's own if it is GTD (#240). It counts as changed only if
  // edited: the input holds minutes, and the order's time may not fall on one.
  const initialExpireAt = order.tif === TimeInForce.GTD ? toLocalInput(order.expireTime) : '';
  const [expireAt, setExpireAt] = useState(initialExpireAt);

  const newPrice = parseFloat(price);
  const newStopPx = parseFloat(stopPx);
  const newQty = parseInt(orderQty, 10);
  const priceChanged = isLimit && newPrice !== order.price;
  const stopPxChanged = isHeldStop && newStopPx !== order.stopPx;
  const qtyChanged = newQty !== order.orderQty;
  const tifChanged = tif !== order.tif;
  const newExpireTime = Date.parse(expireAt);
  const expireTimeChanged =
    tif === TimeInForce.GTD && !Number.isNaN(newExpireTime) && (tifChanged || expireAt !== initialExpireAt);

  // The engine refuses the same cases; saying so here saves a round trip
  let problem = '';
  if (isLimit && !(newPrice > 0)) {
    problem = 'Price must be above 0';
  } else if (isHeldStop && !(newStopPx > 0)) {
    problem = 'Stop price must be above 0';
  } else if (!(newQty > 0)) {
    problem = 'Quantity must be above 0';
  } else if (newQty <= order.cumQty) {
    problem = `Quantity must be above the ${order.cumQty} already filled`;
  } else if (tif === TimeInForce.GTD && Number.isNaN(newExpireTime)) {
    problem = 'A GTD order needs an expiry';
  } else if (expireTimeChanged && newExpireTime <= Date.now()) {
    problem = 'The expiry has passed';
  } else if (!priceChanged && !stopPxChanged && !qtyChanged && !tifChanged && !expireTimeChanged) {
    problem = isHeldStop
      ? 'Change the price, stop price, quantity or time in force'
      : 'Change the price, quantity or time in force';
  }

  const submit = (e: React.FormEvent) => {
    e.preventDefault();
    if (problem) {
      return;
    }
    const req: ReplaceOrderRequest = { orderId: order.orderId, clOrderId: order.clOrderId };
    if (priceChanged) {
      req.price = newPrice;
    }
    if (stopPxChanged) {
      req.stopPx = newStopPx;
    }
    if (qtyChanged) {
      req.orderQty = newQty;
    }
    if (tifChanged) {
      req.tif = tif;
    }
    if (expireTimeChanged) {
      req.expireTime = newExpireTime;
    }
    replaceOrder(req);
    onClose();
  };

  return (
    <form onSubmit={submit} className="flex flex-wrap items-end gap-3 p-3 bg-gray-900/60">
      {isLimit && (
        <div>
          <label className="block text-xs text-gray-500 mb-1">Price</label>
          <input
            type="text"
            inputMode="decimal"
            value={price}
            onChange={e => setPrice(e.target.value.replace(/[^0-9.]/g, ''))}
            className="w-28 bg-gray-900 border border-gray-700 rounded-sm px-2 py-1.5 text-sm font-mono"
          />
        </div>
      )}
      {isHeldStop && (
        <div>
          <label className="block text-xs text-gray-500 mb-1">Stop Price</label>
          <input
            type="text"
            inputMode="decimal"
            value={stopPx}
            onChange={e => setStopPx(e.target.value.replace(/[^0-9.]/g, ''))}
            className="w-28 bg-gray-900 border border-gray-700 rounded-sm px-2 py-1.5 text-sm font-mono"
          />
        </div>
      )}
      <div>
        <label className="block text-xs text-gray-500 mb-1">Quantity</label>
        <input
          type="text"
          inputMode="numeric"
          value={orderQty}
          onChange={e => setOrderQty(e.target.value.replace(/[^0-9]/g, ''))}
          className="w-28 bg-gray-900 border border-gray-700 rounded-sm px-2 py-1.5 text-sm font-mono"
        />
      </div>
      <div>
        <label className="block text-xs text-gray-500 mb-1">Time in Force</label>
        <select
          value={tif}
          onChange={e => setTif(e.target.value as TimeInForce)}
          className="bg-gray-900 border border-gray-700 rounded-sm px-2 py-1.5 text-sm"
        >
          {Object.values(TimeInForce).map(t => (
            <option key={t} value={t}>{t}</option>
          ))}
        </select>
      </div>
      {tif === TimeInForce.GTD && (
        <div>
          <label className="block text-xs text-gray-500 mb-1">Expires</label>
          <input
            type="datetime-local"
            value={expireAt}
            onChange={e => setExpireAt(e.target.value)}
            className="bg-gray-900 border border-gray-700 rounded-sm px-2 py-1.5 text-sm"
          />
        </div>
      )}
      <button
        type="submit"
        disabled={problem !== ''}
        className="px-3 py-1.5 rounded-sm text-xs bg-cyan-700 hover:bg-cyan-600 disabled:opacity-40 disabled:cursor-not-allowed"
      >
        Send replace
      </button>
      <button type="button" onClick={onClose} className="px-3 py-1.5 rounded-sm text-xs text-gray-400 hover:text-gray-200">
        Close
      </button>
      {problem && <span className="text-xs text-gray-500">{problem}</span>}
    </form>
  );
}
