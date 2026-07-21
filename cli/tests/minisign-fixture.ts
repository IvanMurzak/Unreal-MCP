// Test-only minisign keypair + signing helper. Builds minisign-wire-format
// public keys and `.minisig` signatures from Node's Ed25519 + BLAKE2b-512, so the
// signature-verification suites can prove ACCEPT-real / REJECT-tampered without
// the `minisign` binary. The wire layout it emits is byte-identical to what
// `minisign -S` produces (validated against a pynacl-generated golden vector in
// plugin-signature.test.ts), so `release.yml`'s real `minisign` output verifies
// against the same `verifyMinisign` these fixtures exercise.

import { generateKeyPairSync, createHash, sign as cryptoSign, randomBytes } from 'node:crypto';

const ED25519_SPKI_PREFIX = Buffer.from('302a300506032b6570032100', 'hex');

export interface MinisignTestKeypair {
  /** minisign public-key file text (2 lines: untrusted comment + base64). */
  readonly publicKeyText: string;
  /** Just the base64 public-key line (no comment) — the form baked into the CLI. */
  readonly publicKeyLine: string;
  /** The 8-byte key id, hex. */
  readonly keyIdHex: string;
  /**
   * Sign `data` and return the `.minisig` file text. Defaults to prehashed
   * (`ED`) exactly as `minisign -S` does; pass `{ legacy: true }` for the raw
   * (`Ed`) algorithm.
   */
  sign(data: Uint8Array, opts?: { trustedComment?: string; legacy?: boolean }): string;
}

/** Extract the raw 32-byte Ed25519 public key from a KeyObject. */
function rawPublicKey(publicKey: ReturnType<typeof generateKeyPairSync>['publicKey']): Buffer {
  const spki = publicKey.export({ type: 'spki', format: 'der' });
  // SPKI = 12-byte prefix + 32-byte key.
  return Buffer.from(spki.subarray(ED25519_SPKI_PREFIX.length));
}

/** Generate a fresh minisign-format keypair for a test. `keyId` is random unless given. */
export function makeMinisignKeypair(keyId: Buffer = randomBytes(8)): MinisignTestKeypair {
  const { publicKey, privateKey } = generateKeyPairSync('ed25519');
  const raw = rawPublicKey(publicKey);
  const pubBlob = Buffer.concat([Buffer.from('Ed', 'latin1'), keyId, raw]); // 42 bytes
  const publicKeyLine = pubBlob.toString('base64');
  const publicKeyText = `untrusted comment: unreal-mcp test key\n${publicKeyLine}\n`;

  const sign: MinisignTestKeypair['sign'] = (data, opts = {}) => {
    const legacy = opts.legacy === true;
    const algo = legacy ? 'Ed' : 'ED';
    const message = legacy ? Buffer.from(data) : createHash('blake2b512').update(data).digest();
    const signature = cryptoSign(null, message, privateKey); // 64 bytes
    const sigBlob = Buffer.concat([Buffer.from(algo, 'latin1'), keyId, signature]); // 74 bytes
    const trustedComment = opts.trustedComment ?? 'timestamp:1700000000\tfile:unreal-mcp-plugin-source.zip';
    const globalSig = cryptoSign(null, Buffer.concat([signature, Buffer.from(trustedComment, 'utf8')]), privateKey);
    return (
      `untrusted comment: signature from unreal-mcp test key\n` +
      `${sigBlob.toString('base64')}\n` +
      `trusted comment: ${trustedComment}\n` +
      `${globalSig.toString('base64')}\n`
    );
  };

  return { publicKeyText, publicKeyLine, keyIdHex: keyId.toString('hex'), sign };
}
