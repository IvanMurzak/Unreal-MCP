import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import {
  resolveConnection,
  readPluginConfig,
  resolveConnectionFromPluginConfig,
  appendMcp,
  isCloudMode,
  PLUGIN_CONFIG_RELATIVE_PATH,
} from '../src/utils/config.js';
import { generatePortFromDirectory } from '../src/utils/port.js';
import { makeTempDir, rmTempDir } from './helpers.js';

const tempDirs: string[] = [];

afterEach(() => {
  while (tempDirs.length) rmTempDir(tempDirs.pop()!);
});

/** Make a temp project dir; optionally drop a plugin JSON config and/or `.env`. */
function makeProject(opts: { config?: object | string; env?: string } = {}): string {
  const dir = makeTempDir();
  tempDirs.push(dir);
  if (opts.config !== undefined) {
    const configPath = path.join(dir, PLUGIN_CONFIG_RELATIVE_PATH);
    fs.mkdirSync(path.dirname(configPath), { recursive: true });
    const text = typeof opts.config === 'string' ? opts.config : JSON.stringify(opts.config, null, 2);
    fs.writeFileSync(configPath, text, 'utf-8');
  }
  if (opts.env !== undefined) {
    fs.writeFileSync(path.join(dir, '.env'), opts.env, 'utf-8');
  }
  return dir;
}

describe('appendMcp', () => {
  it('appends /mcp to a bare base URL', () => {
    expect(appendMcp('https://ai-game.dev')).toBe('https://ai-game.dev/mcp');
  });
  it('is idempotent when the URL already ends with /mcp', () => {
    expect(appendMcp('https://ai-game.dev/mcp')).toBe('https://ai-game.dev/mcp');
  });
  it('handles a trailing slash after /mcp (no /mcp/mcp)', () => {
    expect(appendMcp('https://ai-game.dev/mcp/')).toBe('https://ai-game.dev/mcp');
  });
  it('is case-insensitive on the existing /mcp segment', () => {
    expect(appendMcp('https://ai-game.dev/MCP')).toBe('https://ai-game.dev/mcp');
  });
  it('strips trailing slashes on a bare base URL', () => {
    expect(appendMcp('https://ai-game.dev/')).toBe('https://ai-game.dev/mcp');
  });
});

describe('readPluginConfig', () => {
  it('returns undefined when the file is absent', () => {
    const dir = makeProject();
    expect(readPluginConfig(dir)).toBeUndefined();
  });
  it('returns undefined for malformed JSON (does not throw)', () => {
    const dir = makeProject({ config: '{ not json' });
    expect(readPluginConfig(dir)).toBeUndefined();
  });
  it('tolerates a UTF-8 BOM', () => {
    const dir = makeProject({ config: '﻿' + JSON.stringify({ connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev' }) });
    const cfg = readPluginConfig(dir);
    expect(cfg?.connectionMode).toBe('Cloud');
    expect(cfg?.cloudUrl).toBe('https://ai-game.dev');
  });
});

describe('resolveConnectionFromPluginConfig', () => {
  it('Cloud → appendMcp(cloudUrl) + cloudToken', () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'ct' });
    expect(r.url).toBe('https://ai-game.dev/mcp');
    expect(r.token).toBe('ct');
  });
  it('Custom + authOption Required → host + token', () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', authOption: 'Required', token: 'tk' });
    expect(r.url).toBe('http://localhost:9001');
    expect(r.token).toBe('tk');
  });
  it('Custom + authOption None → token undefined', () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', authOption: 'None', token: 'tk' });
    expect(r.url).toBe('http://localhost:9001');
    expect(r.token).toBeUndefined();
  });

  it('isCloudMode is case-insensitive', () => {
    expect(isCloudMode({ connectionMode: 'cloud' })).toBe(true);
    expect(isCloudMode({ connectionMode: 'Custom' })).toBe(false);
    expect(isCloudMode({})).toBe(false);
  });
});

describe('resolveConnection — plugin-config layer (issue #71)', () => {
  it('Cloud config → url ends with /mcp, token = cloudToken, source plugin-config', () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' } });
    const r = resolveConnection({ projectDir: dir });
    expect(r.url).toBe('https://ai-game.dev/mcp');
    expect(r.token).toBe('cloud-secret');
    expect(r.source).toBe('plugin-config');
  });

  it('Cloud config whose cloudUrl already ends /mcp → single /mcp', () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev/mcp', cloudToken: 't' } });
    expect(resolveConnection({ projectDir: dir }).url).toBe('https://ai-game.dev/mcp');
  });

  it('Cloud config whose cloudUrl ends /mcp/ → single /mcp', () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev/mcp/', cloudToken: 't' } });
    expect(resolveConnection({ projectDir: dir }).url).toBe('https://ai-game.dev/mcp');
  });

  it('Custom config + authOption Required → host + token', () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost:9100', authOption: 'Required', token: 'tk' } });
    const r = resolveConnection({ projectDir: dir });
    expect(r.url).toBe('http://localhost:9100');
    expect(r.token).toBe('tk');
    expect(r.source).toBe('plugin-config');
  });

  it('Custom config + authOption None → token undefined', () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost:9100', authOption: 'None', token: 'tk' } });
    const r = resolveConnection({ projectDir: dir });
    expect(r.url).toBe('http://localhost:9100');
    expect(r.token).toBeUndefined();
  });

  it('explicit --url override still wins over a Cloud config', () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' } });
    const r = resolveConnection({ projectDir: dir, url: 'http://localhost:5220/', token: 'explicit' });
    expect(r.url).toBe('http://localhost:5220');
    expect(r.token).toBe('explicit');
    expect(r.source).toBe('override');
  });

  it('process-env HOST wins over a Cloud config', () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' } });
    const r = resolveConnection({ projectDir: dir, processEnv: { UNREAL_MCP_HOST: 'http://localhost:7000' } });
    expect(r.url).toBe('http://localhost:7000');
    expect(r.source).toBe('process-env');
  });

  it('project .env HOST wins over a Cloud config', () => {
    const dir = makeProject({
      config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' },
      env: 'UNREAL_MCP_HOST=http://localhost:7001\n',
    });
    const r = resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe('http://localhost:7001');
    expect(r.source).toBe('env-file');
  });

  it('an env token still wins over the config token while the config supplies the URL', () => {
    const dir = makeProject({
      config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' },
      env: 'UNREAL_MCP_TOKEN=env-token\n',
    });
    const r = resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe('https://ai-game.dev/mcp');
    expect(r.token).toBe('env-token');
    expect(r.source).toBe('plugin-config');
  });

  it('no config file and no .env → unchanged localhost:<port> fallback', () => {
    const dir = makeProject();
    const r = resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe(`http://localhost:${generatePortFromDirectory(dir)}`);
    expect(r.token).toBeUndefined();
    expect(r.source).toBe('deterministic-port');
  });

  it('a config with no usable connection fields falls through to localhost', () => {
    const dir = makeProject({ config: { connectionMode: 'Custom' } });
    const r = resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe(`http://localhost:${generatePortFromDirectory(dir)}`);
    expect(r.source).toBe('deterministic-port');
  });
});
