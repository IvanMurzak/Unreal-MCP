import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { zipSync, strToU8 } from 'fflate';
import {
  corePluginSourceAssetName,
  corePluginSourceDownloadUrl,
  corePluginSourceSignatureAssetName,
  corePluginSourceSignatureUrl,
  findUPluginFile,
  resolveLocalPluginRoot,
  resolvePluginSource,
} from '../src/lib/plugin-source.js';
import { PACKAGE_VERSION } from '../src/version.js';
import { makeTempDir, rmTempDir } from './helpers.js';
import { makeMinisignKeypair, type MinisignTestKeypair } from './minisign-fixture.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

/**
 * URL-aware fake fetch: serves the zip bytes for the zip URL and the `.minisig`
 * text for the signature URL. By default the signature is over the SAME zip bytes
 * (a valid signature). `signBytes` signs different bytes (a tampered-zip case);
 * `sigStatus` forces the signature fetch to fail (missing-signature case).
 */
function zipAndSigResponse(
  entries: Record<string, Uint8Array>,
  kp: MinisignTestKeypair,
  opts: { signBytes?: Uint8Array; sigStatus?: number; calls?: string[] } = {},
): typeof fetch {
  const bytes = zipSync(entries);
  const sigText = kp.sign(opts.signBytes ?? bytes);
  return (async (url: string) => {
    opts.calls?.push(url);
    if (url.endsWith('.minisig')) {
      if (opts.sigStatus && opts.sigStatus !== 200) {
        return { ok: false, status: opts.sigStatus, statusText: 'Not Found', text: async () => '' } as unknown as Response;
      }
      return { ok: true, status: 200, statusText: 'OK', text: async () => sigText } as unknown as Response;
    }
    return {
      ok: true,
      status: 200,
      statusText: 'OK',
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    } as unknown as Response;
  }) as typeof fetch;
}

describe('core plugin release helpers', () => {
  it('builds the version-matched core plugin asset name + URL', () => {
    expect(corePluginSourceAssetName('0.6.2')).toBe('unreal-mcp-plugin-source-0.6.2.zip');
    expect(corePluginSourceDownloadUrl('0.6.2')).toBe(
      'https://github.com/IvanMurzak/Unreal-MCP/releases/download/v0.6.2/unreal-mcp-plugin-source-0.6.2.zip',
    );
  });

  it('builds the detached signature sibling asset name + URL', () => {
    expect(corePluginSourceSignatureAssetName('0.6.2')).toBe('unreal-mcp-plugin-source-0.6.2.zip.minisig');
    expect(corePluginSourceSignatureUrl('0.6.2')).toBe(
      'https://github.com/IvanMurzak/Unreal-MCP/releases/download/v0.6.2/unreal-mcp-plugin-source-0.6.2.zip.minisig',
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

describe('resolvePluginSource — download + signature gate', () => {
  it('downloads, VERIFIES the signature, and extracts the core plugin release', async () => {
    const kp = makeMinisignKeypair();
    const result = await resolvePluginSource({
      publicKeyOverride: kp.publicKeyLine,
      fetchImpl: zipAndSigResponse(
        {
          'UnrealMCP/UnrealMCP.uplugin': strToU8(JSON.stringify({ VersionName: PACKAGE_VERSION, Installed: false })),
          'UnrealMCP/Source/marker.txt': strToU8('downloaded'),
        },
        kp,
      ),
    });
    try {
      expect(result.sourceKind).toBe('github-release');
      expect(fs.existsSync(path.join(result.pluginSourceDir, 'UnrealMCP.uplugin'))).toBe(true);
      expect(fs.existsSync(path.join(result.pluginSourceDir, 'Source', 'marker.txt'))).toBe(true);
    } finally {
      result.cleanup();
    }
  });

  it('REJECTS a tampered zip whose bytes do not match the signature (never extracts)', async () => {
    const kp = makeMinisignKeypair();
    await expect(
      resolvePluginSource({
        publicKeyOverride: kp.publicKeyLine,
        fetchImpl: zipAndSigResponse(
          { 'UnrealMCP/UnrealMCP.uplugin': strToU8('{}') },
          kp,
          { signBytes: strToU8('a totally different payload the signature was made over') },
        ),
      }),
    ).rejects.toThrow(/tampered|did not match/i);
  });

  it('REJECTS an unsigned release (the .minisig asset is missing)', async () => {
    const kp = makeMinisignKeypair();
    await expect(
      resolvePluginSource({
        publicKeyOverride: kp.publicKeyLine,
        fetchImpl: zipAndSigResponse({ 'UnrealMCP/UnrealMCP.uplugin': strToU8('{}') }, kp, { sigStatus: 404 }),
      }),
    ).rejects.toThrow(/could not download its signature/i);
  });

  it('FAILS CLOSED when the pinned signing key is un-provisioned (default sentinel key)', async () => {
    const kp = makeMinisignKeypair();
    await expect(
      // No publicKeyOverride → the baked-in sentinel key → never install unverified.
      resolvePluginSource({
        fetchImpl: zipAndSigResponse({ 'UnrealMCP/UnrealMCP.uplugin': strToU8('{}') }, kp),
      }),
    ).rejects.toThrow(/signing key is not provisioned/i);
  });

  it('rejects a downloaded packaged plugin descriptor that pins EngineVersion (after verify)', async () => {
    const kp = makeMinisignKeypair();
    await expect(
      resolvePluginSource({
        publicKeyOverride: kp.publicKeyLine,
        fetchImpl: zipAndSigResponse(
          { 'UnrealMCP/UnrealMCP.uplugin': strToU8(JSON.stringify({ VersionName: PACKAGE_VERSION, EngineVersion: '5.7.0' })) },
          kp,
        ),
      }),
    ).rejects.toThrow(/EngineVersion/i);
  });

  it('rejects zip-slip paths in the downloaded archive (after verify)', async () => {
    const kp = makeMinisignKeypair();
    await expect(
      resolvePluginSource({
        publicKeyOverride: kp.publicKeyLine,
        fetchImpl: zipAndSigResponse({ '../escape/UnrealMCP.uplugin': strToU8('{}') }, kp),
      }),
    ).rejects.toThrow(/suspicious zip entry/i);
  });

  it('a local --plugin-source dir is trusted and skips signature verification', async () => {
    const dir = tmp();
    fs.writeFileSync(path.join(dir, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.1.0' }), 'utf-8');
    const result = await resolvePluginSource({ pluginSourceDir: dir });
    expect(result.sourceKind).toBe('local');
    expect(result.pluginSourceDir).toBe(dir);
  });

  it('the --version escape hatch downloads (and verifies) the requested release, not the CLI version', async () => {
    const kp = makeMinisignKeypair();
    const calls: string[] = [];
    const result = await resolvePluginSource({
      version: '9.9.9',
      publicKeyOverride: kp.publicKeyLine,
      fetchImpl: zipAndSigResponse(
        { 'UnrealMCP/UnrealMCP.uplugin': strToU8(JSON.stringify({ VersionName: '9.9.9' })) },
        kp,
        { calls },
      ),
    });
    try {
      expect(result.sourceKind).toBe('github-release');
      // Both the zip and its .minisig were fetched under the OVERRIDDEN version tag.
      expect(calls.some((u) => u.endsWith('/v9.9.9/unreal-mcp-plugin-source-9.9.9.zip'))).toBe(true);
      expect(calls.some((u) => u.endsWith('/v9.9.9/unreal-mcp-plugin-source-9.9.9.zip.minisig'))).toBe(true);
    } finally {
      result.cleanup();
    }
  });
});
