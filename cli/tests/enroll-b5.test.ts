// B5 regression (auth-fixes design 02 T3 / defect B5): a Windows project root
// reported with backslashes (`C:\a\b` — what `path.resolve` yields inside
// enroll) must derive a routing pin that PREFIX-MATCHES the plugin's
// forward-slash `projectPathHash` (`C:/a/b`). The legacy v1 identity hashed
// separators verbatim, so the enroll pin never matched the plugin's hash and
// pinned routing silently failed on Windows. cli-core's v2 identity converts
// `\`→`/` before hashing, killing B5.

import { describe, it, expect, afterEach } from 'vitest';
import * as path from 'path';
import { enrollPlugin } from '../src/lib/enroll.js';
import { deriveProjectPin } from '../src/utils/port.js';
import {
  derivePinV2,
  deriveProjectPathHashV2,
  derivePin,
  deriveProjectPathHash,
} from '@baizor/gamedev-cli-core';
import { makeTempDir, rmTempDir, fakeResponse } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

describe('B5 — Windows backslash enroll pin matches the plugin hash (cli-core v2 identity)', () => {
  const backslashRoot = 'C:\\Users\\dev\\MyProject';
  const forwardRoot = 'C:/Users/dev/MyProject';

  it('v2: a backslash root pin is a prefix of the plugin forward-slash projectPathHash', () => {
    const pinFromBackslash = derivePinV2(backslashRoot);
    const pluginHash = deriveProjectPathHashV2(forwardRoot); // what the plugin reports

    // The pin is the first 8 hex chars of the v2 hash; because v2 normalizes
    // `\`→`/`, both spellings hash identically, so the backslash-derived pin
    // prefix-matches the plugin's forward-slash hash. THIS is the B5 fix.
    expect(pluginHash.startsWith(pinFromBackslash)).toBe(true);

    // The two spellings derive the exact same v2 pin.
    expect(pinFromBackslash).toBe(derivePinV2(forwardRoot));
  });

  it('documents the pre-fix bug: the legacy v1 pin did NOT match on backslashes', () => {
    // Local v1 pin over the backslash root vs the plugin's v1 forward-slash hash.
    const v1PinFromBackslash = deriveProjectPin(backslashRoot); // no `\`→`/` normalization
    const v1PluginHash = deriveProjectPathHash(forwardRoot);
    // The whole point of B5: these did NOT prefix-match (the silent Windows failure).
    expect(v1PluginHash.startsWith(v1PinFromBackslash)).toBe(false);

    // Sanity: the local v1 pin equals cli-core's v1 pin for the same spelling
    // (parity), so the divergence above is purely the separator normalization.
    expect(v1PinFromBackslash).toBe(derivePin(backslashRoot));
  });

  it('enroll writes a v2 pin that prefix-matches the plugin projectPathHash for the enrolled root', async () => {
    const store = tmp();
    const project = tmp();
    const r = await enrollPlugin({
      enrollCode: 'ABCD-1234',
      projectDir: project,
      storeBaseDir: store,
      fetchImpl: (async () =>
        fakeResponse({
          ok: true,
          status: 200,
          body: JSON.stringify({ access_token: 't', server_url: 'https://ai-game.dev' }),
        })) as unknown as typeof fetch,
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;

    // Whatever separators `path.resolve` produced on this OS, the plugin's v2
    // hash of the same resolved root prefix-matches the enroll pin.
    const resolved = path.resolve(project);
    expect(deriveProjectPathHashV2(resolved).startsWith(r.pin)).toBe(true);
    expect(r.pin).toBe(derivePinV2(resolved));
  });
});
