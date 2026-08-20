// `blake2b` — a self-contained, dependency-free BLAKE2b-512 (RFC 7693) used as a
// FALLBACK when the host runtime's `node:crypto` cannot produce a `blake2b512`
// digest.
//
// WHY THIS EXISTS (measured, 2026-08-20, this workspace):
//
//   runtime               process.versions.openssl   getHashes().length   blake2b512
//   Node   22.18.0        3.0.16  (OpenSSL)          52                   ✅ works
//   Electron 43.0.0       0.0.0   (BoringSSL)         9                   ❌ throws
//                                                                            'Digest method
//                                                                             not supported'
//
// Electron links **BoringSSL**, whose digest table is a small fixed set
// (`md4 md5 ripemd160 sha1 sha224 sha256 sha384 sha512 sha512-256`) with no
// BLAKE2 family at all. The AI Game Dev desktop app value-imports this package and
// runs it IN-PROCESS under Electron, so `createHash('blake2b512')` throws there.
//
// That matters here and only here: minisign's default `ED` algorithm is
// PREHASHED — the Ed25519 signature covers BLAKE2b-512(file), not the file bytes —
// and every Unreal-MCP release asset is signed that way (verified against the real
// `unreal-mcp-plugin-source-0.14.0.zip.minisig`, whose signature blob begins `ED`).
// So plugin-source signature verification, the fail-closed gate that runs BEFORE a
// downloaded zip is ever extracted, could not run at all in the packaged app.
//
// The fix deliberately preserves verifiability of EVERY ALREADY-PUBLISHED release:
// re-signing old assets with the legacy non-prehashed `Ed` tag was rejected because
// it would strand shipped signatures. A correct BLAKE2b-512 here keeps them valid.
//
// CORRECTNESS. A hash that is *almost* right is worse than none — it would silently
// reject good releases. This implementation is pinned by `tests/blake2b.test.ts`
// against the official RFC 7693 / BLAKE2 reference test vectors (empty input, the
// canonical `abc` vector, and multi-block inputs past the 128-byte block size), and
// differentially against Node's native OpenSSL `blake2b512` over a spread of
// lengths that straddle every buffering boundary.
//
// NO NEW DEPENDENCY. Vendored rather than taken from npm: this is the supply-chain
// verification path itself, so widening the dependency graph to fix it would be
// self-defeating. ~120 lines, no I/O, no imports.
//
// IMPLEMENTATION NOTES. BLAKE2b is defined over 64-bit words, which JS lacks
// natively; `BigInt` is ~100x too slow for a 131 MB release asset. So every 64-bit
// word is held as TWO 32-bit lanes in a `Uint32Array`, little-endian: index `2*i` is
// the LOW half of word `i`, index `2*i + 1` is the HIGH half. This is the standard
// portable formulation of RFC 7693 §3.1-§3.2.

/**
 * BLAKE2b IV (RFC 7693 §2.6) — the SHA-512 IV — as 32-bit lanes, low half first.
 * Word 0 `0x6a09e667f3bcc908` becomes `0xf3bcc908, 0x6a09e667`.
 */
const IV32 = new Uint32Array([
  0xf3bcc908, 0x6a09e667, 0x84caa73b, 0xbb67ae85, 0xfe94f82b, 0x3c6ef372, 0x5f1d36f1, 0xa54ff53a, 0xade682d1,
  0x510e527f, 0x2b3e6c1f, 0x9b05688c, 0xfb41bd6b, 0x1f83d9ab, 0x137e2179, 0x5be0cd19,
]);

/**
 * The BLAKE2b message-word permutation SIGMA (RFC 7693 §2.7), 12 rounds x 16
 * indices. BLAKE2b uses 12 rounds, so rounds 10 and 11 reuse rows 0 and 1.
 * Pre-doubled, because `m` also stores each 64-bit word as two lanes, so message
 * word `n` lives at `m[2n]` / `m[2n + 1]`.
 */
// prettier-ignore
const SIGMA_X2 = new Uint8Array([
  0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
  28, 20, 8, 16, 18, 30, 26, 12, 2, 24, 0, 4, 22, 14, 10, 6,
  22, 16, 24, 0, 10, 4, 30, 26, 20, 28, 6, 12, 14, 2, 18, 8,
  14, 18, 6, 2, 26, 24, 22, 28, 4, 12, 10, 20, 8, 0, 30, 16,
  18, 0, 10, 14, 4, 8, 20, 30, 28, 2, 22, 24, 12, 16, 6, 26,
  4, 24, 12, 20, 0, 22, 16, 6, 8, 26, 14, 10, 30, 28, 2, 18,
  24, 10, 2, 30, 28, 26, 8, 20, 0, 14, 12, 6, 18, 4, 16, 22,
  26, 22, 14, 28, 24, 2, 6, 18, 10, 0, 30, 8, 16, 12, 4, 20,
  12, 30, 28, 18, 22, 6, 0, 16, 24, 4, 26, 14, 2, 8, 20, 10,
  20, 4, 16, 8, 14, 12, 2, 10, 30, 22, 18, 28, 6, 24, 26, 0,
  0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
  28, 20, 8, 16, 18, 30, 26, 12, 2, 24, 0, 4, 22, 14, 10, 6,
]);

