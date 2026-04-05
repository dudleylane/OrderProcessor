import React, { useState } from 'react';
import type { AlertsState } from '../hooks/useAlerts';
import type { AlertRule, AlertSeverity } from '../types/alerts';

interface Props {
  alerts: AlertsState;
}

const SEVERITY_COLORS: Record<AlertSeverity, string> = {
  INFO: 'text-blue-400',
  WARNING: 'text-yellow-400',
  CRITICAL: 'text-red-400',
};

const SEVERITY_BG: Record<AlertSeverity, string> = {
  INFO: 'bg-blue-900/30 border-blue-800',
  WARNING: 'bg-yellow-900/30 border-yellow-800',
  CRITICAL: 'bg-red-900/30 border-red-800',
};

const METRIC_OPTIONS = [
  { value: 'queueDepth', label: 'Queue Depth' },
  { value: 'poolCacheMisses', label: 'Pool Cache Misses' },
  { value: 'activeOrders', label: 'Active Orders' },
  { value: 'activeSessions', label: 'Active Sessions' },
  { value: 'eventsProcessed', label: 'Events Processed' },
  { value: 'transactionsProcessed', label: 'Transactions Processed' },
];

export function AlertsPanel({ alerts }: Props) {
  const [showAddRule, setShowAddRule] = useState(false);
  const [newRule, setNewRule] = useState({
    name: '',
    metric: 'queueDepth',
    operator: '>' as AlertRule['operator'],
    threshold: 0,
    severity: 'WARNING' as AlertSeverity,
    useDelta: false,
  });

  const handleAddRule = () => {
    if (!newRule.name.trim()) return;
    alerts.addRule({ ...newRule, enabled: true });
    setNewRule({ name: '', metric: 'queueDepth', operator: '>', threshold: 0, severity: 'WARNING', useDelta: false });
    setShowAddRule(false);
  };

  return (
    <div className="p-4 space-y-6">
      {/* Alert rules */}
      <div className="bg-gray-900 rounded-lg border border-gray-800">
        <div className="flex items-center justify-between p-4 border-b border-gray-800">
          <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider">
            Alert Rules
          </div>
          <div className="flex gap-2">
            <button
              onClick={alerts.resetRules}
              className="px-3 py-1 text-xs text-gray-400 hover:text-gray-200 border border-gray-700 rounded transition-colors"
            >
              Reset Defaults
            </button>
            <button
              onClick={() => setShowAddRule(!showAddRule)}
              className="px-3 py-1 text-xs text-blue-400 hover:text-blue-300 border border-blue-800 rounded transition-colors"
            >
              {showAddRule ? 'Cancel' : 'Add Rule'}
            </button>
          </div>
        </div>

        {/* Add rule form */}
        {showAddRule && (
          <div className="p-4 border-b border-gray-800 bg-gray-950/50">
            <div className="grid grid-cols-2 md:grid-cols-6 gap-3 items-end">
              <div>
                <label className="text-xs text-gray-500 block mb-1">Name</label>
                <input
                  type="text"
                  value={newRule.name}
                  onChange={e => setNewRule(r => ({ ...r, name: e.target.value }))}
                  className="w-full bg-gray-800 border border-gray-700 rounded px-2 py-1 text-sm text-gray-100"
                  placeholder="Rule name"
                />
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Metric</label>
                <select
                  value={newRule.metric}
                  onChange={e => setNewRule(r => ({ ...r, metric: e.target.value }))}
                  className="w-full bg-gray-800 border border-gray-700 rounded px-2 py-1 text-sm text-gray-100"
                >
                  {METRIC_OPTIONS.map(o => <option key={o.value} value={o.value}>{o.label}</option>)}
                </select>
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Operator</label>
                <select
                  value={newRule.operator}
                  onChange={e => setNewRule(r => ({ ...r, operator: e.target.value as AlertRule['operator'] }))}
                  className="w-full bg-gray-800 border border-gray-700 rounded px-2 py-1 text-sm text-gray-100"
                >
                  {['>', '<', '>=', '<=', '=='].map(op => <option key={op} value={op}>{op}</option>)}
                </select>
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Threshold</label>
                <input
                  type="number"
                  value={newRule.threshold}
                  onChange={e => setNewRule(r => ({ ...r, threshold: Number(e.target.value) }))}
                  className="w-full bg-gray-800 border border-gray-700 rounded px-2 py-1 text-sm text-gray-100"
                />
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Severity</label>
                <select
                  value={newRule.severity}
                  onChange={e => setNewRule(r => ({ ...r, severity: e.target.value as AlertSeverity }))}
                  className="w-full bg-gray-800 border border-gray-700 rounded px-2 py-1 text-sm text-gray-100"
                >
                  <option value="INFO">Info</option>
                  <option value="WARNING">Warning</option>
                  <option value="CRITICAL">Critical</option>
                </select>
              </div>
              <div>
                <button
                  onClick={handleAddRule}
                  className="w-full px-3 py-1 text-sm bg-blue-600 hover:bg-blue-500 text-white rounded transition-colors"
                >
                  Add
                </button>
              </div>
            </div>
            <label className="flex items-center gap-2 mt-2 text-xs text-gray-400">
              <input
                type="checkbox"
                checked={newRule.useDelta}
                onChange={e => setNewRule(r => ({ ...r, useDelta: e.target.checked }))}
                className="rounded"
              />
              Compare rate of change (delta) instead of absolute value
            </label>
          </div>
        )}

        {/* Rules table */}
        <div className="divide-y divide-gray-800">
          {alerts.rules.map(rule => (
            <div key={rule.id} className="flex items-center justify-between px-4 py-3 hover:bg-gray-800/50">
              <div className="flex items-center gap-3">
                <button
                  onClick={() => alerts.updateRule(rule.id, { enabled: !rule.enabled })}
                  className={`w-8 h-4 rounded-full transition-colors relative ${
                    rule.enabled ? 'bg-blue-600' : 'bg-gray-700'}`}
                >
                  <span className={`absolute top-0.5 w-3 h-3 rounded-full bg-white transition-transform ${
                    rule.enabled ? 'left-4' : 'left-0.5'}`}
                  />
                </button>
                <span className={`text-sm ${rule.enabled ? 'text-gray-100' : 'text-gray-500'}`}>
                  {rule.name}
                </span>
                <span className={`text-xs ${SEVERITY_COLORS[rule.severity]}`}>
                  {rule.severity}
                </span>
              </div>
              <div className="flex items-center gap-3">
                <span className="text-xs text-gray-500 font-mono">
                  {rule.metric} {rule.operator} {rule.threshold}
                  {rule.useDelta && ' /sec'}
                </span>
                <button
                  onClick={() => alerts.removeRule(rule.id)}
                  className="text-xs text-gray-600 hover:text-red-400 transition-colors"
                >
                  Remove
                </button>
              </div>
            </div>
          ))}
          {alerts.rules.length === 0 && (
            <div className="p-4 text-sm text-gray-500 text-center">No alert rules configured</div>
          )}
        </div>
      </div>

      {/* Alert history */}
      <div className="bg-gray-900 rounded-lg border border-gray-800">
        <div className="flex items-center justify-between p-4 border-b border-gray-800">
          <div className="text-sm font-semibold text-gray-400 uppercase tracking-wider">
            Alert History
            {alerts.unacknowledgedCount > 0 && (
              <span className="ml-2 text-xs bg-red-900 text-red-200 px-2 py-0.5 rounded-full">
                {alerts.unacknowledgedCount} new
              </span>
            )}
          </div>
          {alerts.unacknowledgedCount > 0 && (
            <button
              onClick={alerts.acknowledgeAll}
              className="px-3 py-1 text-xs text-gray-400 hover:text-gray-200 border border-gray-700 rounded transition-colors"
            >
              Acknowledge All
            </button>
          )}
        </div>
        <div className="max-h-96 overflow-y-auto divide-y divide-gray-800/50">
          {alerts.alerts.map(alert => (
            <div
              key={alert.id}
              className={`flex items-center justify-between px-4 py-3 ${
                alert.acknowledged ? 'opacity-50' : ''} ${SEVERITY_BG[alert.severity]} border-l-2`}
            >
              <div className="flex items-center gap-3">
                <span className={`text-xs font-medium ${SEVERITY_COLORS[alert.severity]}`}>
                  {alert.severity}
                </span>
                <span className="text-sm text-gray-100">{alert.message}</span>
              </div>
              <div className="flex items-center gap-3">
                <span className="text-xs text-gray-500">
                  {new Date(alert.timestamp).toLocaleTimeString('en-US', { hour12: false })}
                </span>
                {!alert.acknowledged && (
                  <button
                    onClick={() => alerts.acknowledgeAlert(alert.id)}
                    className="text-xs text-gray-500 hover:text-gray-300 transition-colors"
                  >
                    Ack
                  </button>
                )}
              </div>
            </div>
          ))}
          {alerts.alerts.length === 0 && (
            <div className="p-4 text-sm text-gray-500 text-center">No alerts</div>
          )}
        </div>
      </div>
    </div>
  );
}
