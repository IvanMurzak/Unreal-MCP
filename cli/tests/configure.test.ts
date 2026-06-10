import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { configure } from '../src/lib/configure.js';
import { readEnvFile } from '../src/utils/env-file.js';
import { makeTempDir, rmTempDir } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

describe('configure', () => {
  it('writes UNREAL_MCP_* into .env AND gitignores .env (acceptance criterion)', async () => {
    const dir = tmp();
    const result = await configure({
      projectDir: dir,
      connectionMode: 'Custom',
      host: 'http://localhost:5220',
      token: 'sekret',
      authOption: 'required',
      keepConnected: true,
    });
    expect(result.kind).toBe('success');
    if (result.kind !== 'success') return;

    const env = readEnvFile(result.envPath);
    expect(env['UNREAL_MCP_CONNECTION_MODE']).toBe('Custom');
    expect(env['UNREAL_MCP_HOST']).toBe('http://localhost:5220');
    expect(env['UNREAL_MCP_TOKEN']).toBe('sekret');
    expect(env['UNREAL_MCP_AUTH_OPTION']).toBe('required');
    expect(env['UNREAL_MCP_KEEP_CONNECTED']).toBe('true');

    // .gitignore created and ignores .env
    expect(result.gitignoreAction).toBe('created');
    const gi = fs.readFileSync(path.join(dir, '.gitignore'), 'utf-8');
    expect(gi).toContain('.env');
  });

  it('appends .env to an existing .gitignore', async () => {
    const dir = tmp();
    fs.writeFileSync(path.join(dir, '.gitignore'), 'Binaries/\n', 'utf-8');
    const result = await configure({ projectDir: dir, host: 'http://h' });
    expect(result.kind).toBe('success');
    if (result.kind !== 'success') return;
    expect(result.gitignoreAction).toBe('appended');
    expect(fs.readFileSync(path.join(dir, '.gitignore'), 'utf-8')).toContain('.env');
  });

  it('respects ensureGitignore=false (env-only)', async () => {
    const dir = tmp();
    const result = await configure({ projectDir: dir, host: 'http://h', ensureGitignore: false });
    expect(result.kind).toBe('success');
    if (result.kind !== 'success') return;
    expect(result.gitignoreAction).toBe('skipped');
    expect(fs.existsSync(path.join(dir, '.gitignore'))).toBe(false);
  });

  it('updates a single key without clobbering others on a second call', async () => {
    const dir = tmp();
    await configure({ projectDir: dir, host: 'http://a', token: 'tok' });
    await configure({ projectDir: dir, host: 'http://b' });
    const env = readEnvFile(path.join(dir, '.env'));
    expect(env['UNREAL_MCP_HOST']).toBe('http://b');
    expect(env['UNREAL_MCP_TOKEN']).toBe('tok');
  });

  it('leaves .env unwritten and warns when no values are supplied', async () => {
    const dir = tmp();
    const result = await configure({ projectDir: dir, ensureGitignore: false });
    expect(result.kind).toBe('success');
    if (result.kind !== 'success') return;
    expect(result.keysWritten).toEqual([]);
    expect(fs.existsSync(path.join(dir, '.env'))).toBe(false);
    expect(result.warnings.join(' ')).toContain('left unchanged');
  });

  it('fails (no throw) for a missing project dir', async () => {
    const result = await configure({ projectDir: path.join(makeTempDir(), 'does-not-exist') });
    expect(result.kind).toBe('failure');
    if (result.kind === 'failure') expect(result.error).toBeInstanceOf(Error);
  });
});
