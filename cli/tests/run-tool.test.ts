import { describe, it, expect } from 'vitest';
import { runTool, runSystemTool } from '../src/lib/run-tool.js';
import { fakeResponse } from './helpers.js';

describe('runTool', () => {
  it('POSTs to /api/tools/<name> against an explicit url and returns parsed data', async () => {
    let calledUrl = '';
    let calledBody = '';
    const fetchImpl = (async (url: string, init: RequestInit) => {
      calledUrl = String(url);
      calledBody = String(init.body);
      return fakeResponse({ ok: true, status: 200, body: JSON.stringify({ status: 'success', structured: { pong: true } }) });
    }) as unknown as typeof fetch;

    const r = await runTool({ toolName: 'ping', url: 'http://localhost:5220/', input: { x: 1 }, fetchImpl });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(calledUrl).toBe('http://localhost:5220/api/tools/ping');
    expect(calledBody).toBe('{"x":1}');
    expect((r.data as { structured: { pong: boolean } }).structured.pong).toBe(true);
  });

  it('run-system-tool hits /api/system-tools/<name>', async () => {
    let calledUrl = '';
    const fetchImpl = (async (url: string) => {
      calledUrl = String(url);
      return fakeResponse({ ok: true, status: 200, body: '{}' });
    }) as unknown as typeof fetch;
    await runSystemTool({ toolName: 'ping', url: 'http://h', fetchImpl });
    expect(calledUrl).toBe('http://h/api/system-tools/ping');
  });

  it('surfaces an http-error with status + body', async () => {
    const fetchImpl = (async () =>
      fakeResponse({ ok: false, status: 500, statusText: 'Server Error', body: '{"error":"boom"}' })) as unknown as typeof fetch;
    const r = await runTool({ toolName: 'ping', url: 'http://h', fetchImpl });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') {
      expect(r.reason).toBe('http-error');
      expect(r.httpStatus).toBe(500);
    }
  });

  it('rejects a missing toolName / missing url+projectDir without throwing', async () => {
    const r1 = await runTool({ toolName: '', url: 'http://h' });
    expect(r1.kind).toBe('failure');
    if (r1.kind === 'failure') expect(r1.reason).toBe('invalid-input');
    const r2 = await runTool({ toolName: 'ping' });
    expect(r2.kind).toBe('failure');
    if (r2.kind === 'failure') expect(r2.reason).toBe('invalid-input');
  });

  it('rejects malformed JSON input', async () => {
    const r = await runTool({ toolName: 'ping', url: 'http://h', input: '{not json' });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('invalid-input');
  });

  it('classifies a connection-refused cause', async () => {
    const fetchImpl = (async () => {
      const err = new Error('fetch failed');
      (err as Error & { cause?: unknown }).cause = { code: 'ECONNREFUSED' };
      throw err;
    }) as unknown as typeof fetch;
    const r = await runTool({ toolName: 'ping', url: 'http://h', fetchImpl });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.reason).toBe('connection-refused');
  });
});
