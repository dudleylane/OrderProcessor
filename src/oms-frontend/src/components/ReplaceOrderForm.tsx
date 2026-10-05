import React, { useState } from 'react';
import { OrderType, TimeInForce } from '../types/enums';
import type { Order, ReplaceOrderRequest } from '../types';

interface ReplaceOrderFormProps {
  order: Order;
  replaceOrder: (req: ReplaceOrderRequest) => void;
  onClose: () => void;
}

/**
 * Replaces an order: its price, quantity and time in force, prefilled from the order. Only the fields that change are
 * sent: the server builds the replacement from the changes the request carries, and refuses one with none (#84).
 */
export function ReplaceOrderForm({ order, replaceOrder, onClose }: ReplaceOrderFormProps) {
  const isLimit = order.ordType === OrderType.LIMIT;
  const [price, setPrice] = useState(String(order.price));
  const [orderQty, setOrderQty] = useState(String(order.orderQty));
  const [tif, setTif] = useState<TimeInForce>(order.tif);

  const newPrice = parseFloat(price);
  const newQty = parseInt(orderQty, 10);
  const priceChanged = isLimit && newPrice !== order.price;
  const qtyChanged = newQty !== order.orderQty;
  const tifChanged = tif !== order.tif;

  // The engine refuses the same cases; saying so here saves a round trip
  let problem = '';
  if (isLimit && !(newPrice > 0)) {
    problem = 'Price must be above 0';
  } else if (!(newQty > 0)) {
    problem = 'Quantity must be above 0';
  } else if (newQty <= order.cumQty) {
    problem = `Quantity must be above the ${order.cumQty} already filled`;
  } else if (!priceChanged && !qtyChanged && !tifChanged) {
    problem = 'Change the price, quantity or time in force';
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
    if (qtyChanged) {
      req.orderQty = newQty;
    }
    if (tifChanged) {
      req.tif = tif;
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
            className="w-28 bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm font-mono"
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
          className="w-28 bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm font-mono"
        />
      </div>
      <div>
        <label className="block text-xs text-gray-500 mb-1">Time in Force</label>
        <select
          value={tif}
          onChange={e => setTif(e.target.value as TimeInForce)}
          className="bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm"
        >
          {Object.values(TimeInForce).map(t => (
            <option key={t} value={t}>{t}</option>
          ))}
        </select>
      </div>
      <button
        type="submit"
        disabled={problem !== ''}
        className="px-3 py-1.5 rounded text-xs bg-cyan-700 hover:bg-cyan-600 disabled:opacity-40 disabled:cursor-not-allowed"
      >
        Send replace
      </button>
      <button type="button" onClick={onClose} className="px-3 py-1.5 rounded text-xs text-gray-400 hover:text-gray-200">
        Close
      </button>
      {problem && <span className="text-xs text-gray-500">{problem}</span>}
    </form>
  );
}
