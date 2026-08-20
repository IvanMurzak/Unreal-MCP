// `plugin-signature` — verify a downloaded UnrealMCP plugin-SOURCE zip against a
// SIGNATURE from a pinned publisher key before it is ever extracted or installed
// (zero-config-engine-connect D12). This is the supply-chain UPGRADE of the
// server binary's `server-checksum.ts` SHA256 gate: a same-origin SHA256 is
// integrity-only — a release-asset attacker who can swap the zip can swap the
// checksum with it — whereas a detached signature over a PINNED publisher key
// cannot be forged without the offline private key. Same verify-before-extract
// shape as `download-server.ts`; `plugin-source.ts` fetches the `.minisig`
// sibling asset and calls `verifyMinisign(...)` BETWEEN download and unzip, so an
// unverified zip is NEVER installed (fail-closed). Verification failure = hard
// fail with an actionable reason, never install-unverified.
//
// FORMAT — minisign (github.com/jedisct1/minisign), the Ed25519 signing tool that
// is the workspace key-mint precedent. Wire layout (each `<base64>` line decodes
// to the raw bytes shown):
//
//   public key file (2 lines):
//     untrusted comment: <text>
//     base64( "Ed"(2) | key_id(8) | ed25519_public_key(32) )                [42 B]
//
//   signature file (`.minisig`, 4 lines):
//     untrusted comment: <text>
//     base64( algorithm(2) | key_id(8) | ed25519_signature(64) )            [74 B]
//     trusted comment: <text>
//     base64( ed25519 global_signature(64) )                                [64 B]
//
//   algorithm:  "ED" = PREHASHED — signature is Ed25519 over BLAKE2b-512(file)
//               "Ed" = legacy     — signature is Ed25519 over the raw file bytes
//               (minisign defaults to prehashed "ED".)
//   global_signature = Ed25519 over ( signature(64) || trusted_comment_bytes ),
//               protecting the trusted comment from tampering.
//
// Everything here is PURE (no I/O, no network): parsing is byte-slicing and the
// verify is deterministic (Ed25519 + BLAKE2b-512). The HTTP fetch that surrounds
// this verdict lives in `plugin-source.ts`, so every decision below is
// unit-testable with no real download — the suite proves ACCEPT-real /
// REJECT-tampered against a genuine (pynacl-generated) minisign vector.
//
// BLAKE2b-512 IS NOT UNIVERSALLY AVAILABLE. `node:crypto` delegates digests to the
// runtime's TLS library, and only OpenSSL builds carry the BLAKE2 family. Measured
// on this workspace 2026-08-20:
//
//   Node 22.18.0      openssl 3.0.16   52 digests   blake2b512 ✅
//   Electron 43.0.0   openssl 0.0.0     9 digests   blake2b512 ❌ throws
//                     (BoringSSL)                   'Digest method not supported'
//
// The AI Game Dev desktop app value-imports this package and runs it IN-PROCESS
// under Electron, so an unguarded `createHash('blake2b512')` threw straight out of
// `verifyMinisign` — past every fail-closed verdict below — and surfaced to the
// user as the bare string `Digest method not supported`. Since minisign's default
// `ED` algorithm is PREHASHED (Ed25519 over BLAKE2b-512(file)) and every shipped
// Unreal-MCP release is signed that way, Unreal plugin install was broken outright
// in the packaged app. Unity/Godot are unaffected: their CLIs only hash `sha256`.
//
// The fix is `./blake2b.ts`, a vendored RFC 7693 BLAKE2b-512 used ONLY when the
// runtime lacks the native digest. Native stays first and unchanged. Re-signing
// with the legacy `Ed` tag was rejected because it would strand every
// already-published signature; a correct fallback keeps them all verifiable.

import { createHash, createPublicKey, verify as cryptoVerify } from 'node:crypto';

import { blake2b512Js } from './blake2b.js';

/**
 * The pinned publisher public key the downloaded plugin-source zip is verified
 * against, in minisign public-key wire form (the base64 line, with or without the
 * `untrusted comment:` line). The matching SECRET key is minted offline / held in
 * CI secrets and NEVER ships in this package.
 *
 * ✅ PROVISIONED 2026-07-21 (zero-config-engine-connect e2/e3). The key below is the
 * base64 line of `unreal-mcp-plugin.pub` (minisign key id `194DD8FC6092DA33`); its
 * passwordless secret key is held offline and as the repo secret `MINISIGN_SECRET_KEY`,
 * which release.yml's "Sign the plugin-source zip" step uses (fail-closed — a release
 * errors without it). To ROTATE: mint a new passwordless keypair (`minisign -G -W`),
 * update `MINISIGN_SECRET_KEY`, replace the line below, and cut a new release. Old CLIs
 * keep trusting the old key (no revocation channel — rotate only on compromise + ship
 * the new CLI promptly). Offline/dev installs still skip verification via
 * `--plugin-source <dir>` (a trusted local source, like `--server-source`).
 */
