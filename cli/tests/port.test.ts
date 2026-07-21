import { describe, it, expect } from 'vitest';
import {
  generatePortFromDirectory,
  normalizeProjectRoot,
  deriveProjectPin,
  tryGetExplicitPort,
  tryGetExplicitLoopbackPort,
  isAbsoluteLoopbackHost,
  resolveLocalBindPort,
} from '../src/utils/port.js';

describe('generatePortFromDirectory', () => {
  it('is deterministic and in the 20000-29999 range', () => {
    const a = generatePortFromDirectory('C:\\Projects\\MyGame');
    const b = generatePortFromDirectory('C:\\Projects\\MyGame');
    expect(a).toBe(b);
    expect(a).toBeGreaterThanOrEqual(20000);
    expect(a).toBeLessThanOrEqual(29999);
  });

  it('is case-insensitive (lowercases the path)', () => {
    expect(generatePortFromDirectory('/Foo/Bar')).toBe(generatePortFromDirectory('/foo/bar'));
  });

  it('differs for different directories', () => {
    expect(generatePortFromDirectory('/a')).not.toBe(generatePortFromDirectory('/b'));
  });

  it('trims trailing separators so /a/b and /a/b/ are the same project', () => {
    expect(generatePortFromDirectory('/a/b')).toBe(generatePortFromDirectory('/a/b/'));
    expect(generatePortFromDirectory('/a/b')).toBe(generatePortFromDirectory('/a/b///'));
  });

  it('does NOT convert separators (backslash and forward-slash forms differ)', () => {
    expect(generatePortFromDirectory('C:\\a')).not.toBe(generatePortFromDirectory('C:/a'));
  });
});

// Cross-language parity with the shared C# ProjectIdentity reference
// (McpPlugin/src/AgentConfig/ProjectIdentity.cs). The vectors are copied inline
// from ProjectIdentity.GoldenVectors.json (no runtime dependency on that file);
// the TS port MUST reproduce every `port` byte-for-byte.
describe('generatePortFromDirectory — ProjectIdentity golden-vector parity', () => {
  const VECTORS: ReadonlyArray<{ path: string; port: number; note: string }> = [
    { path: '/home/user/my-game', port: 23940, note: 'POSIX typical project path' },
    { path: '/home/user/my-game/', port: 23940, note: 'trailing slash trimmed → identical' },
    { path: '/home/USER/My-Game', port: 23940, note: 'case-folded → identical' },
    { path: 'C:\\Users\\user\\my-game', port: 29310, note: 'Windows backslash form' },
    { path: 'C:\\Users\\user\\my-game\\', port: 29310, note: 'trailing backslash trimmed → identical' },
    { path: 'C:/Users/user/my-game', port: 24298, note: 'forward-slash form DIFFERS (separators not normalized)' },
    { path: '/home/İstanbul/game', port: 25303, note: 'U+0130 — ToLowerInvariant leaves it unchanged' },
    { path: '/srv/games/space sim', port: 27816, note: 'path containing a space' },
  ];

  for (const v of VECTORS) {
    it(`derives ${v.port} for ${JSON.stringify(v.path)} (${v.note})`, () => {
      expect(generatePortFromDirectory(v.path)).toBe(v.port);
    });
  }

  // The U+0130 vector is the whole point of ToLowerInvariant vs a naive JS
  // toLowerCase(): a naive port lowercases İ → 'i' + U+0307 and computes port
  // 27751. Assert we produce the canonical 25303, not the naive value.
  it('special-cases U+0130 (İ) to match ToLowerInvariant, not naive toLowerCase()', () => {
    expect(generatePortFromDirectory('/home/İstanbul/game')).toBe(25303);
    expect(generatePortFromDirectory('/home/İstanbul/game')).not.toBe(27751);
    // The naive path a bad port would hash: 'İ'.toLowerCase() === 'i̇'.
    expect(normalizeProjectRoot('/home/İstanbul/game')).toContain('İ');
    expect(normalizeProjectRoot('/home/İstanbul/game')).not.toContain('̇');
  });
});