/** Bytes per BLAKE2b compression block (RFC 7693 §2.1: bb = 128). */
export const BLAKE2B_BLOCK_BYTES = 128;
/** Output length of BLAKE2b-512, in bytes. */
export const BLAKE2B_512_BYTES = 64;

/**
 * Streaming BLAKE2b-512 state.
 *
 * `t` is the RFC's byte counter. It is a JS number rather than a 64-bit pair, so
 * it is exact up to `Number.MAX_SAFE_INTEGER` (2^53-1 bytes = 8 PiB) — six orders
 * of magnitude beyond any release asset. Only its low 64 bits are ever consumed,
 * split back into two lanes at compression time.
 */
interface Blake2bState {
  /** Chaining value h[0..7], as 16 lanes. */
  readonly h: Uint32Array;
  /** The 128-byte block buffer. */
  readonly b: Uint8Array;
  /** Bytes currently held in `b`. */
  c: number;
  /** Total bytes fed in (RFC's `t`). */
  t: number;
}

/** Scratch working vectors, reused across calls (this module is synchronous and single-threaded). */
const v = new Uint32Array(32);
const m = new Uint32Array(32);

/** `v[a..a+1] += v[b..b+1]` over the 64-bit lane pair, with carry. */
function add64AA(a: number, b: number): void {
  const lo = v[a]! + v[b]!;
  let hi = v[a + 1]! + v[b + 1]!;
  if (lo >= 0x100000000) hi++;
  v[a] = lo;
  v[a + 1] = hi;
}

/** `v[a..a+1] += (lo, hi)` over the 64-bit lane pair, with carry. `b0` may be negative (int32). */
function add64AC(a: number, b0: number, b1: number): void {
  let lo = v[a]! + b0;
  if (b0 < 0) lo += 0x100000000;
  let hi = v[a + 1]! + b1;
  if (lo >= 0x100000000) hi++;
  v[a] = lo;
  v[a + 1] = hi;
}

/**
 * The BLAKE2b mixing function G (RFC 7693 §3.1), operating on lane pairs.
 * Rotation constants are the BLAKE2b set: R1=32, R2=24, R3=16, R4=63 — each
 * applied as a right-rotation across the two 32-bit lanes.
 */
function mixG(a: number, b: number, c: number, d: number, ix: number, iy: number): void {
  const x0 = m[ix]!;
  const x1 = m[ix + 1]!;
  const y0 = m[iy]!;
  const y1 = m[iy + 1]!;

  // v[a] = v[a] + v[b] + x
  add64AA(a, b);
  add64AC(a, x0, x1);

  // v[d] = (v[d] ^ v[a]) >>> 32  — a rotation by exactly 32 bits is a lane swap.
  let xor0 = v[d]! ^ v[a]!;
  let xor1 = v[d + 1]! ^ v[a + 1]!;
  v[d] = xor1;
  v[d + 1] = xor0;

  // v[c] = v[c] + v[d]
  add64AA(c, d);

  // v[b] = (v[b] ^ v[c]) >>> 24
  xor0 = v[b]! ^ v[c]!;
  xor1 = v[b + 1]! ^ v[c + 1]!;
  v[b] = (xor0 >>> 24) ^ (xor1 << 8);
  v[b + 1] = (xor1 >>> 24) ^ (xor0 << 8);

  // v[a] = v[a] + v[b] + y
  add64AA(a, b);
  add64AC(a, y0, y1);

  // v[d] = (v[d] ^ v[a]) >>> 16
  xor0 = v[d]! ^ v[a]!;
  xor1 = v[d + 1]! ^ v[a + 1]!;
  v[d] = (xor0 >>> 16) ^ (xor1 << 16);
  v[d + 1] = (xor1 >>> 16) ^ (xor0 << 16);

  // v[c] = v[c] + v[d]
  add64AA(c, d);

  // v[b] = (v[b] ^ v[c]) >>> 63  — i.e. rotate LEFT by 1 across the lane pair.
  xor0 = v[b]! ^ v[c]!;
  xor1 = v[b + 1]! ^ v[c + 1]!;
  v[b] = (xor1 >>> 31) ^ (xor0 << 1);
  v[b + 1] = (xor0 >>> 31) ^ (xor1 << 1);
}

