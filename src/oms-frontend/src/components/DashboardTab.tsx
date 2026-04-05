import React, { useMemo } from 'react';
import {
  LineChart, Line, AreaChart, Area,
  XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer,
} from 'recharts';
import type { SystemMetrics } from '../types';

interface Props {
  metricsHistory: SystemMetrics[];
}

function MetricCard({ label, value, sub }: { label: string; value: string; sub?: string }) {
  return (
    <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
      <div className="text-xs text-gray-500 mb-1">{label}</div>
      <div className="text-xl font-semibold text-gray-100 font-mono">{value}</div>
      {sub && <div className="text-xs text-gray-500 mt-1">{sub}</div>}
    </div>
  );
}

export function DashboardTab({ metricsHistory }: Props) {
  const latest = metricsHistory[metricsHistory.length - 1];

  const chartData = useMemo(() => {
    return metricsHistory.map((m, i) => {
      const prev = i > 0 ? metricsHistory[i - 1] : m;
      return {
        time: new Date(m.timestamp).toLocaleTimeString('en-US', {
          hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit',
        }),
        eventsPerSec: Math.max(0, m.eventsProcessed - prev.eventsProcessed),
        transactionsPerSec: Math.max(0, m.transactionsProcessed - prev.transactionsProcessed),
        queueDepth: m.queueDepth,
      };
    });
  }, [metricsHistory]);

  const latestRate = chartData.length > 1 ? chartData[chartData.length - 1] : null;

  if (!latest) {
    return (
      <div className="p-8 text-gray-500 text-center">
        Waiting for metrics data...
      </div>
    );
  }

  const eventBusy = latest.totalEventProcessors - latest.availableEventProcessors;
  const transactBusy = latest.totalTransactProcessors - latest.availableTransactProcessors;

  return (
    <div className="p-4 space-y-6">
      {/* Summary cards */}
      <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-6 gap-3">
        <MetricCard label="Active Sessions" value={String(latest.activeSessions)} />
        <MetricCard label="Active Orders" value={String(latest.activeOrders)} />
        <MetricCard label="Queue Depth" value={String(latest.queueDepth)} />
        <MetricCard
          label="Pool Cache Misses"
          value={String(latest.poolCacheMisses)}
          sub={latest.poolCacheMisses > 0 ? 'Pool exhaustion detected' : 'Healthy'}
        />
        <MetricCard
          label="Events/sec"
          value={latestRate ? String(latestRate.eventsPerSec) : '—'}
        />
        <MetricCard
          label="Transactions/sec"
          value={latestRate ? String(latestRate.transactionsPerSec) : '—'}
        />
      </div>

      {/* Processor utilization */}
      <div className="grid grid-cols-1 md:grid-cols-2 gap-3">
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
          <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-2">
            Event Processors
          </div>
          <div className="flex items-center gap-3">
            <div className="flex-1 bg-gray-800 rounded-full h-3">
              <div
                className="bg-blue-500 h-3 rounded-full transition-all"
                style={{ width: latest.totalEventProcessors > 0
                  ? `${(eventBusy / latest.totalEventProcessors) * 100}%` : '0%' }}
              />
            </div>
            <span className="text-sm text-gray-400 font-mono">
              {eventBusy}/{latest.totalEventProcessors}
            </span>
          </div>
        </div>
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
          <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-2">
            Transaction Processors
          </div>
          <div className="flex items-center gap-3">
            <div className="flex-1 bg-gray-800 rounded-full h-3">
              <div
                className="bg-green-500 h-3 rounded-full transition-all"
                style={{ width: latest.totalTransactProcessors > 0
                  ? `${(transactBusy / latest.totalTransactProcessors) * 100}%` : '0%' }}
              />
            </div>
            <span className="text-sm text-gray-400 font-mono">
              {transactBusy}/{latest.totalTransactProcessors}
            </span>
          </div>
        </div>
      </div>

      {/* Charts */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        {/* Event throughput */}
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
          <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-3">
            Event Throughput (events/sec)
          </div>
          <ResponsiveContainer width="100%" height={200}>
            <LineChart data={chartData}>
              <CartesianGrid strokeDasharray="3 3" stroke="#374151" />
              <XAxis dataKey="time" tick={{ fill: '#9ca3af', fontSize: 10 }} interval="preserveStartEnd" />
              <YAxis tick={{ fill: '#9ca3af', fontSize: 10 }} allowDecimals={false} />
              <Tooltip
                contentStyle={{ backgroundColor: '#1f2937', border: '1px solid #374151', borderRadius: 8 }}
                labelStyle={{ color: '#9ca3af' }}
              />
              <Line type="monotone" dataKey="eventsPerSec" stroke="#60a5fa" dot={false} strokeWidth={2} />
            </LineChart>
          </ResponsiveContainer>
        </div>

        {/* Transaction throughput */}
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
          <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-3">
            Transaction Throughput (txns/sec)
          </div>
          <ResponsiveContainer width="100%" height={200}>
            <LineChart data={chartData}>
              <CartesianGrid strokeDasharray="3 3" stroke="#374151" />
              <XAxis dataKey="time" tick={{ fill: '#9ca3af', fontSize: 10 }} interval="preserveStartEnd" />
              <YAxis tick={{ fill: '#9ca3af', fontSize: 10 }} allowDecimals={false} />
              <Tooltip
                contentStyle={{ backgroundColor: '#1f2937', border: '1px solid #374151', borderRadius: 8 }}
                labelStyle={{ color: '#9ca3af' }}
              />
              <Line type="monotone" dataKey="transactionsPerSec" stroke="#34d399" dot={false} strokeWidth={2} />
            </LineChart>
          </ResponsiveContainer>
        </div>

        {/* Queue depth */}
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800 lg:col-span-2">
          <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-3">
            Event Queue Depth
          </div>
          <ResponsiveContainer width="100%" height={200}>
            <AreaChart data={chartData}>
              <CartesianGrid strokeDasharray="3 3" stroke="#374151" />
              <XAxis dataKey="time" tick={{ fill: '#9ca3af', fontSize: 10 }} interval="preserveStartEnd" />
              <YAxis tick={{ fill: '#9ca3af', fontSize: 10 }} allowDecimals={false} />
              <Tooltip
                contentStyle={{ backgroundColor: '#1f2937', border: '1px solid #374151', borderRadius: 8 }}
                labelStyle={{ color: '#9ca3af' }}
              />
              <Area type="monotone" dataKey="queueDepth" stroke="#f59e0b" fill="#78350f" fillOpacity={0.4} strokeWidth={2} />
            </AreaChart>
          </ResponsiveContainer>
        </div>
      </div>
    </div>
  );
}
