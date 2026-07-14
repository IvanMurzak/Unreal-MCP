import { describe, it, expect } from 'vitest';
import { generatePortFromDirectory, normalizeProjectRoot, deriveProjectPin } from '../src/utils/port.js';

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
