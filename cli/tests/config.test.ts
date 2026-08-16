import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import {
  resolveConnection,
  readPluginConfig,
  resolveConnectionFromPluginConfig,
  parseCustomAuthMode,
  appendMcp,
  isCloudMode,
  PLUGIN_CONFIG_RELATIVE_PATH,
} from '../src/utils/config.js';
import { generatePortFromDirectory } from '../src/utils/port.js';
import { writeProjectMarker } from '../src/utils/project-marker.js';
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

describe('appendMcp', async () => {
  it('appends /mcp to a bare base URL', async () => {
    expect(appendMcp('https://ai-game.dev')).toBe('https://ai-game.dev/mcp');
  });
  it('is idempotent when the URL already ends with /mcp', async () => {
    expect(appendMcp('https://ai-game.dev/mcp')).toBe('https://ai-game.dev/mcp');
  });
  it('handles a trailing slash after /mcp (no /mcp/mcp)', async () => {
    expect(appendMcp('https://ai-game.dev/mcp/')).toBe('https://ai-game.dev/mcp');
  });
  it('is case-insensitive on the existing /mcp segment', async () => {
    expect(appendMcp('https://ai-game.dev/MCP')).toBe('https://ai-game.dev/mcp');
  });
  it('strips trailing slashes on a bare base URL', async () => {
    expect(appendMcp('https://ai-game.dev/')).toBe('https://ai-game.dev/mcp');
  });
});

describe('readPluginConfig', async () => {
  it('returns undefined when the file is absent', async () => {
    const dir = makeProject();
    expect(readPluginConfig(dir)).toBeUndefined();
  });
  it('returns undefined for malformed JSON (does not throw)', async () => {
    const dir = makeProject({ config: '{ not json' });
    expect(readPluginConfig(dir)).toBeUndefined();
  });
  it('tolerates a UTF-8 BOM', async () => {
    const dir = makeProject({ config: '﻿' + JSON.stringify({ connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev' }) });
    const cfg = readPluginConfig(dir);
    expect(cfg?.connectionMode).toBe('Cloud');
    expect(cfg?.cloudUrl).toBe('https://ai-game.dev');
  });
});

describe('resolveConnectionFromPluginConfig', async () => {
  it('Cloud → appendMcp(cloudUrl) + cloudToken', async () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'ct' });
    expect(r.url).toBe('https://ai-game.dev/mcp');
    expect(r.token).toBe('ct');
  });
  it('Custom + authOption Required → host + token', async () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', authOption: 'Required', token: 'tk' });
    expect(r.url).toBe('http://localhost:9001');
    expect(r.token).toBe('tk');
  });
  it('Custom + authOption None → token undefined', async () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', authOption: 'None', token: 'tk' });
    expect(r.url).toBe('http://localhost:9001');
    expect(r.token).toBeUndefined();
  });

  // Defect C — the current plugin writes authOption "Token"; the retired `'required'` gate
  // dropped the token. The gate now keys on Token (with Required as the legacy migration alias).
  it('Custom + authOption Token → token sent (defect C: the retired required-gate is gone)', async () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', authOption: 'Token', token: 'tk' });
    expect(r.token).toBe('tk');
  });
  it('Custom + authOption Oauth → token undefined (native OAuth sends no static bearer)', async () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', authOption: 'Oauth', token: 'tk' });
    expect(r.token).toBeUndefined();
  });
  it('Custom + legacy authOption Required → token sent (g5 migration → Token)', async () => {
    const r = resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', authOption: 'Required', token: 'tk' });
    expect(r.token).toBe('tk');
  });
  it('Custom + missing/unrecognized authOption → token undefined (defaults to None)', async () => {
    expect(resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', token: 'tk' }).token).toBeUndefined();
    expect(resolveConnectionFromPluginConfig({ connectionMode: 'Custom', host: 'http://localhost:9001', authOption: 'bogus', token: 'tk' }).token).toBeUndefined();
  });

  it('isCloudMode is case-insensitive', async () => {
    expect(isCloudMode({ connectionMode: 'cloud' })).toBe(true);
    expect(isCloudMode({ connectionMode: 'Custom' })).toBe(false);
    expect(isCloudMode({})).toBe(false);
  });
});