export const MINISIGN_PUBLIC_KEY_UNSET = 'UNPROVISIONED';
export const MINISIGN_PUBLIC_KEY = 'RWQz2pJg/NhNGQhoFbZZiUuNKRdB2esdgZ0Wns1Rd+jbzFefv5zIeerB';

/** Sibling-asset suffix of the detached minisign signature for the source zip. */
export const PLUGIN_SOURCE_SIGNATURE_ASSET_SUFFIX = '.minisig';

/** minisign algorithm tags (2 ASCII bytes). */
const ALGO_PREHASHED = 'ED';
const ALGO_LEGACY = 'Ed';
const ALGO_PUBLIC_KEY = 'Ed';

/** Byte lengths of the fixed-width minisign fields. */
const KEY_ID_LEN = 8;
const ED25519_PUBLIC_KEY_LEN = 32;
const ED25519_SIGNATURE_LEN = 64;
const PUBLIC_KEY_BLOB_LEN = 2 + KEY_ID_LEN + ED25519_PUBLIC_KEY_LEN; // 42
const SIGNATURE_BLOB_LEN = 2 + KEY_ID_LEN + ED25519_SIGNATURE_LEN; // 74
/** A BLAKE2b-512 digest is exactly 64 bytes; anything else is not a usable digest. */
const BLAKE2B_512_DIGEST_LEN = 64;

/**
 * The fixed 12-byte ASN.1 SPKI prefix for a raw Ed25519 public key. Prepending it
 * to the 32 raw key bytes yields the DER SPKI that `createPublicKey` imports —
 * the portable way to turn minisign's raw key bytes into a `node:crypto` key
 * without a PEM round-trip. (Verified: `publicKey.export({type:'spki',format:'der'})`
 * of an Ed25519 key begins with exactly these bytes.)
 */
const ED25519_SPKI_PREFIX = Buffer.from('302a300506032b6570032100', 'hex');

/**
 * A BLAKE2b-512 implementation. Returns `null` when it cannot produce a digest —
 * never throws, so a missing digest becomes a fail-closed VERDICT rather than an
 * exception escaping `verifyMinisign`.
 */
export type Blake2b512Fn = (data: Uint8Array) => Buffer | null;

/**
 * BLAKE2b-512 via `node:crypto`. Returns `null` — rather than throwing — on any
 * runtime whose TLS library lacks the digest (Electron/BoringSSL throws
 * `Digest method not supported` here; see the header note).
 */
export function nativeBlake2b512(data: Uint8Array): Buffer | null {
  try {
    return createHash('blake2b512').update(data).digest();
  } catch {
    return null;
  }
}

/** BLAKE2b-512 via the vendored pure-JS RFC 7693 implementation. Never throws. */
export function fallbackBlake2b512(data: Uint8Array): Buffer | null {
  try {
    return Buffer.from(blake2b512Js(data));
  } catch {
    return null;
  }
}

/**
 * The native-first / fallback-second digest policy, with both implementations
 * injected so the policy itself is testable on a single runtime. A digest counts
 * only if it is present AND exactly 64 bytes; anything else falls through, and
 * `null` out the bottom means NOTHING could be hashed (fail-closed).
 *
 * `native` is tried first and its result is used unchanged whenever it is usable,
 * so on an OpenSSL runtime the fallback is never even called.
 */
export function selectBlake2b512(data: Uint8Array, native: Blake2b512Fn, fallback: Blake2b512Fn): Buffer | null {
  let digest: Buffer | null = null;
  try {
    digest = native(data);
  } catch {
    digest = null;
  }
  if (digest && digest.length === BLAKE2B_512_DIGEST_LEN) return digest;

  try {
    digest = fallback(data);
  } catch {
    return null;
  }
  return digest && digest.length === BLAKE2B_512_DIGEST_LEN ? digest : null;
}

/**
 * The production digest policy: **native first, vendored fallback second**.
 *
 * On a runtime that has `blake2b512` this is exactly the previous behaviour — the
 * native OpenSSL digest, byte for byte. The fallback runs only where the native
 * digest is unavailable, and produces the identical bytes (proven by the
 * differential tests in `tests/blake2b.test.ts`, and measured end-to-end against
 * the real v0.14.0 release asset under Electron 43).
 *
 * Returns `null` only if BOTH fail, which callers MUST treat as "did not verify".
 */
