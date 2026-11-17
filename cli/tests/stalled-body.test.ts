import { createServer, type Server, type ServerResponse } from 'node:http';
import type { AddressInfo } from 'node:net';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { runTool } from '../src/lib/run-tool.js';
import { probePing } from '../src/utils/probe.js';

// A server that sends the headers and the first bytes of the body, then stalls without closing. The call's deadline
// and the caller's abort signal must still cut it off: a timeout reports `timeout`, a cancellation `aborted`.
describe('a body that stalls after the headers', () => {
  let server: Server;
  let base: string;
  const open = new Set<ServerResponse>();
  // Resolves when the server has sent the headers of the next request: a test aborts only once the body phase began.
  let headersSent: () => void = () => {};
  const nextHeaders = (): Promise<void> => new Promise((resolve) => { headersSent = resolve; });

  beforeAll(async () => {
    server = createServer((req, res) => {
      req.resume();
      req.on('end', () => {
        open.add(res);
        res.writeHead(200, { 'content-type': 'application/json', 'content-length': '200' });
        res.write('{"status":"succ', () => headersSent());
      });
    });
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
    base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  });
  afterAll(async () => {
    for (const res of open) res.socket?.destroy();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  });

  it('run-tool times out with reason "timeout"', async () => {
    const headers = nextHeaders();
    const pending = runTool({ toolName: 'stall', url: base, input: {}, timeoutMs: 1_000 });
    await headers; // the deadline passes during the body, not before the headers
    const result = await pending;
    expect(result.kind === 'failure' && result.reason).toBe('timeout');
  }, 5_000);

  it('run-tool: a caller abort during the body reports reason "aborted"', async () => {
    const ac = new AbortController();
    const headers = nextHeaders();
    const pending = runTool({ toolName: 'stall', url: base, input: {}, timeoutMs: 60_000, signal: ac.signal });
    await headers;
    ac.abort();
    const result = await pending;
    expect(result.kind === 'failure' && result.reason).toBe('aborted');
  }, 5_000);

  it('the readiness probe times out instead of hanging', async () => {
    const headers = nextHeaders();
    const pending = probePing(base, { timeoutMs: 1_000 });
    await headers;
    const result = await pending;
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.reason).toContain('timed out');
  }, 5_000);
});
