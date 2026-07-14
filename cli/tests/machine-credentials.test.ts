import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import {
  MachineCredentialStore,
  STORE_DIR_NAME,
  CREDENTIALS_FILE_NAME,
  type MachineCredentials,
} from '../src/utils/machine-credentials.js';
import { makeTempDir, rmTempDir } from './helpers.js';

const isWindows = process.platform === 'win32';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

function sampleCreds(): MachineCredentials {
  return {
    version: 1,
    accessToken: 'jwt-abc',
    refreshToken: 'refresh-xyz',
    expiresAt: '2026-07-14T12:00:00.000Z',
    serverTarget: 'https://ai-game.dev',
  };
}

describe('MachineCredentialStore', () => {
  it('defaults the credential file to ~/.ai-game-dev/credentials.json', () => {
    const store = new MachineCredentialStore();
    expect(store.baseDirectory).toBe(path.join(os.homedir(), STORE_DIR_NAME));
    expect(store.credentialsPath).toBe(path.join(os.homedir(), STORE_DIR_NAME, CREDENTIALS_FILE_NAME));
    expect(CREDENTIALS_FILE_NAME).toBe('credentials.json');
    expect(STORE_DIR_NAME).toBe('.ai-game-dev');
  });

  it('round-trips a credential through write() and read()', async () => {
    const store = new MachineCredentialStore(tmp());
    expect(store.exists()).toBe(false);
    expect(await store.read()).toBeNull();

    const creds = sampleCreds();
    await store.write(creds);

    expect(store.exists()).toBe(true);
    expect(await store.read()).toEqual(creds);
  });

  it('omits null/undefined fields from the persisted document', async () => {
    const store = new MachineCredentialStore(tmp());
    await store.write({ version: 1, accessToken: 'only-jwt', serverTarget: 'https://ai-game.dev' });
    const read = await store.read();
    expect(read).toEqual({ version: 1, accessToken: 'only-jwt', serverTarget: 'https://ai-game.dev' });
    expect(read).not.toHaveProperty('refreshToken');
    expect(read).not.toHaveProperty('subject');
  });

  it('delete() removes the credential file (sign-out)', async () => {
    const store = new MachineCredentialStore(tmp());
    await store.write(sampleCreds());
    expect(store.exists()).toBe(true);
    store.delete();
    expect(store.exists()).toBe(false);
    expect(await store.read()).toBeNull();
    store.delete(); // idempotent — no throw when absent
  });

  // POSIX: plaintext JSON at rest, but locked down to owner-only (0600 file / 0700 dir).
  it.skipIf(isWindows)('writes plaintext camelCase JSON with 0600/0700 perms on POSIX', async () => {
    const base = tmp();
    const store = new MachineCredentialStore(base);
    await store.write(sampleCreds());

    const raw = fs.readFileSync(store.credentialsPath, 'utf-8');
    const parsed = JSON.parse(raw);
    expect(parsed.accessToken).toBe('jwt-abc');
    expect(parsed.refreshToken).toBe('refresh-xyz');
    expect(parsed.serverTarget).toBe('https://ai-game.dev');
    expect(raw).toContain('"accessToken"'); // camelCase keys (matches the C# store)

    expect(fs.statSync(store.credentialsPath).mode & 0o777).toBe(0o600);
    expect(fs.statSync(base).mode & 0o777).toBe(0o700);
  });

  // Windows: DPAPI-encrypted at rest — the secret must never sit in plaintext bytes.
  it.skipIf(!isWindows)('DPAPI-encrypts the document at rest on Windows', async () => {
    const store = new MachineCredentialStore(tmp());
    await store.write(sampleCreds());

    const rawBytes = fs.readFileSync(store.credentialsPath).toString('latin1');
    expect(rawBytes).not.toContain('jwt-abc'); // token is not in the on-disk bytes
    expect(rawBytes).not.toContain('refresh-xyz');
    expect(rawBytes).not.toContain('accessToken');

    // ...yet read() recovers it via CryptUnprotectData.
    expect(await store.read()).toEqual(sampleCreds());
  });
});