export function blake2b512(data: Uint8Array): Buffer | null {
  return selectBlake2b512(data, nativeBlake2b512, fallbackBlake2b512);
}

/** The subset of `process.versions` that identifies which crypto backend is in play. */
export interface DigestRuntimeVersions {
  readonly node?: string | undefined;
  readonly electron?: string | undefined;
  readonly openssl?: string | undefined;
}

/**
 * A one-line description of the runtime whose crypto backend is being blamed, for
 * the `digest-unavailable` message. Without this the failure reaches the user as
 * an unattributable four-word string; with it, a bug report carries the one fact
 * that explains it (`electron 43.0.0, openssl 0.0.0` = BoringSSL, no BLAKE2).
 * `versions` is injectable so the wording is testable without a second runtime.
 */
export function describeDigestRuntime(versions: DigestRuntimeVersions = process.versions): string {
  const parts: string[] = [];
  if (versions.electron) parts.push(`electron ${versions.electron}`);
  if (versions.node) parts.push(`node ${versions.node}`);
  if (versions.openssl) parts.push(`openssl ${versions.openssl}`);
  return parts.length > 0 ? parts.join(', ') : 'unknown runtime';
}

/** Parsed minisign public key. */
export interface MinisignPublicKey {
  readonly algorithm: string;
  readonly keyId: Buffer;
  readonly publicKey: Buffer;
}

/** Parsed minisign signature file. */
export interface MinisignSignature {
  readonly algorithm: string;
  readonly keyId: Buffer;
  readonly signature: Buffer;
  readonly trustedComment: string;
  readonly globalSignature: Buffer;
}

/**
 * The verdict of verifying a downloaded plugin-source zip against its `.minisig`
 * and the pinned publisher key. Only `'verified'` is safe to extract; every other
 * value is a distinct fail-closed reason the caller MUST treat as "do NOT extract,
 * do NOT install".
 */
export type SignatureVerdict =
  /** Pinned key parsed, signature parsed, key ids matched, BOTH signatures valid. */
  | 'verified'
  /** The baked-in publisher key is still the un-provisioned sentinel (fail-closed). */
  | 'public-key-not-provisioned'
  /** The pinned publisher key text was missing / malformed. */
  | 'public-key-unparsable'
  /** The `.minisig` text was missing / malformed. */
  | 'signature-unparsable'
  /** The signature's key id does not match the pinned publisher key's key id. */
  | 'key-id-mismatch'
  /** The signature's algorithm tag is neither `ED` (prehashed) nor `Ed` (legacy). */
  | 'unsupported-algorithm'
  /**
   * A prehashed (`ED`) signature needs BLAKE2b-512 of the file, and NEITHER the
   * runtime's native digest NOR the vendored fallback could produce one. Nothing
   * is verified in that state, so it is fail-closed like any other non-`verified`
   * verdict — the download is rejected, never installed unverified.
   */
  | 'digest-unavailable'
  /** The Ed25519 signature over the file did not verify (tampered zip / wrong key). */
  | 'signature-mismatch'
  /** The Ed25519 global signature over the trusted comment did not verify. */
  | 'global-signature-mismatch';

/** Non-comment, non-blank lines of a minisign text blob, in order. Pure. */
function contentLines(text: string | null | undefined): string[] {
  if (!text) return [];
  return text
    .replace(/\r\n/g, '\n')
    .split('\n')
    .map((line) => line.trimEnd());
}

/** Standard base64 decode that returns `null` unless it decodes to exactly `expectedLen` bytes. Pure. */
function decodeFixed(b64: string, expectedLen: number): Buffer | null {
  const trimmed = b64.trim();
  if (trimmed.length === 0) return null;
  let decoded: Buffer;
  try {
    decoded = Buffer.from(trimmed, 'base64');
  } catch {
    return null;
  }
  // Buffer.from is lenient (silently drops junk); guard on the exact width so a
  // malformed line can never produce a short/over-long blob that later slices misread.
  if (decoded.length !== expectedLen) return null;
  // Reject non-canonical base64 (e.g. trailing junk that decoded to the right length by luck).
  if (decoded.toString('base64').replace(/=+$/, '') !== trimmed.replace(/=+$/, '')) return null;
  return decoded;
}

