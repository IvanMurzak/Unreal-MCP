// The BLAKE2b-512 FALLBACK inside signature verification.
//
// THE VACUITY TRAP THIS FILE EXISTS TO AVOID. Node HAS `blake2b512`. So a test
// that just calls `verifyMinisign(...)` on this runtime takes the NATIVE branch
// and would pass with the fallback deleted — proving nothing about the code path
// that actually runs in the packaged Electron app. Every assertion below either
// forces the fallback explicitly (via the documented `blake2b512` seam, or by
// calling `fallbackBlake2b512` directly), or injects a native implementation that
// fails the way Electron/BoringSSL really fails.
//
// The suite is organised as four claims:
//   1. the native-first/fallback-second POLICY switches over correctly, including
//      when native throws `Digest method not supported` exactly as Electron does;
//   2. the fallback and the native digest are INTERCHANGEABLE inside a real
//      signature verification — sign with one, verify with the other;
//   3. an unobtainable digest FAILS CLOSED and can never read as verified, with a
//      positive control in the same fixture family proving the fixture verifies
//      when the digest IS obtainable;
//   4. the REAL published v0.14.0 release signature is prehashed and matches the
//      pinned key — i.e. the premise of this whole fix holds on a real artifact.

import { describe, it, expect } from 'vitest';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash, getHashes } from 'node:crypto';
import {
  MINISIGN_PUBLIC_KEY,
  parseMinisignPublicKey,
  parseMinisignSignature,
  verifyMinisign,
  signatureFailureReason,
  describeDigestRuntime,
  nativeBlake2b512,
  fallbackBlake2b512,
  blake2b512,
  selectBlake2b512,
  type SignatureVerdict,
} from '../src/lib/plugin-signature.js';
import { makeMinisignKeypair } from './minisign-fixture.js';

const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), 'fixtures');

/** The exact error Electron 43 / BoringSSL throws (measured 2026-08-20). */
const ELECTRON_DIGEST_ERROR = 'Digest method not supported';
const electronNativeDigest = (): Buffer => {
  throw new Error(ELECTRON_DIGEST_ERROR);
};

/** Deterministic, non-repeating payload of `n` bytes. */
function payload(n: number): Uint8Array {
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i++) out[i] = (i * 167 + 13) & 0xff;
  return out;
}

// ---------------------------------------------------------------------------
// Premise. If Node ever loses `blake2b512`, several controls below silently stop
// discriminating (they would be comparing the fallback with itself). Assert it.
// ---------------------------------------------------------------------------
describe('runtime premise', () => {
  it('this test runtime HAS the native blake2b512 digest', () => {
    expect(getHashes()).toContain('blake2b512');
    expect(nativeBlake2b512(payload(10))).not.toBeNull();
  });
});

// ---------------------------------------------------------------------------
// Claim 1 — the native-first / fallback-second policy.
// ---------------------------------------------------------------------------
describe('digest policy: native first, vendored fallback second', () => {
  const data = payload(5000);
  const trueDigest = createHash('blake2b512').update(data).digest();

  it('uses the fallback when native throws Electron/BoringSSL\'s exact error', () => {
    const got = selectBlake2b512(data, electronNativeDigest, fallbackBlake2b512);
    expect(got).not.toBeNull();
    expect(got!.equals(trueDigest)).toBe(true);
  });

  it('uses the fallback when native reports unavailability by returning null', () => {
    const got = selectBlake2b512(data, () => null, fallbackBlake2b512);
    expect(got!.equals(trueDigest)).toBe(true);
  });

  it('uses the fallback when native returns a wrong-width digest', () => {
    const got = selectBlake2b512(data, () => Buffer.alloc(32, 0xaa), fallbackBlake2b512);
    expect(got!.equals(trueDigest)).toBe(true);
  });

  it('prefers native and does NOT consult the fallback when native works', () => {
    const sentinel = Buffer.alloc(64, 0x5a);
    const got = selectBlake2b512(data, () => sentinel, () => {
      throw new Error('fallback must not be reached when native succeeds');
    });
    expect(got!.equals(sentinel)).toBe(true);
  });

  it('returns null — never a partial or fabricated digest — when BOTH fail', () => {
    expect(selectBlake2b512(data, () => null, () => null)).toBeNull();
    expect(selectBlake2b512(data, electronNativeDigest, () => {
      throw new Error('no digest here either');
    })).toBeNull();
    expect(selectBlake2b512(data, () => null, () => Buffer.alloc(8))).toBeNull();
  });

  it('the production policy agrees with the fallback byte for byte', () => {
    for (const n of [0, 1, 127, 128, 129, 4096]) {
      const d = payload(n);
      expect(blake2b512(d)!.toString('hex'), `length ${n}`).toBe(fallbackBlake2b512(d)!.toString('hex'));
    }
  });
});

