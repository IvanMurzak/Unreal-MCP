#!/usr/bin/env node
// Verify a REAL published plugin-source release asset against its REAL `.minisig`
// and the pinned publisher key — through both digest backends — on WHATEVER
// runtime you launch it with.
//
// This exists because the bug it guards is runtime-specific and therefore
// invisible to a Node-only test suite: `node:crypto` has no `blake2b512` on
// Electron/BoringSSL, and minisign's default `ED` algorithm is prehashed
// (Ed25519 over BLAKE2b-512(file)). The npm CI legs run on Node, which HAS the
// native digest — so the only way to observe the Electron behaviour is to run
// this file under Electron. Deliberately dependency-free so it can.
//
//   # 1. fetch the assets (any released version)
//   gh release download v0.14.0 --repo IvanMurzak/Unreal-MCP \
//     --pattern 'unreal-mcp-plugin-source-*.zip*' --dir /tmp/rel
//
//   # 2. Node (native digest available)
//   npm run build && node cli/scripts/verify-real-asset.mjs /tmp/rel 0.14.0
//
//   # 3. Electron (native digest ABSENT — the case the fallback exists for)
//   ELECTRON_RUN_AS_NODE=1 /path/to/electron.exe \
//     cli/scripts/verify-real-asset.mjs /tmp/rel 0.14.0
//
// Exits 0 only if the real asset VERIFIES and a one-byte corruption of it is
// REJECTED — a verifier that cannot reject is the failure mode that matters.

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

const dir = process.argv[2];
const version = process.argv[3] ?? '0.14.0';
if (!dir) {
  console.error('usage: verify-real-asset.mjs <dir-with-release-assets> [version]');
  process.exit(2);
}

const distUrl = pathToFileURL(join(import.meta.dirname, '..', 'dist', 'lib', 'plugin-signature.js'));
const { verifyMinisign, nativeBlake2b512, fallbackBlake2b512, blake2b512, MINISIGN_PUBLIC_KEY, describeDigestRuntime } =
  await import(distUrl.href).catch((err) => {
    console.error('Could not load dist/lib/plugin-signature.js — run `npm run build` first.\n' + err.message);
    process.exit(2);
  });

const zipName = `unreal-mcp-plugin-source-${version}.zip`;
const zip = readFileSync(join(dir, zipName));
const signatureText = readFileSync(join(dir, `${zipName}.minisig`), 'utf8');

const time = (fn) => {
  const t = Date.now();
  const value = fn();
  return { value, ms: Date.now() - t };
};

console.log(`runtime      : ${describeDigestRuntime()}`);
console.log(`asset        : ${zipName} (${zip.length} bytes)`);

const native = time(() => nativeBlake2b512(zip));
const fallback = time(() => fallbackBlake2b512(zip));
console.log(`native   b2b : ${native.value ? native.value.toString('hex') : 'UNAVAILABLE on this runtime'} [${native.ms} ms]`);
console.log(`fallback b2b : ${fallback.value ? fallback.value.toString('hex') : 'FAILED'} [${fallback.ms} ms]`);
if (native.value && fallback.value) {
  console.log(`agree        : ${native.value.equals(fallback.value)}`);
}
console.log(`policy pick  : ${blake2b512(zip)?.toString('hex') ?? 'NULL'}`);

const results = {
  'real asset, production policy': verifyMinisign(MINISIGN_PUBLIC_KEY, signatureText, zip),
  'real asset, fallback forced': verifyMinisign(MINISIGN_PUBLIC_KEY, signatureText, zip, {
    blake2b512: fallbackBlake2b512,
  }),
};

const corrupted = Buffer.from(zip);
corrupted[Math.floor(corrupted.length / 2)] ^= 0x01;
results['corrupted asset, production policy'] = verifyMinisign(MINISIGN_PUBLIC_KEY, signatureText, corrupted);
results['corrupted asset, fallback forced'] = verifyMinisign(MINISIGN_PUBLIC_KEY, signatureText, corrupted, {
  blake2b512: fallbackBlake2b512,
});

console.log('');
for (const [label, verdict] of Object.entries(results)) {
  console.log(`${label.padEnd(36)} => ${verdict}`);
}

const ok =
  results['real asset, production policy'] === 'verified' &&
  results['real asset, fallback forced'] === 'verified' &&
  results['corrupted asset, production policy'] === 'signature-mismatch' &&
  results['corrupted asset, fallback forced'] === 'signature-mismatch';

console.log(`\nRESULT=${ok ? 'OK' : 'FAILED'}`);
process.exit(ok ? 0 : 1);