// The D14 routing pin shares the ProjectIdentity derivation with the port, so it
// is gated by the SAME golden vectors (ProjectIdentity.GoldenVectors.json `pin`
// field). `deriveProjectPin` MUST reproduce every `pin` byte-for-byte.
describe('deriveProjectPin — ProjectIdentity golden-vector parity', () => {
  const VECTORS: ReadonlyArray<{ path: string; pin: string; note: string }> = [
    { path: '/home/user/my-game', pin: '34ea75f2', note: 'POSIX typical project path' },
    { path: '/home/user/my-game/', pin: '34ea75f2', note: 'trailing slash trimmed → identical' },
    { path: '/home/USER/My-Game', pin: '34ea75f2', note: 'case-folded → identical' },
    { path: 'C:\\Users\\user\\my-game', pin: '8ef72cf7', note: 'Windows backslash form' },
    { path: 'C:\\Users\\user\\my-game\\', pin: '8ef72cf7', note: 'trailing backslash trimmed → identical' },
    { path: 'C:/Users/user/my-game', pin: '5a87324e', note: 'forward-slash form DIFFERS (separators not normalized)' },
    { path: '/home/İstanbul/game', pin: '672d80a7', note: 'U+0130 — ToLowerInvariant leaves it unchanged' },
    { path: '/srv/games/space sim', pin: '08c6cbb6', note: 'path containing a space' },
  ];

  for (const v of VECTORS) {
    it(`derives ${v.pin} for ${JSON.stringify(v.path)} (${v.note})`, () => {
      expect(deriveProjectPin(v.path)).toBe(v.pin);
    });
  }

  it('is 8 lowercase hex chars and independent of any port override', () => {
    const pin = deriveProjectPin('/home/user/my-game');
    expect(pin).toMatch(/^[0-9a-f]{8}$/);
  });
});

// --- Defect D / D21: local-server bind-port precedence -------------------
//
// Parity with the bridge binder `ProjectConnectionResolver` (design 04, owner
// ruling 2026-07-19). Rows below mirror `ProjectConnectionResolverTests.cs`
// theory tables so the CLI and the sidecar can never disagree on a port.

describe('tryGetExplicitLoopbackPort — reads only an explicit LOOPBACK port', () => {
  // Mirrors ProjectConnectionResolverTests.TryGetExplicitLoopbackPort_ReadsOnlyAnExplicitLoopbackPort.
  const ROWS: ReadonlyArray<[string | undefined, number | undefined]> = [
    ['http://localhost:27618/mcp', 27618],       // canonical typed-port case
    ['http://127.0.0.1:27618', 27618],           // IPv4 loopback literal
    ['http://[::1]:27618/mcp', 27618],           // IPv6 literal — port follows the closing bracket
    ['http://user:pass@localhost:27618', 27618], // userinfo colon is not a port separator
    ['http://LOCALHOST:27618', 27618],           // loopback detection is case-insensitive
    ['http://localhost/mcp', undefined],         // no port typed — NOT the scheme default 80
    ['http://localhost:/mcp', undefined],        // empty port
    ['http://localhost:abc/mcp', undefined],     // non-numeric — rejected before parsing
    ['http://localhost:70000', undefined],       // out of range
    ['http://localhost:0', undefined],           // zero is not a bindable port
    ['https://mcp.example.com:9999', undefined], // not loopback
    ['localhost:27618', undefined],              // not an absolute URI
    ['', undefined],
    [undefined, undefined],
  ];
  for (const [host, expected] of ROWS) {
    it(`${JSON.stringify(host)} → ${expected}`, () => {
      expect(tryGetExplicitLoopbackPort(host)).toBe(expected);
    });
  }
});

describe('tryGetExplicitPort — raw-string parser mirrors the LIB incl. its guards', () => {
  // Mirrors ProjectConnectionResolverTests.TryGetExplicitPort_MirrorsTheLibParserIncludingItsGuards.
  const ROWS: ReadonlyArray<[string | undefined, number | undefined]> = [
    ['http://localhost:65535', 65535],                 // upper bound is inclusive
    ['http://localhost:65536', undefined],             // range guard
    ['http://localhost:99999999999999999999', undefined], // overflow, not an exception
    ['http://localhost:+80', undefined],               // no sign
    ['http://localhost:-80', undefined],
    ['http://localhost:8 0', undefined],               // no embedded whitespace
    ['http://localhost:8080?x=1', 8080],               // query terminates the authority
    ['http://localhost:8080#frag', 8080],              // ...as does a fragment
    ['//localhost:8080', undefined],                   // authority empty before the first '/'
    ['localhost:8080', 8080],                          // scheme-less IS parsed here; the loopback gate rejects it
    ['http://[::1]', undefined],                       // bracketed IPv6 alone carries no port
  ];
  for (const [host, expected] of ROWS) {
    it(`${JSON.stringify(host)} → ${expected}`, () => {
      expect(tryGetExplicitPort(host)).toBe(expected);
    });
  }
});

