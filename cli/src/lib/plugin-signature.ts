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
// verify is deterministic `node:crypto` (Ed25519 + BLAKE2b-512). The HTTP fetch
// that surrounds this verdict lives in `plugin-source.ts`, so every decision
// below is unit-testable with no real download — the suite proves ACCEPT-real /
// REJECT-tampered against a genuine (pynacl-generated) minisign vector.

import { createHash, createPublicKey, verify as cryptoVerify } from 'node:crypto';

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

/**
 * The fixed 12-byte ASN.1 SPKI prefix for a raw Ed25519 public key. Prepending it
 * to the 32 raw key bytes yields the DER SPKI that `createPublicKey` imports —
 * the portable way to turn minisign's raw key bytes into a `node:crypto` key
 * without a PEM round-trip. (Verified: `publicKey.export({type:'spki',format:'der'})`
 * of an Ed25519 key begins with exactly these bytes.)
 */
const ED25519_SPKI_PREFIX = Buffer.from('302a300506032b6570032100', 'hex');

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

/**
 * The single fail-closed decision `plugin-source.ts` calls BEFORE `unzipSync`:
 * verify `fileBytes` against `signatureText` (the `.minisig`) using the pinned
 * `publicKeyText`. Returns `'verified'` ONLY when the pinned key parsed, the
 * signature parsed, their key ids matched, and BOTH the file signature and the
 * trusted-comment global signature verified with Ed25519; every other outcome is
 * a distinct fail-closed verdict. Pure, deterministic, never throws.
 */
export function verifyMinisign(
  publicKeyText: string | null | undefined,
  signatureText: string | null | undefined,
  fileBytes: Uint8Array,
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
  const message =
    sig.algorithm === ALGO_PREHASHED ? createHash('blake2b512').update(fileBytes).digest() : Buffer.from(fileBytes);
  if (!ed25519Verify(message, keyObject, sig.signature)) return 'signature-mismatch';

  // Global signature protects the trusted comment: Ed25519 over ( signature || trusted_comment ).
  const globalMessage = Buffer.concat([sig.signature, Buffer.from(sig.trustedComment, 'utf8')]);
  if (!ed25519Verify(globalMessage, keyObject, sig.globalSignature)) return 'global-signature-mismatch';

  return 'verified';
}

/** A short, actionable human-readable reason for a non-`'verified'` verdict. Pure. */
export function signatureFailureReason(verdict: SignatureVerdict, assetName: string): string {
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
    case 'signature-mismatch':
      return `the downloaded plugin source did not match its '${assetName}' signature (tampered or wrong key)`;
    case 'global-signature-mismatch':
      return `the '${assetName}' signature's trusted comment failed verification`;
    default:
      return 'the signature was verified';
  }
}