/**
 * Parse a minisign public key (the full 2-line file, or just its base64 line).
 * Returns `null` on any malformed input (fail-closed — an unparsable key never
 * verifies). Pure.
 */
export function parseMinisignPublicKey(text: string | null | undefined): MinisignPublicKey | null {
  const lines = contentLines(text).filter(
    (line) => line.length > 0 && !line.toLowerCase().startsWith('untrusted comment:'),
  );
  const b64 = lines[lines.length - 1];
  if (!b64) return null;
  const blob = decodeFixed(b64, PUBLIC_KEY_BLOB_LEN);
  if (!blob) return null;
  const algorithm = blob.subarray(0, 2).toString('latin1');
  if (algorithm !== ALGO_PUBLIC_KEY) return null;
  return {
    algorithm,
    keyId: blob.subarray(2, 2 + KEY_ID_LEN),
    publicKey: blob.subarray(2 + KEY_ID_LEN, PUBLIC_KEY_BLOB_LEN),
  };
}

/**
 * Parse a minisign signature file (`.minisig`). Returns `null` on any malformed
 * input (fail-closed). Pure.
 */
export function parseMinisignSignature(text: string | null | undefined): MinisignSignature | null {
  const lines = contentLines(text);
  const trustedIdx = lines.findIndex((line) => line.toLowerCase().startsWith('trusted comment:'));
  if (trustedIdx < 0) return null;

  // Signature line = the last non-blank, non-untrusted-comment line BEFORE the trusted comment.
  let sigLine: string | undefined;
  for (let i = trustedIdx - 1; i >= 0; i--) {
    const line = lines[i]!;
    if (line.length === 0 || line.toLowerCase().startsWith('untrusted comment:')) continue;
    sigLine = line;
    break;
  }
  if (!sigLine) return null;

  // Global-signature line = the first non-blank line AFTER the trusted comment.
  let globalLine: string | undefined;
  for (let i = trustedIdx + 1; i < lines.length; i++) {
    const line = lines[i]!;
    if (line.length === 0) continue;
    globalLine = line;
    break;
  }
  if (!globalLine) return null;

  const sigBlob = decodeFixed(sigLine, SIGNATURE_BLOB_LEN);
  if (!sigBlob) return null;
  const globalSignature = decodeFixed(globalLine, ED25519_SIGNATURE_LEN);
  if (!globalSignature) return null;

  // The signed trusted comment is the text after `trusted comment:` + one space.
  const trustedComment = lines[trustedIdx]!.slice('trusted comment:'.length).replace(/^ /, '');

  return {
    algorithm: sigBlob.subarray(0, 2).toString('latin1'),
    keyId: sigBlob.subarray(2, 2 + KEY_ID_LEN),
    signature: sigBlob.subarray(2 + KEY_ID_LEN, SIGNATURE_BLOB_LEN),
    trustedComment,
    globalSignature,
  };
}

/** Import a raw 32-byte Ed25519 public key as a `node:crypto` key, or `null` on failure. */
function importEd25519PublicKey(raw: Buffer): ReturnType<typeof createPublicKey> | null {
  try {
    return createPublicKey({ key: Buffer.concat([ED25519_SPKI_PREFIX, raw]), format: 'der', type: 'spki' });
  } catch {
    return null;
  }
}

/** Ed25519 verify that never throws (a thrown crypto error is a fail-closed `false`). */
function ed25519Verify(message: Buffer, keyObject: ReturnType<typeof createPublicKey>, signature: Buffer): boolean {
  try {
    return cryptoVerify(null, message, keyObject, signature);
  } catch {
    return false;
  }
}

/** Options for {@link verifyMinisign}. Production callers pass none. */
export interface VerifyMinisignOptions {
  /**
   * INTERNAL TEST SEAM — the BLAKE2b-512 implementation used for prehashed (`ED`)
   * signatures. Defaults to {@link blake2b512} (native first, vendored fallback
   * second), which is what every production caller gets; `plugin-source.ts` does
   * NOT forward this, and no CLI flag reaches it.
   *
   * It selects WHICH implementation computes the digest — it can never skip,
   * weaken, or short-circuit verification. A wrong digest fails Ed25519 and lands
   * on `signature-mismatch`; a `null` digest lands on `digest-unavailable`. Both
   * are fail-closed.
   */
  readonly blake2b512?: Blake2b512Fn;
}