// ---------------------------------------------------------------------------
// Claim 2 — native and fallback are interchangeable inside a REAL verification.
// The fixture SIGNS with node's native BLAKE2b-512 (see minisign-fixture.ts);
// verifying those signatures through the fallback is therefore a genuine
// cross-implementation agreement test, not a self-comparison.
// ---------------------------------------------------------------------------
describe('signatures signed with the NATIVE digest verify through the FALLBACK', () => {
  const key = makeMinisignKeypair();
  const forceFallback = { blake2b512: fallbackBlake2b512 };

  // Empty, sub-block, exactly one block, just over one block, and multi-block.
  const SIZES = [0, 1, 100, 128, 129, 4096, 200_000];

  for (const n of SIZES) {
    it(`verifies a prehashed signature over ${n} bytes with native forced unavailable`, () => {
      const data = payload(n);
      const sig = key.sign(data);
      expect(verifyMinisign(key.publicKeyLine, sig, data, forceFallback)).toBe('verified');
    });

    it(`REJECTS a one-byte corruption of the ${n}-byte payload through the fallback`, () => {
      const data = payload(n);
      const sig = key.sign(data);
      // A verifier that cannot reject is worse than no verifier. For the empty
      // payload, corruption means appending a byte (there is none to flip).
      const tampered = n === 0 ? Uint8Array.from([0x01]) : Uint8Array.from(data);
      if (n > 0) tampered[Math.floor(n / 2)] ^= 0x01;
      expect(verifyMinisign(key.publicKeyLine, sig, tampered, forceFallback)).toBe('signature-mismatch');
    });
  }

  it('produces the same verdict under the fallback as under native, across the same corpus', () => {
    for (const n of SIZES) {
      const data = payload(n);
      const sig = key.sign(data);
      const nativeVerdict = verifyMinisign(key.publicKeyLine, sig, data, { blake2b512: nativeBlake2b512 });
      const fallbackVerdict = verifyMinisign(key.publicKeyLine, sig, data, forceFallback);
      expect(fallbackVerdict, `length ${n}`).toBe(nativeVerdict);
      expect(fallbackVerdict).toBe('verified');
    }
  });

  it('never consults the digest at all for a LEGACY (Ed) signature', () => {
    // Legacy signs raw bytes. If the fallback were wired in unconditionally, this
    // would blow up — the seam proves the digest is scoped to prehashed only.
    const data = payload(300);
    const legacySig = key.sign(data, { legacy: true });
    const verdict = verifyMinisign(key.publicKeyLine, legacySig, data, {
      blake2b512: () => {
        throw new Error('digest must not be computed for a legacy Ed signature');
      },
    });
    expect(verdict).toBe('verified');
  });
});

