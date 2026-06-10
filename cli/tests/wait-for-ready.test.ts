import { describe, it, expect } from 'vitest';
import { waitForReady } from '../src/lib/wait-for-ready.js';
import { fakeResponse } from './helpers.js';

describe('waitForReady', () => {
  it('succeeds once the probe returns 2xx', async () => {
    let calls = 0;
    const fetchImpl = (async () => {
      calls += 1;
      if (calls < 3) {
        const err = new Error('refused');
        (err as Error & { cause?: unknown }).cause = { code: 'ECONNREFUSED' };
        throw err;
      }
      return fakeResponse({ ok: true, status: 200, body: '{}' });
    }) as unknown as typeof fetch;

    let clock = 0;
    const r = await waitForReady({
      url: 'http://localhost:5220',
      timeoutMs: 100000,
      intervalMs: 1000,
      fetchImpl,
      nowImpl: () => clock,
      sleepImpl: async (ms) => {
        clock += ms;
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind === 'success') expect(r.attempts).toBe(3);
  });

  it('fails on timeout with the last reason', async () => {
    const fetchImpl = (async () => {
      const err = new Error('refused');
      (err as Error & { cause?: unknown }).cause = { code: 'ECONNREFUSED' };
      throw err;
    }) as unknown as typeof fetch;

    let clock = 0;
    const r = await waitForReady({
      url: 'http://localhost:5220',
      timeoutMs: 5000,
      intervalMs: 1000,
      fetchImpl,
      nowImpl: () => clock,
      sleepImpl: async (ms) => {
        clock += ms;
      },
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') {
      expect(r.lastReason).toBe('connection refused');
      expect(r.attempts).toBeGreaterThan(0);
    }
  });
});
