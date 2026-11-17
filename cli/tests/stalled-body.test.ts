import { createServer, type Server, type ServerResponse } from 'node:http';
import type { AddressInfo } from 'node:net';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { runTool } from '../src/lib/run-tool.js';

// A server that sends the headers and the first bytes of the body, then stalls without closing. The call's deadline
// and the caller's abort signal must still cut it off: a timeout reports `timeout`, a cancellation `aborted`.
describe('runTool on a body that stalls after the headers', () => {
  let server: Server;
  let base: string;
  const open = new Set<ServerResponse>();

  beforeAll(async () => {
    server = createServer((req, res) => {
      req.resume();
      req.on('end', () => {
        open.add(res);
        res.writeHead(200, { 'content-type': 'application/json', 'content-length': '200' });
        res.write('{"status":"succ');
      });
    });
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
    base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  });
  afterAll(async () => {
    for (const res of open) res.socket?.destroy();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  });

  it('times out with reason "timeout"', async () => {
    const started = Date.now();
    const result = await runTool({ toolName: 'stall', url: base, input: {}, timeoutMs: 300 });
    expect(result.kind === 'failure' && result.reason).toBe('timeout');
    expect(Date.now() - started).toBeLessThan(3_000);
  }, 5_000);

  it('a caller abort during the body reports reason "aborted"', async () => {
    const ac = new AbortController();
    const pending = runTool({ toolName: 'stall', url: base, input: {}, timeoutMs: 60_000, signal: ac.signal });
    setTimeout(() => ac.abort(), 300);
    const result = await pending;
    expect(result.kind === 'failure' && result.reason).toBe('aborted');
  }, 5_000);
});