describe('resolveConnection — plugin-config layer (issue #71)', async () => {
  it('Cloud config → url ends with /mcp, token = cloudToken, source plugin-config', async () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' } });
    const r = await resolveConnection({ projectDir: dir });
    expect(r.url).toBe('https://ai-game.dev/mcp');
    expect(r.token).toBe('cloud-secret');
    expect(r.source).toBe('plugin-config');
  });

  it('Cloud config whose cloudUrl already ends /mcp → single /mcp', async () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev/mcp', cloudToken: 't' } });
    expect((await resolveConnection({ projectDir: dir })).url).toBe('https://ai-game.dev/mcp');
  });

  it('Cloud config whose cloudUrl ends /mcp/ → single /mcp', async () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev/mcp/', cloudToken: 't' } });
    expect((await resolveConnection({ projectDir: dir })).url).toBe('https://ai-game.dev/mcp');
  });

  it('Custom config + authOption Required → host + token', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost:9100', authOption: 'Required', token: 'tk' } });
    const r = await resolveConnection({ projectDir: dir });
    expect(r.url).toBe('http://localhost:9100');
    expect(r.token).toBe('tk');
    expect(r.source).toBe('plugin-config');
  });

  it('Custom config + authOption None → token undefined', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost:9100', authOption: 'None', token: 'tk' } });
    const r = await resolveConnection({ projectDir: dir });
    expect(r.url).toBe('http://localhost:9100');
    expect(r.token).toBeUndefined();
  });

  it('explicit --url override still wins over a Cloud config', async () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' } });
    const r = await resolveConnection({ projectDir: dir, url: 'http://localhost:5220/', token: 'explicit' });
    expect(r.url).toBe('http://localhost:5220');
    expect(r.token).toBe('explicit');
    expect(r.source).toBe('override');
  });

  it('process-env HOST wins over a Cloud config', async () => {
    const dir = makeProject({ config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' } });
    const r = await resolveConnection({ projectDir: dir, processEnv: { UNREAL_MCP_HOST: 'http://localhost:7000' } });
    expect(r.url).toBe('http://localhost:7000');
    expect(r.source).toBe('process-env');
  });

  it('project .env HOST wins over a Cloud config', async () => {
    const dir = makeProject({
      config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' },
      env: 'UNREAL_MCP_HOST=http://localhost:7001\n',
    });
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe('http://localhost:7001');
    expect(r.source).toBe('env-file');
  });

  it('an env token still wins over the config token while the config supplies the URL', async () => {
    const dir = makeProject({
      config: { connectionMode: 'Cloud', cloudUrl: 'https://ai-game.dev', cloudToken: 'cloud-secret' },
      env: 'UNREAL_MCP_TOKEN=env-token\n',
    });
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe('https://ai-game.dev/mcp');
    expect(r.token).toBe('env-token');
    expect(r.source).toBe('plugin-config');
  });

  it('no config file and no .env → unchanged localhost:<port> fallback', async () => {
    const dir = makeProject();
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe(`http://localhost:${generatePortFromDirectory(dir)}`);
    expect(r.token).toBeUndefined();
    expect(r.source).toBe('deterministic-port');
  });

  it('a config with no usable connection fields falls through to localhost', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom' } });
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe(`http://localhost:${generatePortFromDirectory(dir)}`);
    expect(r.source).toBe('deterministic-port');
  });

  // Defect C wired through resolveConnection: the current-plugin `Token` config sends the token.
  it('Custom config + authOption Token → token sent via the plugin-config layer (defect C)', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost:9100', authOption: 'Token', token: 'tk' } });
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.token).toBe('tk');
    expect(r.source).toBe('plugin-config');
  });
});

describe('parseCustomAuthMode (defect C — token-mode gate)', async () => {
  it('maps None/Token/Oauth case-insensitively', async () => {
    expect(parseCustomAuthMode('None')).toBe('none');
    expect(parseCustomAuthMode('token')).toBe('token');
    expect(parseCustomAuthMode('OAUTH')).toBe('oauth');
  });
  it('migrates the retired Required → token', async () => {
    expect(parseCustomAuthMode('Required')).toBe('token');
    expect(parseCustomAuthMode('required')).toBe('token');
  });
  it('defaults absent / unrecognized to none (no bearer)', async () => {
    expect(parseCustomAuthMode(undefined)).toBe('none');
    expect(parseCustomAuthMode('')).toBe('none');
    expect(parseCustomAuthMode('bogus')).toBe('none');
  });
});

describe('resolveConnection — Custom-mode loopback port resolution (defect D / D21)', async () => {
  it('port-less loopback host (http://localhost) → derived port, NOT verbatim :80', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost', authOption: 'None' } });
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe(`http://localhost:${generatePortFromDirectory(dir)}`);
    expect(r.source).toBe('plugin-config');
  });

  it('a typed loopback port with no marker is kept (level 2 beats the derivation)', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost:8080', authOption: 'None' } });
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe('http://localhost:8080');
    expect(r.source).toBe('plugin-config');
  });

  it('a marker portOverride wins over a legacy :8080 host (verbatim :8080 no longer used)', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost:8080', authOption: 'None' } });
    writeProjectMarker(dir, { portOverride: 21234 });
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe('http://localhost:21234');
  });

  it('a marker portOverride also rewrites a port-less loopback host', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://localhost', authOption: 'None' } });
    writeProjectMarker(dir, { portOverride: 25000 });
    expect((await resolveConnection({ projectDir: dir, processEnv: {} })).url).toBe('http://localhost:25000');
  });

  it('a NON-loopback Custom host is used verbatim (the local server binds no LAN authority)', async () => {
    const dir = makeProject({ config: { connectionMode: 'Custom', host: 'http://192.168.1.5:8080', authOption: 'None' } });
    expect((await resolveConnection({ projectDir: dir, processEnv: {} })).url).toBe('http://192.168.1.5:8080');
  });

  it('the deterministic fallback honors a marker portOverride when there is no host', async () => {
    const dir = makeProject();
    writeProjectMarker(dir, { portOverride: 26543 });
    const r = await resolveConnection({ projectDir: dir, processEnv: {} });
    expect(r.url).toBe('http://localhost:26543');
    expect(r.source).toBe('deterministic-port');
  });
});
