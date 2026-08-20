// Correctness proof for the vendored pure-JS BLAKE2b-512 (`src/lib/blake2b.ts`),
// which is the FALLBACK digest that makes minisign `ED` (prehashed) signature
// verification work on runtimes whose crypto backend has no BLAKE2 — notably
// Electron/BoringSSL, where the desktop app runs this package in-process.
//
// WHY THIS FILE IS SHAPED LIKE THIS. A hash that is *almost* right is worse than
// no hash: it would silently refuse every good release. And this suite runs under
// NODE, which HAS `blake2b512` — so a test that merely called the verification
// path would take the NATIVE branch and prove nothing about the fallback. Every
// assertion below therefore calls the pure-JS implementation DIRECTLY, and the
// expected values are pinned to constants produced by OTHER implementations:
//
//   * the official published BLAKE2b-512 vectors (empty, "abc", the pangram, and
//     the classic one-million-'a' vector);
//   * the BLAKE2 known-answer-test unkeyed sequence inputs (in[i] = i & 0xff) at
//     lengths that straddle the 128-byte block boundary;
//   * all of the above regenerated 2026-08-20 from CPython 3.12.10
//     `hashlib.blake2b` — the BLAKE2 REFERENCE implementation, independent of both
//     OpenSSL and this code — so these are cross-implementation vectors, not a
//     recording of our own output.
//
// The differential block additionally compares against Node's native OpenSSL
// digest, a fourth independent implementation, across every buffering boundary.

import { describe, it, expect } from 'vitest';
import { createHash, getHashes } from 'node:crypto';
import { blake2b512Js, createBlake2b512Js, BLAKE2B_BLOCK_BYTES, BLAKE2B_512_BYTES } from '../src/lib/blake2b.js';

const hex = (bytes: Uint8Array): string => Buffer.from(bytes).toString('hex');

/** in[i] = i & 0xff — the BLAKE2 KAT unkeyed sequence input. */
function sequence(n: number): Uint8Array {
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i++) out[i] = i & 0xff;
  return out;
}

/** A deterministic non-repeating pattern, so a block-ordering bug cannot cancel out. */
function pattern(n: number): Uint8Array {
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i++) out[i] = (i * 167 + 13) & 0xff;
  return out;
}

describe('vendored BLAKE2b-512 — official published vectors', () => {
  it('hashes the empty input to the RFC 7693 / reference value', () => {
    expect(hex(blake2b512Js(new Uint8Array(0)))).toBe(
      '786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419' +
        'd25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce',
    );
  });

  it('hashes "abc" to the RFC 7693 Appendix A value', () => {
    expect(hex(blake2b512Js(Buffer.from('abc', 'utf8')))).toBe(
      'ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1' +
        '7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923',
    );
  });

  it('hashes the standard pangram to the published value', () => {
    expect(hex(blake2b512Js(Buffer.from('The quick brown fox jumps over the lazy dog', 'utf8')))).toBe(
      'a8add4bdddfd93e4877d2746e62817b116364a1fa7bc148d95090bc7333b3673' +
        'f82401cf7aa2e4cb1ecd90296e3f14cb5413f8ed77be73045b13914cdcd6a918',
    );
  });

  // 1,000,000 bytes = 7813 full blocks — the strongest single check that the
  // counter, the lazy final-block flush and the block loop all hold at scale.
  it('hashes one million "a" bytes to the published value (7813 blocks)', () => {
    const million = new Uint8Array(1_000_000).fill(0x61);
    expect(hex(blake2b512Js(million))).toBe(
      '98fb3efb7206fd19ebf69b6f312cf7b64e3b94dbe1a17107913975a793f177e1' +
        'd077609d7fba363cbba00d05f7aa4e4fa8715d6428104c0a75643b0ff3fd3eaf',
    );
  });
});

describe('vendored BLAKE2b-512 — KAT sequence inputs across the block boundary', () => {
  // in[i] = i & 0xff at 127 / 128 / 129 / 255 / 256 bytes. 128 is EXACTLY one
  // block: the case a naive implementation gets wrong, because the final block
  // must be compressed with the last-block flag rather than flushed early.
  const CASES: ReadonlyArray<readonly [number, string]> = [
    [
      127,
      'b6292669ccd38d5f01caae96ba272c76a879a45743afa0725d83b9ebb26665b7' +
        '31f1848c52f11972b6644f554c064fa90780dbbbf3a89d4fc31f67df3e5857ef',
    ],
    [
      128,
      '2319e3789c47e2daa5fe807f61bec2a1a6537fa03f19ff32e87eecbfd64b7e0e' +
        '8ccff439ac333b040f19b0c4ddd11a61e24ac1fe0f10a039806c5dcc0da3d115',
    ],
    [
      129,
      'f59711d44a031d5f97a9413c065d1e614c417ede998590325f49bad2fd444d3e' +
        '4418be19aec4e11449ac1a57207898bc57d76a1bcf3566292c20c683a5c4648f',
    ],
    [
      255,
      '5b21c5fd8868367612474fa2e70e9cfa2201ffeee8fafab5797ad58fefa17c9b' +
        '5b107da4a3db6320baaf2c8617d5a51df914ae88da3867c2d41f0cc14fa67928',
    ],
    [
      256,
      '1ecc896f34d3f9cac484c73f75f6a5fb58ee6784be41b35f46067b9c65c63a67' +
        '94d3d744112c653f73dd7deb6666204c5a9bfa5b46081fc10fdbe7884fa5cbf8',
    ],
  ];

  for (const [n, expected] of CASES) {
    it(`matches the reference digest for the ${n}-byte sequence input`, () => {
      expect(hex(blake2b512Js(sequence(n)))).toBe(expected);
    });
  }

  it('exposes the RFC block size and digest length it was built against', () => {
    expect(BLAKE2B_BLOCK_BYTES).toBe(128);
    expect(BLAKE2B_512_BYTES).toBe(64);
  });
});

