import { StrictMode } from 'react';
import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { SystemMetrics } from '../types';
import type { Alert, AlertRule } from '../types/alerts';
import { sample } from '../test/fixtures';
import { useAlerts } from './useAlerts';

// #520's cases (#548). Each renders the hook under StrictMode and feeds it new history arrays, as the reducer does, with
// the newest 300 samples kept. Only Date is faked, so that a case can move past a rule's 30-second cooldown.

const HISTORY_CAP = 300; // useOmsWebSocket's MAX_METRICS_HISTORY
const FAILED_RULE = 'default-transaction-failed';

/** The history once the reducer has taken samples: the newest HISTORY_CAP kept */
function withSamples(history: SystemMetrics[], samples: SystemMetrics[]): SystemMetrics[] {
  return [...history, ...samples].slice(-HISTORY_CAP);
}

function renderAlerts() {
  return renderHook(({ history }: { history: SystemMetrics[] }) => useAlerts(history), {
    initialProps: { history: [] as SystemMetrics[] },
    wrapper: StrictMode,
  });
}

function raisedBy(alerts: Alert[], ruleId: string): Alert[] {
  return alerts.filter(a => a.ruleId === ruleId);
}

/** Moves the faked clock on */
function wait(ms: number) {
  vi.setSystemTime(Date.now() + ms);
}

describe('useAlerts (#520)', () => {
  beforeEach(() => {
    localStorage.clear();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(new Date('2026-11-22T12:00:00Z'));
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('raises one alert for a failure first counted after the history is full, and none for the samples after it', () => {
    const hook = renderAlerts();
    let history: SystemMetrics[] = [];
    for (let n = 1; n <= 400; n++) {
      wait(1000);
      history = withSamples(history, [sample({ transactionsFailed: n >= 350 ? 1 : 0 })]);
      hook.rerender({ history });
    }
    const raised = raisedBy(hook.result.current.alerts, FAILED_RULE);
    expect(raised).toHaveLength(1);
    expect(raised[0].severity).toBe('CRITICAL');
    expect(raised[0].value).toBe(1);
  });

  it('raises the alert on the step into the first of two samples a render brings, where the newest is a step of 0', () => {
    const hook = renderAlerts();
    let history = withSamples([], [sample({ transactionsFailed: 0 })]);
    hook.rerender({ history });
    wait(1000);
    history = withSamples(history, [sample({ transactionsFailed: 1 }), sample({ transactionsFailed: 1 })]);
    hook.rerender({ history });
    const raised = raisedBy(hook.result.current.alerts, FAILED_RULE);
    expect(raised).toHaveLength(1);
    expect(raised[0].value).toBe(1);
  });

  it('raises one alert for two failing samples in one render: the cooldown holds the second', () => {
    const hook = renderAlerts();
    let history = withSamples([], [sample({ transactionsFailed: 0 })]);
    hook.rerender({ history });
    wait(1000);
    history = withSamples(history, [sample({ transactionsFailed: 1 }), sample({ transactionsFailed: 2 })]);
    hook.rerender({ history });
    const raised = raisedBy(hook.result.current.alerts, FAILED_RULE);
    expect(raised).toHaveLength(1);
    expect(raised[0].value).toBe(1);
  });

  it('compares the first sample of a render with the last one evaluated, though that one has left the history', () => {
    const hook = renderAlerts();
    // The history full at a count of 5: its first sample, with nothing before it, raises an alert of its own
    let history = withSamples([], Array.from({ length: HISTORY_CAP }, () => sample({ transactionsFailed: 5 })));
    hook.rerender({ history });
    const before = raisedBy(hook.result.current.alerts, FAILED_RULE).length;
    wait(31_000);
    // 310 samples at a count of 6: the last sample evaluated is no longer in the history
    history = withSamples(history, Array.from({ length: 310 }, () => sample({ transactionsFailed: 6 })));
    hook.rerender({ history });
    const raised = raisedBy(hook.result.current.alerts, FAILED_RULE);
    expect(raised).toHaveLength(before + 1);
    expect(raised[0].value).toBe(1); // the step from 5, not the count of 6
  });

  it('evaluates nothing again when a rule changes with no new sample, even past the cooldown', () => {
    const hook = renderAlerts();
    const history = withSamples([], [sample({ transactionsFailed: 0 }), sample({ transactionsFailed: 1 })]);
    hook.rerender({ history });
    expect(raisedBy(hook.result.current.alerts, FAILED_RULE)).toHaveLength(1);
    wait(31_000);
    act(() => {
      hook.result.current.updateRule('default-queue-warn', { threshold: 150 });
    });
    expect(raisedBy(hook.result.current.alerts, FAILED_RULE)).toHaveLength(1);
  });

  it('takes a restarted server\'s first sample as having no predecessor, though the server before it counted more (#677)', () => {
    const hook = renderAlerts();
    // The server before the restart had counted 3 failures: its first sample raises an alert of its own
    let history = withSamples([], [sample({ serverStart: 1000, transactionsFailed: 3 })]);
    hook.rerender({ history });
    expect(raisedBy(hook.result.current.alerts, FAILED_RULE)).toHaveLength(1);
    wait(31_000);
    // The restarted server has counted 1 failure since its start: compared with the 3 before, it would be a step of -2
    history = withSamples(history, [sample({ serverStart: 2000, transactionsFailed: 1 })]);
    hook.rerender({ history });
    const raised = raisedBy(hook.result.current.alerts, FAILED_RULE);
    expect(raised).toHaveLength(2);
    expect(raised[0].value).toBe(1);
  });

  it('compares a sample from the same start with the one before it, as after a reconnect to the same server (#677)', () => {
    const hook = renderAlerts();
    let history = withSamples([], [sample({ serverStart: 1000, transactionsFailed: 3 })]);
    hook.rerender({ history });
    wait(31_000);
    history = withSamples(history, [sample({ serverStart: 1000, transactionsFailed: 3 })]);
    hook.rerender({ history });
    expect(raisedBy(hook.result.current.alerts, FAILED_RULE)).toHaveLength(1); // the first sample's own
    wait(31_000);
    history = withSamples(history, [sample({ serverStart: 1000, transactionsFailed: 4 })]);
    hook.rerender({ history });
    const raised = raisedBy(hook.result.current.alerts, FAILED_RULE);
    expect(raised).toHaveLength(2);
    expect(raised[0].value).toBe(1); // the step from 3, not the count of 4
  });

  it('sees two rises of 3 in one render as two rises of 3, not one of 6, and alerts on a later rise of 6', () => {
    const rising: AlertRule = {
      id: 'queue-rising',
      name: 'Queue Rising',
      metric: 'queueDepth',
      operator: '>',
      threshold: 5,
      severity: 'WARNING',
      enabled: true,
      useDelta: true,
    };
    localStorage.setItem('oms-alert-rules', JSON.stringify([rising]));
    const hook = renderAlerts();
    let history = withSamples([], [sample({ queueDepth: 0 })]);
    hook.rerender({ history });
    wait(1000);
    history = withSamples(history, [sample({ queueDepth: 3 }), sample({ queueDepth: 6 })]);
    hook.rerender({ history });
    expect(raisedBy(hook.result.current.alerts, 'queue-rising')).toHaveLength(0);
    wait(1000);
    history = withSamples(history, [sample({ queueDepth: 12 })]);
    hook.rerender({ history });
    const raised = raisedBy(hook.result.current.alerts, 'queue-rising');
    expect(raised).toHaveLength(1);
    expect(raised[0].value).toBe(6);
  });
});