/**
 * The single fail-closed decision `plugin-source.ts` calls BEFORE `unzipSync`:
 * verify `fileBytes` against `signatureText` (the `.minisig`) using the pinned
 * `publicKeyText`. Returns `'verified'` ONLY when the pinned key parsed, the
 * signature parsed, their key ids matched, a digest was obtainable for a
 * prehashed signature, and BOTH the file signature and the trusted-comment global
 * signature verified with Ed25519; every other outcome is a distinct fail-closed
 * verdict. Deterministic, and never throws.
 */
export function verifyMinisign(
  publicKeyText: string | null | undefined,
  signatureText: string | null | undefined,
  fileBytes: Uint8Array,
  options: VerifyMinisignOptions = {},
): SignatureVerdict {
  const pkText = (publicKeyText ?? '').trim();
  if (pkText.length === 0 || pkText === MINISIGN_PUBLIC_KEY_UNSET) return 'public-key-not-provisioned';

  const pub = parseMinisignPublicKey(pkText);
  if (!pub) return 'public-key-unparsable';

  const sig = parseMinisignSignature(signatureText);
  if (!sig) return 'signature-unparsable';

  if (!pub.keyId.equals(sig.keyId)) return 'key-id-mismatch';
  if (sig.algorithm !== ALGO_PREHASHED && sig.algorithm !== ALGO_LEGACY) return 'unsupported-algorithm';

  const keyObject = importEd25519PublicKey(pub.publicKey);
  if (!keyObject) return 'public-key-unparsable';

  // Prehashed (`ED`) signs BLAKE2b-512 of the file; legacy (`Ed`) signs the file bytes.
  // The digest fn is native-first with a vendored pure-JS fallback, because
  // Electron/BoringSSL has no `blake2b512` (header note). A `null` digest means
  // NOTHING was verified, so it must fail closed rather than fall through to a
  // comparison against arbitrary bytes.
  let message: Buffer;
  if (sig.algorithm === ALGO_PREHASHED) {
    let digest: Buffer | null;
    try {
      digest = (options.blake2b512 ?? blake2b512)(fileBytes);
    } catch {
      // Keeps the "never throws" contract even for an injected implementation.
      digest = null;
    }
    if (!digest || digest.length !== BLAKE2B_512_DIGEST_LEN) return 'digest-unavailable';
    message = digest;
  } else {
    message = Buffer.from(fileBytes);
  }
  if (!ed25519Verify(message, keyObject, sig.signature)) return 'signature-mismatch';

  // Global signature protects the trusted comment: Ed25519 over ( signature || trusted_comment ).
  const globalMessage = Buffer.concat([sig.signature, Buffer.from(sig.trustedComment, 'utf8')]);
  if (!ed25519Verify(globalMessage, keyObject, sig.globalSignature)) return 'global-signature-mismatch';

  return 'verified';
}

/**
 * A short, actionable human-readable reason for a non-`'verified'` verdict.
 *
 * Pure for every verdict except `digest-unavailable`, whose wording embeds a
 * description of the current runtime's crypto backend (that is the single fact
 * that explains the failure). Pass `runtimeDescription` to pin it in a test.
 */
export function signatureFailureReason(
  verdict: SignatureVerdict,
  assetName: string,
  runtimeDescription: string = describeDigestRuntime(),
): string {
  switch (verdict) {
    case 'public-key-not-provisioned':
      return (
        `the plugin-source signing key is not provisioned in this CLI build, so the download cannot be ` +
        `verified — pass --plugin-source <dir> for an offline/dev install, or upgrade to a CLI release ` +
        `built after signing-key provisioning`
      );
    case 'public-key-unparsable':
      return `the pinned publisher key baked into this CLI is malformed`;
    case 'signature-unparsable':
      return `the '${assetName}' signature was missing or malformed`;
    case 'key-id-mismatch':
      return `the '${assetName}' signature was made with a different key than this CLI trusts`;
    case 'unsupported-algorithm':
      return `the '${assetName}' signature uses an unsupported algorithm`;
    case 'digest-unavailable':
      return (
        `the '${assetName}' signature is prehashed (BLAKE2b-512) and this runtime could not compute that ` +
        `digest — neither its built-in crypto (${runtimeDescription}) nor the bundled fallback ` +
        `produced one, so the download was NOT verified. Please report this with the runtime details above; ` +
        `meanwhile, pass --plugin-source <dir> to install from a trusted local checkout`
      );
    case 'signature-mismatch':
      return `the downloaded plugin source did not match its '${assetName}' signature (tampered or wrong key)`;
    case 'global-signature-mismatch':
      return `the '${assetName}' signature's trusted comment failed verification`;
    default:
      return 'the signature was verified';
  }
}
