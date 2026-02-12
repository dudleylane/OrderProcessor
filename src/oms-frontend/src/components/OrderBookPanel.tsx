import React, { useEffect, useState } from 'react';
import type { Instrument, OrderBookSnapshot } from '../types';
import { formatPrice, formatQuantity } from '../utils/format';

interface OrderBookPanelProps {
  instruments: Instrument[];
  bookData: Map<string, OrderBookSnapshot>;
  subscribeBook: (symbol: string) => void;
  unsubscribeBook: (symbol: string) => void;
}

function bookAge(timestamp: number): { label: string; color: string } {
  const age = Date.now() - timestamp;
  if (age < 5000) return { label: `${Math.floor(age / 1000)}s`, color: 'text-green-400' };
  if (age < 30000) return { label: `${Math.floor(age / 1000)}s`, color: 'text-amber-400' };
  return { label: `${Math.floor(age / 1000)}s`, color: 'text-red-400' };
}

export function OrderBookPanel({
  instruments,
  bookData,
  subscribeBook,
  unsubscribeBook,
}: OrderBookPanelProps) {
  const [selectedSymbol, setSelectedSymbol] = useState<string | null>(null);
  const [, setTick] = useState(0);

  // Re-render every 2s to update staleness indicator
  useEffect(() => {
    const iv = setInterval(() => setTick(t => t + 1), 2000);
    return () => clearInterval(iv);
  }, []);

  // Subscribe/unsubscribe when selection changes
  useEffect(() => {
    if (!selectedSymbol) return;
    subscribeBook(selectedSymbol);
    return () => {
      unsubscribeBook(selectedSymbol);
    };
  }, [selectedSymbol, subscribeBook, unsubscribeBook]);

  const snapshot = selectedSymbol ? bookData.get(selectedSymbol) : null;

  return (
    <div className="p-4 border-b border-gray-800">
      <h2 className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-3">
        Order Book
      </h2>

      <div className="flex gap-3">
        {/* Instrument list */}
        <div className="w-1/3 max-h-64 overflow-y-auto">
          {instruments.length === 0 ? (
            <div className="text-xs text-gray-600">No instruments</div>
          ) : (
            instruments.map(inst => (
              <button
                key={inst.id}
                onClick={() => setSelectedSymbol(inst.symbol)}
                className={`block w-full text-left px-2 py-1.5 text-xs font-mono rounded transition-colors
                  ${selectedSymbol === inst.symbol
                    ? 'bg-blue-900/40 text-blue-400'
                    : 'text-gray-400 hover:bg-gray-800'}`}
              >
                {inst.symbol}
              </button>
            ))
          )}
        </div>

        {/* Bid/Ask depth */}
        <div className="flex-1">
          {!selectedSymbol ? (
            <div className="text-xs text-gray-600 flex items-center justify-center h-32">
              Select an instrument
            </div>
          ) : !snapshot ? (
            <div className="text-xs text-gray-600 flex items-center justify-center h-32">
              Waiting for data...
            </div>
          ) : (
            <>
              {/* Staleness */}
              <div className="flex justify-between items-center mb-2">
                <span className="text-xs font-mono font-medium text-gray-300">
                  {selectedSymbol}
                </span>
                <span className={`text-[10px] ${bookAge(snapshot.timestamp).color}`}>
                  {bookAge(snapshot.timestamp).label} ago
                </span>
              </div>

              <div className="flex gap-2 text-xs">
                {/* Bids (descending, best first) */}
                <div className="flex-1">
                  <div className="text-gray-500 mb-1 font-medium">Bids</div>
                  <table className="w-full">
                    <thead>
                      <tr className="text-gray-600">
                        <th className="text-right pr-2 pb-1">#</th>
                        <th className="text-right pr-2 pb-1">Qty</th>
                        <th className="text-right pb-1">Price</th>
                      </tr>
                    </thead>
                    <tbody>
                      {snapshot.bids.slice(0, 8).map((lvl, i) => (
                        <tr key={i} className="text-blue-400">
                          <td className="text-right pr-2 text-gray-600">{lvl.orderCount}</td>
                          <td className="text-right pr-2 font-mono">{formatQuantity(lvl.qty)}</td>
                          <td className="text-right font-mono font-medium">{formatPrice(lvl.price)}</td>
                        </tr>
                      ))}
                      {snapshot.bids.length === 0 && (
                        <tr><td colSpan={3} className="text-gray-600 text-center py-2">--</td></tr>
                      )}
                    </tbody>
                  </table>
                </div>

                {/* Asks (ascending, best first) */}
                <div className="flex-1">
                  <div className="text-gray-500 mb-1 font-medium">Asks</div>
                  <table className="w-full">
                    <thead>
                      <tr className="text-gray-600">
                        <th className="text-left pb-1">Price</th>
                        <th className="text-left pl-2 pb-1">Qty</th>
                        <th className="text-left pl-2 pb-1">#</th>
                      </tr>
                    </thead>
                    <tbody>
                      {snapshot.asks.slice(0, 8).map((lvl, i) => (
                        <tr key={i} className="text-orange-400">
                          <td className="font-mono font-medium">{formatPrice(lvl.price)}</td>
                          <td className="pl-2 font-mono">{formatQuantity(lvl.qty)}</td>
                          <td className="pl-2 text-gray-600">{lvl.orderCount}</td>
                        </tr>
                      ))}
                      {snapshot.asks.length === 0 && (
                        <tr><td colSpan={3} className="text-gray-600 text-center py-2">--</td></tr>
                      )}
                    </tbody>
                  </table>
                </div>
              </div>
            </>
          )}
        </div>
      </div>
    </div>
  );
}
