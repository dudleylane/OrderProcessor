import React, { useState } from 'react';
import { useOmsWebSocket } from './hooks/useOmsWebSocket';
import { OrderBookPanel } from './components/OrderBookPanel';
import { OrderEntryPanel } from './components/OrderEntryPanel';
import { OrderBlotter } from './components/OrderBlotter';
import { ExecutionBlotter } from './components/ExecutionBlotter';
import { PositionSummary } from './components/PositionSummary';
import { ACTIVE_STATUSES } from './utils/constants';

export default function App() {
  const oms = useOmsWebSocket();
  const [activeTab, setActiveTab] = useState<'orders' | 'executions' | 'positions'>('orders');

  const activeOrderCount = Array.from(oms.orders.values())
    .filter(o => ACTIVE_STATUSES.has(o.status)).length;

  return (
    <div className="min-h-screen bg-gray-950 text-gray-100">
      {/* Header */}
      <header className="border-b border-gray-800 px-6 py-3 flex items-center justify-between">
        <div className="flex items-center gap-3">
          <h1 className="text-xl font-semibold tracking-tight">
            OrderProcessor <span className="text-blue-400">OMS</span>
          </h1>
        </div>
        <div className="flex items-center gap-4">
          <div className={`flex items-center gap-2 text-sm ${oms.connected ? 'text-green-400' : 'text-red-400'}`}>
            <span className={`w-2 h-2 rounded-full ${oms.connected ? 'bg-green-400' : 'bg-red-400'}`} />
            {oms.connected ? 'Connected' : 'Disconnected'}
          </div>
        </div>
      </header>

      <div className="flex flex-col lg:flex-row gap-0">
        {/* Left panel: Order Book + Order Entry */}
        <div className="lg:w-1/3 border-r border-gray-800">
          <OrderBookPanel
            instruments={oms.instruments}
            bookData={oms.bookData}
            subscribeBook={oms.subscribeBook}
            unsubscribeBook={oms.unsubscribeBook}
          />
          <OrderEntryPanel
            instruments={oms.instruments}
            accounts={oms.accounts}
            sendOrder={oms.sendOrder}
            connected={oms.connected}
          />
        </div>

        {/* Right panel: Blotters */}
        <div className="lg:w-2/3 flex flex-col">
          {/* Tab bar */}
          <div className="flex border-b border-gray-800">
            {(['orders', 'executions', 'positions'] as const).map(tab => (
              <button
                key={tab}
                onClick={() => setActiveTab(tab)}
                className={`px-6 py-3 text-sm font-medium capitalize transition-colors
                  ${activeTab === tab
                    ? 'text-blue-400 border-b-2 border-blue-400'
                    : 'text-gray-500 hover:text-gray-300'}`}
              >
                {tab}
                {tab === 'orders' && activeOrderCount > 0 && (
                  <span className="ml-2 text-xs bg-gray-800 px-2 py-0.5 rounded-full">
                    {activeOrderCount}
                  </span>
                )}
                {tab === 'executions' && oms.executions.length > 0 && (
                  <span className="ml-2 text-xs bg-gray-800 px-2 py-0.5 rounded-full">
                    {oms.executions.length}
                  </span>
                )}
              </button>
            ))}
          </div>

          {/* Tab content */}
          <div className="flex-1 overflow-auto">
            {activeTab === 'orders' && (
              <OrderBlotter
                orders={oms.orders}
                cancelOrder={oms.cancelOrder}
                replaceOrder={oms.replaceOrder}
              />
            )}
            {activeTab === 'executions' && (
              <ExecutionBlotter
                executions={oms.executions}
                orders={oms.orders}
              />
            )}
            {activeTab === 'positions' && (
              <PositionSummary
                executions={oms.executions}
                orders={oms.orders}
              />
            )}
          </div>
        </div>
      </div>
    </div>
  );
}