describe('vendored BLAKE2b-512 — multi-block pattern vectors (CPython reference)', () => {
  const CASES: ReadonlyArray<readonly [number, string]> = [
    [
      129,
      '05a880ec1f4512c1136fe0cda87c6e2c2af4958b28979c932a3596190f4cb1a1' +
        '2be2c11f5d1d8f17e69c702e22fb7fb2dbeb071060c9566108809152ea14b5c3',
    ],
    [
      1000,
      'a76e210917c8640f5e6eb177432dc0f134f547c90dddfff29e9a322dba3b1b38' +
        '53ce81842e744013745fe9c9685f56ca9c2a26c16a633f01c644fc9d7073ec2f',
    ],
    [
      4096,
      'c7f16e79c5b29833f793e56fcc255b6f9eb90e2e8f1e3870582d0df9c561f9c9' +
        '40764830f87bcefcb650f964aae954e091be71409a9928cd384d51c9d68534b2',
    ],
    [
      100_000,
      '00fd0d71de9a0a35dda83d6e64f4ffc8c59c83ca54f0b8ee1d6f3adaf59dad82' +
        'b122837e35972c3c481f7b0276a524150477737cb5e44dcdafc5f1fe6209a7ad',
    ],
  ];

  for (const [n, expected] of CASES) {
    it(`matches the reference digest for the ${n}-byte pattern (${Math.ceil(n / 128)} blocks)`, () => {
      expect(hex(blake2b512Js(pattern(n)))).toBe(expected);
    });
  }
});

describe('vendored BLAKE2b-512 — differential against native OpenSSL', () => {
  // This suite runs on Node, which HAS blake2b512 (OpenSSL). Asserting the
  // vendored implementation is byte-identical to it is the strongest cheap
  // control we have — and it is exactly the equality the fix depends on, since
  // release signatures are produced against the real minisign/OpenSSL digest.
  it('has the native digest available in this runtime (guards the test below from being vacuous)', () => {
    // If this ever fails, the "differential" assertions would be comparing the
    // fallback to itself and would prove nothing — so assert the premise.
    expect(getHashes()).toContain('blake2b512');
    expect(() => createHash('blake2b512')).not.toThrow();
  });

  const LENGTHS = [0, 1, 2, 63, 64, 65, 127, 128, 129, 191, 192, 255, 256, 257, 383, 384, 1023, 1024, 1025, 65_536];

  for (const n of LENGTHS) {
    it(`agrees with node:crypto blake2b512 at ${n} bytes`, () => {
      const data = pattern(n);
      const native = createHash('blake2b512').update(data).digest('hex');
      expect(hex(blake2b512Js(data))).toBe(native);
    });
  }

  it('agrees with node:crypto over 200 random lengths up to 4 KiB', () => {
    for (let i = 0; i < 200; i++) {
      const n = Math.floor(Math.random() * 4096);
      const data = new Uint8Array(n);
      for (let j = 0; j < n; j++) data[j] = (j * 251 + i * 31) & 0xff;
      const native = createHash('blake2b512').update(data).digest('hex');
      expect(hex(blake2b512Js(data)), `length ${n}`).toBe(native);
    }
  });
});

describe('vendored BLAKE2b-512 — streaming form', () => {
  it('produces the same digest as the one-shot form for ragged chunk sizes', () => {
    const data = pattern(5000);
    for (const chunk of [1, 7, 127, 128, 129, 333, 4096]) {
      const s = createBlake2b512Js();
      for (let i = 0; i < data.length; i += chunk) s.update(data.subarray(i, Math.min(i + chunk, data.length)));
      expect(hex(s.digest()), `chunk ${chunk}`).toBe(hex(blake2b512Js(data)));
    }
  });

  it('refuses to be reused after digest() (a reused state would silently mis-hash)', () => {
    const s = createBlake2b512Js();
    s.update(Buffer.from('abc'));
    s.digest();
    expect(() => s.digest()).toThrow(/digest\(\) called twice/);
    expect(() => s.update(Buffer.from('x'))).toThrow(/after digest\(\)/);
  });

  it('is deterministic — the shared scratch vectors do not leak between calls', () => {
    // The module reuses module-level Uint32Array scratch space across calls.
    // Interleaving two different inputs proves no state survives a call.
    const a = pattern(700);
    const b = sequence(900);
    const firstA = hex(blake2b512Js(a));
    const firstB = hex(blake2b512Js(b));
    expect(hex(blake2b512Js(a))).toBe(firstA);
    expect(hex(blake2b512Js(b))).toBe(firstB);
    expect(hex(blake2b512Js(a))).toBe(firstA);
  });

  it('always returns exactly 64 bytes', () => {
    for (const n of [0, 1, 128, 1000]) {
      expect(blake2b512Js(pattern(n)).length).toBe(64);
    }
  });
});
