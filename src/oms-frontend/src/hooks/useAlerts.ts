import { useState, useEffect, useCallback, useRef } from 'react';
import type { SystemMetrics } from '../types';
import type { AlertRule, Alert, AlertSeverity } from '../types/alerts';

const STORAGE_KEY = 'oms-alert-rules';
const MAX_ALERTS = 500;
const COOLDOWN_MS = 30_000; // 30 seconds between repeated alerts per rule

const DEFAULT_RULES: AlertRule[] = [
  {
    id: 'default-cache-miss',
    name: 'Pool Cache Misses',
    metric: 'poolCacheMisses',
    operator: '>',
    threshold: 0,
    severity: 'WARNING',
    enabled: true,
    useDelta: true,
  },
  {
    id: 'default-queue-warn',
    name: 'Queue Depth High',
    metric: 'queueDepth',
    operator: '>',
    threshold: 100,
    severity: 'WARNING',
    enabled: true,
    useDelta: false,
  },
  {
    id: 'default-queue-crit',
    name: 'Queue Depth Critical',
    metric: 'queueDepth',
    operator: '>',
    threshold: 500,
    severity: 'CRITICAL',
    enabled: true,
    useDelta: false,
  },
];

function loadRules(): AlertRule[] {
  try {
    const stored = localStorage.getItem(STORAGE_KEY);
    if (stored) return JSON.parse(stored);
  } catch { /* ignore */ }
  return DEFAULT_RULES;
}

function saveRules(rules: AlertRule[]) {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(rules));
}

function evaluateRule(rule: AlertRule, current: SystemMetrics, prev: SystemMetrics | null): { triggered: boolean; value: number } {
  let value: number;
  if (rule.useDelta && prev) {
    value = (current[rule.metric as keyof SystemMetrics] as number) - (prev[rule.metric as keyof SystemMetrics] as number);
  } else {
    value = current[rule.metric as keyof SystemMetrics] as number;
  }

  let triggered = false;
  switch (rule.operator) {
    case '>': triggered = value > rule.threshold; break;
    case '<': triggered = value < rule.threshold; break;
    case '>=': triggered = value >= rule.threshold; break;
    case '<=': triggered = value <= rule.threshold; break;
    case '==': triggered = value === rule.threshold; break;
  }
  return { triggered, value };
}

export interface AlertsState {
  rules: AlertRule[];
  alerts: Alert[];
  unacknowledgedCount: number;
  criticalCount: number;
  addRule: (rule: Omit<AlertRule, 'id'>) => void;
  removeRule: (id: string) => void;
  updateRule: (id: string, updates: Partial<AlertRule>) => void;
  acknowledgeAlert: (id: string) => void;
  acknowledgeAll: () => void;
  resetRules: () => void;
}

export function useAlerts(metricsHistory: SystemMetrics[]): AlertsState {
  const [rules, setRules] = useState<AlertRule[]>(loadRules);
  const [alerts, setAlerts] = useState<Alert[]>([]);
  const cooldownMap = useRef<Map<string, number>>(new Map());
  const lastEvaluatedLen = useRef(0);

  // Persist rules
  useEffect(() => { saveRules(rules); }, [rules]);

  // Evaluate rules on new metrics
  useEffect(() => {
    if (metricsHistory.length <= lastEvaluatedLen.current) return;
    lastEvaluatedLen.current = metricsHistory.length;

    const current = metricsHistory[metricsHistory.length - 1];
    const prev = metricsHistory.length > 1 ? metricsHistory[metricsHistory.length - 2] : null;
    if (!current) return;

    const now = Date.now();
    const newAlerts: Alert[] = [];

    for (const rule of rules) {
      if (!rule.enabled) continue;

      const lastFired = cooldownMap.current.get(rule.id) ?? 0;
      if (now - lastFired < COOLDOWN_MS) continue;

      const { triggered, value } = evaluateRule(rule, current, prev);
      if (triggered) {
        cooldownMap.current.set(rule.id, now);
        newAlerts.push({
          id: `${rule.id}-${now}`,
          ruleId: rule.id,
          ruleName: rule.name,
          severity: rule.severity,
          message: `${rule.name}: ${value} ${rule.operator} ${rule.threshold}`,
          value,
          threshold: rule.threshold,
          timestamp: now,
          acknowledged: false,
        });
      }
    }

    if (newAlerts.length > 0) {
      setAlerts(prev => [...newAlerts, ...prev].slice(0, MAX_ALERTS));
    }
  }, [metricsHistory, rules]);

  const addRule = useCallback((rule: Omit<AlertRule, 'id'>) => {
    setRules(prev => [...prev, { ...rule, id: `rule-${Date.now()}` }]);
  }, []);

  const removeRule = useCallback((id: string) => {
    setRules(prev => prev.filter(r => r.id !== id));
  }, []);

  const updateRule = useCallback((id: string, updates: Partial<AlertRule>) => {
    setRules(prev => prev.map(r => r.id === id ? { ...r, ...updates } : r));
  }, []);

  const acknowledgeAlert = useCallback((id: string) => {
    setAlerts(prev => prev.map(a => a.id === id ? { ...a, acknowledged: true } : a));
  }, []);

  const acknowledgeAll = useCallback(() => {
    setAlerts(prev => prev.map(a => ({ ...a, acknowledged: true })));
  }, []);

  const resetRules = useCallback(() => {
    setRules(DEFAULT_RULES);
  }, []);

  const unacknowledgedCount = alerts.filter(a => !a.acknowledged).length;
  const criticalCount = alerts.filter(a => !a.acknowledged && a.severity === 'CRITICAL').length;

  return {
    rules, alerts, unacknowledgedCount, criticalCount,
    addRule, removeRule, updateRule, acknowledgeAlert, acknowledgeAll, resetRules,
  };
}
