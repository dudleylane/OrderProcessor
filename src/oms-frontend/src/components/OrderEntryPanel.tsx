import React, { useState } from 'react';
import { OrderType, Side, TimeInForce, Currency, Capacity } from '../types/enums';
import type { Instrument, Account, NewOrderRequest } from '../types';

interface OrderEntryPanelProps {
  instruments: Instrument[];
  accounts: Account[];
  sendOrder: (req: NewOrderRequest) => void;
  connected: boolean;
}

const BASIC_SIDES: Side[] = [Side.BUY, Side.SELL];
const ADVANCED_SIDES: Side[] = [Side.BUY_MINUS, Side.SELL_PLUS, Side.SELL_SHORT, Side.CROSS];

export function OrderEntryPanel({
  instruments,
  accounts,
  sendOrder,
  connected,
}: OrderEntryPanelProps) {
  const [symbol, setSymbol] = useState('');
  const [side, setSide] = useState<Side>(Side.BUY);
  const [ordType, setOrdType] = useState<OrderType>(OrderType.LIMIT);
  const [price, setPrice] = useState('');
  const [stopPx, setStopPx] = useState('');
  const [orderQty, setOrderQty] = useState('');
  const [minQty, setMinQty] = useState('');
  const [tif, setTif] = useState<TimeInForce>(TimeInForce.DAY);
  const [account, setAccount] = useState('');
  const [currency, setCurrency] = useState<Currency>(Currency.USD);
  const [capacity, setCapacity] = useState<Capacity>(Capacity.AGENCY);
  const [lastResult, setLastResult] = useState<string | null>(null);

  const needsPrice = ordType === OrderType.LIMIT || ordType === OrderType.STOPLIMIT;
  const needsStopPx = ordType === OrderType.STOP || ordType === OrderType.STOPLIMIT;

  const validate = (): string | null => {
    if (!symbol) return 'Select an instrument';
    const qty = parseInt(orderQty);
    if (!qty || qty <= 0) return 'Quantity must be > 0';
    if (needsPrice) {
      const p = parseFloat(price);
      if (!p || p <= 0) return 'Price must be > 0 for LIMIT orders';
    }
    if (needsStopPx) {
      const sp = parseFloat(stopPx);
      if (!sp || sp <= 0) return 'Stop price must be > 0 for STOP orders';
    }
    return null;
  };

  const handleSubmit = () => {
    const error = validate();
    if (error) {
      setLastResult(error);
      return;
    }

    const req: NewOrderRequest = {
      symbol,
      side,
      ordType,
      orderQty: parseInt(orderQty),
      tif,
      currency,
      capacity,
    };

    if (needsPrice) req.price = parseFloat(price);
    if (needsStopPx) req.stopPx = parseFloat(stopPx);
    if (minQty) req.minQty = parseInt(minQty);
    if (account) req.account = account;

    sendOrder(req);
    setLastResult('Order submitted');
  };

  return (
    <div className="p-4">
      <h2 className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-3">
        Order Entry
      </h2>

      <div className="space-y-3">
        {/* Instrument */}
        <div>
          <label className="block text-xs text-gray-500 mb-1">Instrument</label>
          <select
            value={symbol}
            onChange={e => setSymbol(e.target.value)}
            className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm"
          >
            <option value="">Select...</option>
            {instruments.map(inst => (
              <option key={inst.id} value={inst.symbol}>{inst.symbol}</option>
            ))}
          </select>
        </div>

        {/* Side */}
        <div>
          <label className="block text-xs text-gray-500 mb-1">Side</label>
          <div className="flex gap-2 mb-1">
            {BASIC_SIDES.map(s => (
              <button
                key={s}
                onClick={() => setSide(s)}
                className={`flex-1 py-2 rounded text-sm font-medium transition-colors
                  ${side === s
                    ? (s === Side.BUY ? 'bg-blue-600 text-white' : 'bg-orange-600 text-white')
                    : 'bg-gray-800 text-gray-400 hover:bg-gray-700'}`}
              >
                {s}
              </button>
            ))}
          </div>
          <select
            value={ADVANCED_SIDES.includes(side) ? side : ''}
            onChange={e => { if (e.target.value) setSide(e.target.value as Side); }}
            className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1 text-xs text-gray-500"
          >
            <option value="">Advanced sides...</option>
            {ADVANCED_SIDES.map(s => (
              <option key={s} value={s}>{s.replace(/_/g, ' ')}</option>
            ))}
          </select>
        </div>

        {/* Order Type */}
        <div>
          <label className="block text-xs text-gray-500 mb-1">Order Type</label>
          <div className="flex gap-1">
            {Object.values(OrderType).map(t => (
              <button
                key={t}
                onClick={() => setOrdType(t)}
                className={`flex-1 py-1.5 rounded text-xs font-medium transition-colors
                  ${ordType === t
                    ? 'bg-gray-600 text-white'
                    : 'bg-gray-800 text-gray-500 hover:bg-gray-700'}`}
              >
                {t}
              </button>
            ))}
          </div>
        </div>

        {/* Price + Stop Px */}
        <div className="flex gap-2">
          {needsPrice && (
            <div className="flex-1">
              <label className="block text-xs text-gray-500 mb-1">Price</label>
              <input
                type="text"
                value={price}
                onChange={e => setPrice(e.target.value.replace(/[^0-9.]/g, ''))}
                className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm font-mono"
                placeholder="0.0000"
              />
            </div>
          )}
          {needsStopPx && (
            <div className="flex-1">
              <label className="block text-xs text-gray-500 mb-1">Stop Price</label>
              <input
                type="text"
                value={stopPx}
                onChange={e => setStopPx(e.target.value.replace(/[^0-9.]/g, ''))}
                className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm font-mono"
                placeholder="0.0000"
              />
            </div>
          )}
        </div>

        {/* Qty + Min Qty */}
        <div className="flex gap-2">
          <div className="flex-1">
            <label className="block text-xs text-gray-500 mb-1">Quantity</label>
            <input
              type="text"
              value={orderQty}
              onChange={e => setOrderQty(e.target.value.replace(/[^0-9]/g, ''))}
              className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm font-mono"
              placeholder="0"
            />
            {orderQty && parseInt(orderQty) > 0 && (
              <div className="text-xs text-gray-600 mt-0.5">
                {parseInt(orderQty) >= 1_000_000
                  ? `${(parseInt(orderQty) / 1_000_000).toFixed(1)}M`
                  : parseInt(orderQty) >= 1_000
                    ? `${(parseInt(orderQty) / 1_000).toFixed(0)}K`
                    : ''}
              </div>
            )}
          </div>
          <div className="flex-1">
            <label className="block text-xs text-gray-500 mb-1">Min Qty</label>
            <input
              type="text"
              value={minQty}
              onChange={e => setMinQty(e.target.value.replace(/[^0-9]/g, ''))}
              className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm font-mono"
              placeholder="Optional"
            />
          </div>
        </div>

        {/* TIF + Account */}
        <div className="flex gap-2">
          <div className="flex-1">
            <label className="block text-xs text-gray-500 mb-1">Time in Force</label>
            <select
              value={tif}
              onChange={e => setTif(e.target.value as TimeInForce)}
              className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm"
            >
              {Object.values(TimeInForce).map(t => (
                <option key={t} value={t}>{t}</option>
              ))}
            </select>
          </div>
          <div className="flex-1">
            <label className="block text-xs text-gray-500 mb-1">Account</label>
            <select
              value={account}
              onChange={e => setAccount(e.target.value)}
              className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm"
            >
              <option value="">Default</option>
              {accounts.map(a => (
                <option key={a.id} value={a.account}>{a.account} ({a.firm})</option>
              ))}
            </select>
          </div>
        </div>

        {/* Currency + Capacity */}
        <div className="flex gap-2">
          <div className="flex-1">
            <label className="block text-xs text-gray-500 mb-1">Currency</label>
            <select
              value={currency}
              onChange={e => setCurrency(e.target.value as Currency)}
              className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm"
            >
              {Object.values(Currency).map(c => (
                <option key={c} value={c}>{c}</option>
              ))}
            </select>
          </div>
          <div className="flex-1">
            <label className="block text-xs text-gray-500 mb-1">Capacity</label>
            <select
              value={capacity}
              onChange={e => setCapacity(e.target.value as Capacity)}
              className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm"
            >
              {Object.values(Capacity).map(c => (
                <option key={c} value={c}>{c.replace(/_/g, ' ')}</option>
              ))}
            </select>
          </div>
        </div>

        {/* Submit */}
        <button
          onClick={handleSubmit}
          disabled={!connected}
          className="w-full py-2.5 rounded font-medium text-sm transition-colors
            bg-blue-600 hover:bg-blue-500 disabled:bg-gray-700 disabled:text-gray-500"
        >
          {connected ? `Submit ${side} Order` : 'Disconnected'}
        </button>

        {lastResult && (
          <div className={`text-xs p-2 rounded ${
            lastResult === 'Order submitted'
              ? 'bg-green-900/30 text-green-400'
              : 'bg-red-900/30 text-red-400'
          }`}>
            {lastResult}
          </div>
        )}
      </div>
    </div>
  );
}