// ---------------------------------------------------------------------------
// Claim 3 — fail closed. Every assertion here has a positive control built from
// the SAME keypair, payload and signature, differing only in the digest backend,
// so "did not verify" cannot be an artefact of a broken fixture.
// ---------------------------------------------------------------------------
describe('fail-closed when no digest can be produced', () => {
  const key = makeMinisignKeypair();
  const data = payload(2048);
  const sig = key.sign(data);

  it('POSITIVE CONTROL: this exact fixture verifies when a digest IS available', () => {
    expect(verifyMinisign(key.publicKeyLine, sig, data, { blake2b512: fallbackBlake2b512 })).toBe('verified');
    expect(verifyMinisign(key.publicKeyLine, sig, data)).toBe('verified');
  });

  it('returns digest-unavailable — not verified — when the digest backend yields null', () => {
    expect(verifyMinisign(key.publicKeyLine, sig, data, { blake2b512: () => null })).toBe('digest-unavailable');
  });

  it('returns digest-unavailable — not verified — when the digest backend THROWS', () => {
    expect(
      verifyMinisign(key.publicKeyLine, sig, data, {
        blake2b512: () => {
          throw new Error(ELECTRON_DIGEST_ERROR);
        },
      }),
    ).toBe('digest-unavailable');
  });

  it('returns digest-unavailable for a wrong-width digest rather than hashing on regardless', () => {
    expect(verifyMinisign(key.publicKeyLine, sig, data, { blake2b512: () => Buffer.alloc(32) })).toBe(
      'digest-unavailable',
    );
  });

  it('a WRONG but well-formed digest fails closed as signature-mismatch, never verified', () => {
    const verdict = verifyMinisign(key.publicKeyLine, sig, data, { blake2b512: () => Buffer.alloc(64, 0xff) });
    expect(verdict).toBe('signature-mismatch');
    expect(verdict).not.toBe('verified');
  });

  it('verifyMinisign never throws, whatever the injected backend does', () => {
    for (const backend of [
      () => {
        throw new Error('boom');
      },
      () => {
        throw 'not even an Error';
      },
      () => null,
      () => Buffer.alloc(0),
    ]) {
      expect(() => verifyMinisign(key.publicKeyLine, sig, data, { blake2b512: backend as never })).not.toThrow();
    }
  });
});

// ---------------------------------------------------------------------------
// Claim 4 — the REAL published release signature. This is the premise the whole
// fix rests on: our releases really are prehashed (`ED`), so the BLAKE2b path
// really does run on every Unreal plugin install.
// ---------------------------------------------------------------------------
describe('the real v0.14.0 release signature', () => {
  const realSignatureText = readFileSync(join(FIXTURES, 'unreal-mcp-plugin-source-0.14.0.zip.minisig'), 'utf8');

  /**
   * BLAKE2b-512 of the real published `unreal-mcp-plugin-source-0.14.0.zip`
   * (131,089,755 bytes; sha256 7db3a5c4…786a), measured 2026-08-20 with BOTH
   * node:crypto's native OpenSSL digest and the vendored fallback — they agreed
   * byte for byte. The zip itself is far too large to commit; this digest is the
   * part the signature actually covers.
   */
  const REAL_ZIP_BLAKE2B512 = Buffer.from(
    '843e643e37733305730d7b6fbeff832152d98b16445de0b0d42534c400fb0744' +
      'eae650fcf7d95672641da4f761db1c4a40838da242f5a1ecc1ae39df9c66c8d8',
    'hex',
  );

  it('is tagged ED — PREHASHED — so verification really does need BLAKE2b-512', () => {
    const sig = parseMinisignSignature(realSignatureText);
    expect(sig).not.toBeNull();
    expect(sig!.algorithm).toBe('ED');
  });

  it('was made with the key this CLI pins', () => {
    const pub = parseMinisignPublicKey(MINISIGN_PUBLIC_KEY);
    const sig = parseMinisignSignature(realSignatureText);
    expect(pub).not.toBeNull();
    expect(pub!.keyId.equals(sig!.keyId)).toBe(true);
    // These are the 8 key-id bytes in WIRE order. `minisign -R` prints the same
    // id as a little-endian u64, i.e. 194DD8FC6092DA33 — the form quoted in the
    // MINISIGN_PUBLIC_KEY docblock and docs/RELEASING.md. Same key, two spellings.
    expect(pub!.keyId.toString('hex')).toBe('33da9260fcd84d19');
    expect(Buffer.from(pub!.keyId).reverse().toString('hex').toUpperCase()).toBe('194DD8FC6092DA33');
  });

  // The real signature + the real pinned key + the real file's BLAKE2b-512 digest
  // ⇒ verified. Combined with the vector/differential proof that our fallback
  // COMPUTES that digest from those bytes, this closes the chain end to end.
  // (The full-file run is `npm run verify:real-asset`; measured verified under
  // both Node 22 and Electron 43 on 2026-08-20.)
  it('verifies against the pinned key when handed the real payload digest', () => {
    expect(verifyMinisign(MINISIGN_PUBLIC_KEY, realSignatureText, new Uint8Array(0), {
      blake2b512: () => REAL_ZIP_BLAKE2B512,
    })).toBe('verified');
  });

  it('does NOT verify when that digest is altered by a single bit', () => {
    const flipped = Buffer.from(REAL_ZIP_BLAKE2B512);
    flipped[0] ^= 0x01;
    expect(verifyMinisign(MINISIGN_PUBLIC_KEY, realSignatureText, new Uint8Array(0), { blake2b512: () => flipped })).toBe(
      'signature-mismatch',
    );
  });

  // OPT-IN full end-to-end over the real 131 MB asset. Not committed (too large);
  // point UNREAL_MCP_REAL_ASSET_DIR at a directory holding the downloaded zip and
  // its .minisig to run it. Reports loudly rather than silently skipping.
  const realDir = process.env.UNREAL_MCP_REAL_ASSET_DIR;
  const realZipPath = realDir ? join(realDir, 'unreal-mcp-plugin-source-0.14.0.zip') : '';
  const haveRealZip = realZipPath !== '' && existsSync(realZipPath);

  it.runIf(haveRealZip)(
    'full end-to-end: the real 131 MB asset verifies through the FALLBACK and rejects a corrupted byte',
    () => {
      const zip = readFileSync(realZipPath);
      expect(fallbackBlake2b512(zip)!.equals(REAL_ZIP_BLAKE2B512)).toBe(true);
      expect(verifyMinisign(MINISIGN_PUBLIC_KEY, realSignatureText, zip, { blake2b512: fallbackBlake2b512 })).toBe(
        'verified',
      );
      const corrupted = Buffer.from(zip);
      corrupted[Math.floor(corrupted.length / 2)] ^= 0x01;
      expect(verifyMinisign(MINISIGN_PUBLIC_KEY, realSignatureText, corrupted, { blake2b512: fallbackBlake2b512 })).toBe(
        'signature-mismatch',
      );
    },
    120_000,
  );
});

