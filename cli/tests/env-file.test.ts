import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import {
  parseEnvContent,
  stripMatchingQuotes,
  writeEnvFile,
  readEnvFile,
  ensureEnvGitignored,
  gitignoreAlreadyIgnoresEnv,
} from '../src/utils/env-file.js';
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

describe('parseEnvContent (plugin parser rules)', () => {
  it('skips blanks/comments, splits on first =, trims, strips quotes, keeps known keys', () => {
    const parsed = parseEnvContent(
      [
        '# comment',
        '',
        '  UNREAL_MCP_HOST = http://localhost:1234  ',
        'UNREAL_MCP_TOKEN="secret=with=eq"',
        "UNREAL_MCP_TRANSPORT='http'",
        'UNKNOWN_KEY=ignored',
        'malformed-line-no-eq',
      ].join('\n'),
    );
    expect(parsed['UNREAL_MCP_HOST']).toBe('http://localhost:1234');
    expect(parsed['UNREAL_MCP_TOKEN']).toBe('secret=with=eq');
    expect(parsed['UNREAL_MCP_TRANSPORT']).toBe('http');
    expect((parsed as Record<string, string>)['UNKNOWN_KEY']).toBeUndefined();
  });
});

describe('stripMatchingQuotes', () => {
  it('strips a matching pair only', () => {
    expect(stripMatchingQuotes('"x"')).toBe('x');
    expect(stripMatchingQuotes("'x'")).toBe('x');
    expect(stripMatchingQuotes('"x')).toBe('"x');
    expect(stripMatchingQuotes('x')).toBe('x');
  });
});

describe('writeEnvFile', () => {
  it('creates a new .env and reports added keys', () => {
    const dir = tmp();
    const envPath = path.join(dir, '.env');
    const r = writeEnvFile(envPath, { UNREAL_MCP_HOST: 'http://localhost:5000', UNREAL_MCP_TOKEN: 'tok' });
    expect(r.added.sort()).toEqual(['UNREAL_MCP_HOST', 'UNREAL_MCP_TOKEN']);
    expect(readEnvFile(envPath)['UNREAL_MCP_HOST']).toBe('http://localhost:5000');
  });

  it('updates an existing key in place and preserves comments + unknown lines', () => {
    const dir = tmp();
    const envPath = path.join(dir, '.env');
    fs.writeFileSync(envPath, '# keep me\nUNREAL_MCP_HOST=old\nMY_OWN_VAR=1\n', 'utf-8');
    const r = writeEnvFile(envPath, { UNREAL_MCP_HOST: 'new' });
    expect(r.updated).toEqual(['UNREAL_MCP_HOST']);
    const text = fs.readFileSync(envPath, 'utf-8');
    expect(text).toContain('# keep me');
    expect(text).toContain('MY_OWN_VAR=1');
    expect(text).toContain('UNREAL_MCP_HOST=new');
    expect(text).not.toContain('UNREAL_MCP_HOST=old');
  });

  it('quotes values that contain whitespace', () => {
    const dir = tmp();
    const envPath = path.join(dir, '.env');
    writeEnvFile(envPath, { UNREAL_MCP_TOOLS: 'a b c' });
    expect(fs.readFileSync(envPath, 'utf-8')).toContain('UNREAL_MCP_TOOLS="a b c"');
  });

  it('round-trips a value containing a double-quote without corruption', () => {
    const dir = tmp();
    const envPath = path.join(dir, '.env');
    writeEnvFile(envPath, { UNREAL_MCP_TOOLS: 'a"b' });
    // No backslash escaping is emitted, so the plugin parser reads it back verbatim.
    expect(fs.readFileSync(envPath, 'utf-8')).not.toContain('\\"');
    expect(readEnvFile(envPath)['UNREAL_MCP_TOOLS']).toBe('a"b');
  });

  it('collapses duplicate key lines and counts the update once', () => {
    // Regression: a file with the same known key twice used to rewrite BOTH
    // lines and push the key into `updated` twice, leaving a duplicate behind.
    const dir = tmp();
    const envPath = path.join(dir, '.env');
    fs.writeFileSync(envPath, 'UNREAL_MCP_HOST=old1\nMY_OWN_VAR=1\nUNREAL_MCP_HOST=old2\n', 'utf-8');
    const r = writeEnvFile(envPath, { UNREAL_MCP_HOST: 'new' });
    expect(r.updated).toEqual(['UNREAL_MCP_HOST']);
    const lines = fs.readFileSync(envPath, 'utf-8').split(/\r?\n/).filter(Boolean);
    expect(lines.filter((l) => l.startsWith('UNREAL_MCP_HOST='))).toEqual(['UNREAL_MCP_HOST=new']);
    expect(lines).toContain('MY_OWN_VAR=1');
  });

  it('removes a key when the value is null', () => {
    const dir = tmp();
    const envPath = path.join(dir, '.env');
    writeEnvFile(envPath, { UNREAL_MCP_HOST: 'x', UNREAL_MCP_TOKEN: 'y' });
    writeEnvFile(envPath, { UNREAL_MCP_TOKEN: null });
    const parsed = readEnvFile(envPath);
    expect(parsed['UNREAL_MCP_HOST']).toBe('x');
    expect(parsed['UNREAL_MCP_TOKEN']).toBeUndefined();
  });
});

describe('ensureEnvGitignored', () => {
  it('creates .gitignore when absent', () => {
    const dir = tmp();
    const r = ensureEnvGitignored(dir);
    expect(r.action).toBe('created');
    expect(fs.readFileSync(r.gitignorePath, 'utf-8')).toContain('.env');
  });

  it('appends .env when .gitignore exists without it', () => {
    const dir = tmp();
    fs.writeFileSync(path.join(dir, '.gitignore'), 'node_modules/\n', 'utf-8');
    const r = ensureEnvGitignored(dir);
    expect(r.action).toBe('appended');
    const text = fs.readFileSync(r.gitignorePath, 'utf-8');
    expect(text).toContain('node_modules/');
    expect(text).toMatch(/\.env\s*$/);
  });

  it('is a no-op when .env is already ignored', () => {
    const dir = tmp();
    fs.writeFileSync(path.join(dir, '.gitignore'), 'Binaries/\n.env\n', 'utf-8');
    const r = ensureEnvGitignored(dir);
    expect(r.action).toBe('already-ignored');
  });

  it('does not double-append when the file has no trailing newline', () => {
    const dir = tmp();
    fs.writeFileSync(path.join(dir, '.gitignore'), 'Saved/', 'utf-8'); // no newline
    const r = ensureEnvGitignored(dir);
    expect(r.action).toBe('appended');
    const lines = fs.readFileSync(r.gitignorePath, 'utf-8').split(/\r?\n/).filter(Boolean);
    expect(lines).toContain('Saved/');
    expect(lines).toContain('.env');
  });
});

describe('gitignoreAlreadyIgnoresEnv', () => {
  it('detects .env, /.env, and *.env (but not negations)', () => {
    expect(gitignoreAlreadyIgnoresEnv('.env')).toBe(true);
    expect(gitignoreAlreadyIgnoresEnv('/.env')).toBe(true);
    expect(gitignoreAlreadyIgnoresEnv('*.env')).toBe(true);
    expect(gitignoreAlreadyIgnoresEnv('!.env')).toBe(false);
    expect(gitignoreAlreadyIgnoresEnv('# .env')).toBe(false);
    expect(gitignoreAlreadyIgnoresEnv('node_modules/')).toBe(false);
  });
});