/** The BLAKE2b compression function F (RFC 7693 §3.2). `last` sets the final-block flag f0. */
function compress(state: Blake2bState, last: boolean): void {
  const { h, b } = state;

  for (let i = 0; i < 16; i++) {
    v[i] = h[i]!;
    v[i + 16] = IV32[i]!;
  }

  // v[12] ^= t (low 64 bits). Lane 24/25 is word 12. The high 64 bits of the
  // counter (v[13], lanes 26/27) stay zero — `t` never exceeds 2^53.
  v[24] = v[24]! ^ state.t;
  v[25] = v[25]! ^ state.t / 0x100000000;

  // Final block: v[14] = ~v[14].
  if (last) {
    v[28] = ~v[28]!;
    v[29] = ~v[29]!;
  }

  // Load the block as 16 little-endian 64-bit words → 32 lanes.
  for (let i = 0; i < 32; i++) {
    const j = i * 4;
    m[i] = b[j]! ^ (b[j + 1]! << 8) ^ (b[j + 2]! << 16) ^ (b[j + 3]! << 24);
  }

  for (let r = 0; r < 12; r++) {
    const s = r * 16;
    // Column rounds.
    mixG(0, 8, 16, 24, SIGMA_X2[s]!, SIGMA_X2[s + 1]!);
    mixG(2, 10, 18, 26, SIGMA_X2[s + 2]!, SIGMA_X2[s + 3]!);
    mixG(4, 12, 20, 28, SIGMA_X2[s + 4]!, SIGMA_X2[s + 5]!);
    mixG(6, 14, 22, 30, SIGMA_X2[s + 6]!, SIGMA_X2[s + 7]!);
    // Diagonal rounds.
    mixG(0, 10, 20, 30, SIGMA_X2[s + 8]!, SIGMA_X2[s + 9]!);
    mixG(2, 12, 22, 24, SIGMA_X2[s + 10]!, SIGMA_X2[s + 11]!);
    mixG(4, 14, 16, 26, SIGMA_X2[s + 12]!, SIGMA_X2[s + 13]!);
    mixG(6, 8, 18, 28, SIGMA_X2[s + 14]!, SIGMA_X2[s + 15]!);
  }

  for (let i = 0; i < 16; i++) {
    h[i] = h[i]! ^ v[i]! ^ v[i + 16]!;
  }
}

/** Fresh unkeyed BLAKE2b-512 state (RFC 7693 §2.5 parameter block: depth/fanout 1, no key). */
function init(): Blake2bState {
  const h = new Uint32Array(IV32);
  // h[0] ^= 0x0101kknn  (kk = key length = 0, nn = digest length = 64).
  h[0] = h[0]! ^ 0x01010000 ^ (0 << 8) ^ BLAKE2B_512_BYTES;
  return { h, b: new Uint8Array(BLAKE2B_BLOCK_BYTES), c: 0, t: 0 };
}

/**
 * Absorb `input`. The final block must NOT be compressed here (it needs the `last`
 * flag), so a full buffer is only flushed once we know more bytes are coming —
 * that lazy flush is what makes an exactly-block-multiple input hash correctly.
 * Copies in bulk rather than byte-at-a-time: the release asset is ~131 MB.
 */
function update(state: Blake2bState, input: Uint8Array): void {
  let i = 0;
  while (i < input.length) {
    if (state.c === BLAKE2B_BLOCK_BYTES) {
      state.t += state.c;
      compress(state, false);
      state.c = 0;
    }
    const n = Math.min(BLAKE2B_BLOCK_BYTES - state.c, input.length - i);
    state.b.set(input.subarray(i, i + n), state.c);
    state.c += n;
    i += n;
  }
}

/** Zero-pad the trailing block, compress it with the final-block flag, and emit 64 LE bytes. */
function final(state: Blake2bState): Uint8Array {
  state.t += state.c;
  state.b.fill(0, state.c);
  compress(state, true);

  const out = new Uint8Array(BLAKE2B_512_BYTES);
  for (let i = 0; i < BLAKE2B_512_BYTES; i++) {
    out[i] = (state.h[i >> 2]! >> (8 * (i & 3))) & 0xff;
  }
  return out;
}

/**
 * Pure-JS BLAKE2b-512 of `data` (unkeyed, 64-byte digest), per RFC 7693.
 *
 * Deterministic and allocation-light; no dependencies, no I/O. Byte-identical to
 * `createHash('blake2b512')` on runtimes that have it — proven by the differential
 * test in `tests/blake2b.test.ts`.
 */
export function blake2b512Js(data: Uint8Array): Uint8Array {
  const state = init();
  update(state, data);
  return final(state);
}

/**
 * Streaming form, for callers that hold the payload in chunks. Same digest as
 * feeding the concatenation to {@link blake2b512Js}.
 */
export function createBlake2b512Js(): { update(chunk: Uint8Array): void; digest(): Uint8Array } {
  const state = init();
  let done = false;
  return {
    update(chunk: Uint8Array): void {
      if (done) throw new Error('blake2b512: update() called after digest()');
      update(state, chunk);
    },
    digest(): Uint8Array {
      if (done) throw new Error('blake2b512: digest() called twice');
      done = true;
      return final(state);
    },
  };
}