describe('isAbsoluteLoopbackHost', () => {
  it('matches localhost / 127.0.0.0/8 / ::1 (case-insensitive)', () => {
    expect(isAbsoluteLoopbackHost('http://localhost:8080')).toBe(true);
    expect(isAbsoluteLoopbackHost('http://LocalHost')).toBe(true);
    expect(isAbsoluteLoopbackHost('http://127.0.0.1')).toBe(true);
    expect(isAbsoluteLoopbackHost('http://127.9.9.9:1')).toBe(true);
    expect(isAbsoluteLoopbackHost('http://[::1]:1')).toBe(true);
  });
  it('rejects non-loopback, non-absolute, and unparseable hosts', () => {
    expect(isAbsoluteLoopbackHost('https://mcp.example.com')).toBe(false);
    expect(isAbsoluteLoopbackHost('http://192.168.1.5:8080')).toBe(false);
    expect(isAbsoluteLoopbackHost('localhost:8080')).toBe(false); // parses as scheme, no authority
    expect(isAbsoluteLoopbackHost('http://localhost:abc')).toBe(false); // WHATWG rejects the port
    expect(isAbsoluteLoopbackHost('')).toBe(false);
    expect(isAbsoluteLoopbackHost(undefined)).toBe(false);
  });
});

describe('resolveLocalBindPort — three-level precedence (marker → typed → derived)', () => {
  const projectDir = 'C:\\Projects\\MyGame';
  const derived = generatePortFromDirectory(projectDir);

  it('1. marker portOverride wins over a typed loopback host AND the derivation', () => {
    const r = resolveLocalBindPort({ projectDir, host: 'http://localhost:8080', markerPortOverride: 21234 });
    expect(r).toEqual({ port: 21234, source: 'marker-override' });
  });

  it('2. typed loopback-host port wins over the derivation when there is no marker', () => {
    const r = resolveLocalBindPort({ projectDir, host: 'http://localhost:8080' });
    expect(r).toEqual({ port: 8080, source: 'typed-host' });
  });

  it('3a. derives when the loopback host carries no explicit port (port-less http://localhost)', () => {
    const r = resolveLocalBindPort({ projectDir, host: 'http://localhost' });
    expect(r).toEqual({ port: derived, source: 'derived' });
  });

  it('3b. derives when the host is non-loopback (a LAN IP the local server never binds)', () => {
    const r = resolveLocalBindPort({ projectDir, host: 'http://192.168.1.5:8080' });
    expect(r).toEqual({ port: derived, source: 'derived' });
  });

  it('3c. derives when there is no host at all (default fallback)', () => {
    const r = resolveLocalBindPort({ projectDir });
    expect(r).toEqual({ port: derived, source: 'derived' });
  });

  it('ignores an out-of-range / non-integer marker portOverride and falls through', () => {
    expect(resolveLocalBindPort({ projectDir, markerPortOverride: 0 }).source).toBe('derived');
    expect(resolveLocalBindPort({ projectDir, markerPortOverride: 70000 }).source).toBe('derived');
    expect(resolveLocalBindPort({ projectDir, markerPortOverride: 8080.5 }).source).toBe('derived');
  });

  it('parity: the derivation level uses the Windows backslash-root derivation byte-for-byte', () => {
    // C:\\Users\\user\\my-game → 29310 (port.ts golden vector), proving level 3 == the binder.
    expect(resolveLocalBindPort({ projectDir: 'C:\\Users\\user\\my-game', host: 'http://localhost' }))
      .toEqual({ port: 29310, source: 'derived' });
  });
});