// ---------------------------------------------------------------------------
// Error text. The original failure reached the user as four bare words with no
// indication of what failed or what to do; that is what these pin.
// ---------------------------------------------------------------------------
describe('failure messages carry context and a way forward', () => {
  it('names the asset, the runtime and the escape hatch for digest-unavailable', () => {
    const reason = signatureFailureReason(
      'digest-unavailable',
      'unreal-mcp-plugin-source-0.14.0.zip.minisig',
      'electron 43.0.0, node 24.17.0, openssl 0.0.0',
    );
    expect(reason).toContain('unreal-mcp-plugin-source-0.14.0.zip.minisig');
    expect(reason).toContain('BLAKE2b-512');
    expect(reason).toContain('electron 43.0.0, node 24.17.0, openssl 0.0.0');
    expect(reason).toContain('--plugin-source');
    expect(reason).toContain('NOT verified');
    // The bare four-word string the user used to see is not an acceptable message.
    expect(reason).not.toBe('Digest method not supported');
    expect(reason.length).toBeGreaterThan(80);
  });

  it('describes the runtime crypto backend from process.versions', () => {
    expect(describeDigestRuntime({ electron: '43.0.0', node: '24.17.0', openssl: '0.0.0' })).toBe(
      'electron 43.0.0, node 24.17.0, openssl 0.0.0',
    );
    expect(describeDigestRuntime({ node: '22.18.0', openssl: '3.0.16' })).toBe('node 22.18.0, openssl 3.0.16');
    expect(describeDigestRuntime({})).toBe('unknown runtime');
    // The default reads the live runtime, and must not be empty.
    expect(describeDigestRuntime().length).toBeGreaterThan(0);
  });

  it('gives every failing verdict a distinct message that does not claim success', () => {
    const failing: SignatureVerdict[] = [
      'public-key-not-provisioned',
      'public-key-unparsable',
      'signature-unparsable',
      'key-id-mismatch',
      'unsupported-algorithm',
      'digest-unavailable',
      'signature-mismatch',
      'global-signature-mismatch',
    ];
    const seen = new Set<string>();
    for (const verdict of failing) {
      const reason = signatureFailureReason(verdict, 'asset.minisig', 'test runtime');
      // The `default:` arm of that switch returns "the signature was verified".
      // A new verdict that forgot its case would land there and tell the user the
      // opposite of the truth — so assert no failing verdict can produce it.
      expect(reason, verdict).not.toContain('was verified');
      expect(reason.length, verdict).toBeGreaterThan(20);
      expect(seen.has(reason), `duplicate message for ${verdict}`).toBe(false);
      seen.add(reason);
    }
  });
});
