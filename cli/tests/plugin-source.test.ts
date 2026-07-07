import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { zipSync, strToU8 } from 'fflate';
import {
  corePluginAssetName,
  corePluginDownloadUrl,
  findUPluginFile,
  resolveLocalPluginRoot,
  resolvePluginSource,
} from '../src/lib/plugin-source.js';
import { PACKAGE_VERSION } from '../src/version.js';
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

function zipResponse(entries: Record<string, Uint8Array>): typeof fetch {
  const bytes = zipSync(entries);
  return (async () =>
    ({
      ok: true,
      status: 200,
      statusText: 'OK',
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    }) as unknown as Response) as typeof fetch;
}

describe('core plugin release helpers', () => {
  it('builds the version-matched core plugin asset name + URL', () => {
    expect(corePluginAssetName('0.6.2')).toBe('unreal-mcp-plugin-0.6.2.zip');
    expect(corePluginDownloadUrl('0.6.2')).toBe(
      'https://github.com/IvanMurzak/Unreal-MCP/releases/download/v0.6.2/unreal-mcp-plugin-0.6.2.zip',
    );
  });
});

describe('resolveLocalPluginRoot / findUPluginFile', () => {
  it('accepts the plugin dir directly', () => {
    const dir = tmp();
    fs.writeFileSync(path.join(dir, 'UnrealMCP.uplugin'), '{}', 'utf-8');
    expect(resolveLocalPluginRoot(dir)).toBe(dir);
  });

  it('accepts a parent dir that contains the plugin', () => {
    const dir = tmp();
    const plugin = path.join(dir, 'nested', 'UnrealMCP');
    fs.mkdirSync(plugin, { recursive: true });
    fs.writeFileSync(path.join(plugin, 'UnrealMCP.uplugin'), '{}', 'utf-8');
    expect(resolveLocalPluginRoot(dir)).toBe(plugin);
    expect(findUPluginFile(dir)).toBe(path.join(plugin, 'UnrealMCP.uplugin'));
  });
});

describe('resolvePluginSource', () => {
  it('downloads + extracts the core plugin release when no local source is supplied', async () => {
    const result = await resolvePluginSource({
      fetchImpl: zipResponse({
        'UnrealMCP.uplugin': strToU8(JSON.stringify({ VersionName: PACKAGE_VERSION })),
        'Source/marker.txt': strToU8('downloaded'),
      }),
    });
    try {
      expect(result.sourceKind).toBe('github-release');
      expect(fs.existsSync(path.join(result.pluginSourceDir, 'UnrealMCP.uplugin'))).toBe(true);
      expect(fs.existsSync(path.join(result.pluginSourceDir, 'Source', 'marker.txt'))).toBe(true);
    } finally {
      result.cleanup();
    }
  });

  it('rejects zip-slip paths in the downloaded archive', async () => {
    await expect(
      resolvePluginSource({
        fetchImpl: zipResponse({
          '../escape/UnrealMCP.uplugin': strToU8('{}'),
        }),
      }),
    ).rejects.toThrow(/suspicious zip entry/i);
  });
});
