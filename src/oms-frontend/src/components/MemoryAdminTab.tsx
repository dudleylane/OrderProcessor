import React, { useMemo } from 'react';
import {
  LineChart, Line,
  XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer,
} from 'recharts';
import type { SystemMetrics } from '../types';

interface Props {
  metricsHistory: SystemMetrics[];
}

const ALLOC_MODES: Record<number, string> = {
  0: 'Default (heap)',
  1: 'Huge Pages (2MB)',
  2: 'NUMA Local',
};

export function MemoryAdminTab({ metricsHistory }: Props) {
  const latest = metricsHistory[metricsHistory.length - 1];

  const chartData = useMemo(() => {
    return metricsHistory.map((m, i) => {
      const prev = i > 0 ? metricsHistory[i - 1] : m;
      return {
        time: new Date(m.timestamp).toLocaleTimeString('en-US', {
          hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit',
        }),
        cacheMissRate: Math.max(0, m.poolCacheMisses - prev.poolCacheMisses),
        totalCacheMisses: m.poolCacheMisses,
      };
    });
  }, [metricsHistory]);

  // Health: count misses in last 60 data points
  const recentMisses = useMemo(() => {
    const recent = chartData.slice(-60);
    return recent.reduce((sum, d) => sum + d.cacheMissRate, 0);
  }, [chartData]);

  const healthColor = recentMisses === 0 ? 'text-green-400' : recentMisses <= 5 ? 'text-yellow-400' : 'text-red-400';
  const healthLabel = recentMisses === 0 ? 'Healthy' : recentMisses <= 5 ? 'Warning' : 'Critical';
  const healthDot = recentMisses === 0 ? 'bg-green-400' : recentMisses <= 5 ? 'bg-yellow-400' : 'bg-red-400';

  if (!latest) {
    return (
      <div className="p-8 text-gray-500 text-center">
        Waiting for metrics data...
      </div>
    );
  }

  return (
    <div className="p-4 space-y-6">
      {/* Pool health indicator */}
      <div className="bg-gray-900 rounded-lg p-4 border border-gray-800 flex items-center justify-between">
        <div>
          <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider">
            TransactionScope Pool Health
          </div>
          <div className="text-xs text-gray-500 mt-1">
            {recentMisses} cache miss(es) in last 60 seconds
          </div>
        </div>
        <div className={`flex items-center gap-2 text-sm font-medium ${healthColor}`}>
          <span className={`w-3 h-3 rounded-full ${healthDot}`} />
          {healthLabel}
        </div>
      </div>

      {/* Pool overview */}
      <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
          <div className="text-xs text-gray-500 mb-1">Pool Size</div>
          <div className="text-xl font-semibold text-gray-100 font-mono">{latest.poolSize}</div>
          <div className="text-xs text-gray-500 mt-1">pre-allocated slots</div>
        </div>
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
          <div className="text-xs text-gray-500 mb-1">Arena Size</div>
          <div className="text-xl font-semibold text-gray-100 font-mono">{latest.poolArenaSize}</div>
          <div className="text-xs text-gray-500 mt-1">bytes per scope</div>
        </div>
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
          <div className="text-xs text-gray-500 mb-1">Total Cache Misses</div>
          <div className={`text-xl font-semibold font-mono ${latest.poolCacheMisses > 0 ? 'text-red-400' : 'text-gray-100'}`}>
            {latest.poolCacheMisses}
          </div>
          <div className="text-xs text-gray-500 mt-1">cumulative heap fallbacks</div>
        </div>
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
          <div className="text-xs text-gray-500 mb-1">Allocation Mode</div>
          <div className="text-lg font-semibold text-gray-100">
            Default (heap)
          </div>
        </div>
      </div>

      {/* Cache miss rate chart */}
      <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
        <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-3">
          Cache Miss Rate (misses/sec)
        </div>
        <ResponsiveContainer width="100%" height={250}>
          <LineChart data={chartData}>
            <CartesianGrid strokeDasharray="3 3" stroke="#374151" />
            <XAxis dataKey="time" tick={{ fill: '#9ca3af', fontSize: 10 }} interval="preserveStartEnd" />
            <YAxis tick={{ fill: '#9ca3af', fontSize: 10 }} allowDecimals={false} />
            <Tooltip
              contentStyle={{ backgroundColor: '#1f2937', border: '1px solid #374151', borderRadius: 8 }}
              labelStyle={{ color: '#9ca3af' }}
            />
            <Line type="monotone" dataKey="cacheMissRate" stroke="#ef4444" dot={false} strokeWidth={2} />
          </LineChart>
        </ResponsiveContainer>
      </div>

      {/* Arena details */}
      <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
        <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider mb-3">
          Arena Allocator Details
        </div>
        <div className="grid grid-cols-1 md:grid-cols-2 gap-4 text-sm">
          <div>
            <table className="w-full">
              <tbody>
                <tr className="border-b border-gray-800">
                  <td className="py-2 text-gray-500">Arena capacity</td>
                  <td className="py-2 text-gray-100 font-mono text-right">{latest.poolArenaSize} bytes</td>
                </tr>
                <tr className="border-b border-gray-800">
                  <td className="py-2 text-gray-500">Typical operation size</td>
                  <td className="py-2 text-gray-100 font-mono text-right">~160 bytes</td>
                </tr>
                <tr className="border-b border-gray-800">
                  <td className="py-2 text-gray-500">Operations per scope</td>
                  <td className="py-2 text-gray-100 font-mono text-right">~12</td>
                </tr>
                <tr>
                  <td className="py-2 text-gray-500">Overflow behavior</td>
                  <td className="py-2 text-gray-100 text-right">Heap fallback</td>
                </tr>
              </tbody>
            </table>
          </div>
          <div>
            <table className="w-full">
              <tbody>
                <tr className="border-b border-gray-800">
                  <td className="py-2 text-gray-500">Pool acquire</td>
                  <td className="py-2 text-gray-100 text-right">CAS lock-free</td>
                </tr>
                <tr className="border-b border-gray-800">
                  <td className="py-2 text-gray-500">Max CAS backoff</td>
                  <td className="py-2 text-gray-100 font-mono text-right">16 pauses</td>
                </tr>
                <tr className="border-b border-gray-800">
                  <td className="py-2 text-gray-500">Thread-local scope</td>
                  <td className="py-2 text-gray-100 text-right">ScopeArenaGuard</td>
                </tr>
                <tr>
                  <td className="py-2 text-gray-500">False-sharing prevention</td>
                  <td className="py-2 text-gray-100 text-right">alignas(64)</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}
