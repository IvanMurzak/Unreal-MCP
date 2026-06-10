import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { fileURLToPath } from 'url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

/** Create a unique temp directory and return its absolute path. */
export function makeTempDir(prefix = 'unreal-cli-test-'): string {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}

/** Remove a temp directory tree (best-effort). */
export function rmTempDir(dir: string): void {
  try {
    fs.rmSync(dir, { recursive: true, force: true });
  } catch {
    /* ignore */
  }
}

/** Write a minimal `.uproject` into `dir` and return its path. */
export function writeUProject(dir: string, name: string, engineAssociation = '5.7'): string {
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, `${name}.uproject`);
  fs.writeFileSync(
    file,
    JSON.stringify(
      {
        FileVersion: 3,
        EngineAssociation: engineAssociation,
        Modules: [{ Name: name, Type: 'Runtime', LoadingPhase: 'Default' }],
      },
      null,
      '\t',
    ),
    'utf-8',
  );
  return file;
}

/** Absolute path to the committed LauncherInstalled.dat fixture. */
export function launcherFixturePath(): string {
  return path.resolve(HERE, 'fixtures', 'LauncherInstalled.dat');
}

/** A `Response`-like stub for injecting into `fetchImpl`. */
export function fakeResponse(opts: { ok: boolean; status: number; body?: string; statusText?: string }): Response {
  return {
    ok: opts.ok,
    status: opts.status,
    statusText: opts.statusText ?? '',
    text: async () => opts.body ?? '',
    json: async () => JSON.parse(opts.body ?? '{}'),
  } as unknown as Response;
}
