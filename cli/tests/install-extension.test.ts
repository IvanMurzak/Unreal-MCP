import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { zipSync } from 'fflate';
import { installExtension } from '../src/lib/install-extension.js';
import {
  EXTENSIONS_CATALOG,
  findExtension,
  hasVersion,
  unknownExtensionMessage,
  type ExtensionDescriptor,
} from '../src/utils/extensions-catalog.js';
import {
  resolveInstallSource,
  extensionDownloadUrl,
  extensionAssetName,
  releaseTag,
  stripLeadingV,
  assertTrustedDownloadUrl,
} from '../src/utils/extension-source.js';
import {
  parseUPluginDependencies,
  readUPluginVersionName,
  enablePluginsInUProject,
} from '../src/utils/uproject-plugins.js';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';
import type { ExtensionBuildStep } from '../src/lib/types.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});
function tmp(): string {
  const d = makeTempDir();
  dirs.push(d);
  return d;
}

/** Build a local extension plugin source dir: `<dir>/<name>/<name>.uplugin` + a Source marker. */
function makeExtensionSource(
  name: string,
  opts: { version?: string; plugins?: string[]; intermediate?: boolean } = {},
): string {
  const root = tmp();
  const pluginDir = path.join(root, name);
  fs.mkdirSync(path.join(pluginDir, 'Source', name), { recursive: true });
  const uplugin: Record<string, unknown> = {
    FileVersion: 3,
    VersionName: opts.version ?? '1.0.0',
    FriendlyName: name,
    Modules: [{ Name: name, Type: 'Editor', LoadingPhase: 'Default' }],
  };
  if (opts.plugins && opts.plugins.length > 0) {
    uplugin.Plugins = opts.plugins.map((p) => ({ Name: p, Enabled: true }));
  }
  fs.writeFileSync(path.join(pluginDir, `${name}.uplugin`), JSON.stringify(uplugin, null, '\t'), 'utf-8');
  fs.writeFileSync(path.join(pluginDir, 'Source', name, `${name}.cpp`), '// marker', 'utf-8');
  if (opts.intermediate) {
    fs.mkdirSync(path.join(pluginDir, 'Intermediate', 'Build'), { recursive: true });
    fs.writeFileSync(path.join(pluginDir, 'Intermediate', 'Build', 'x.obj'), 'OBJ', 'utf-8');
    fs.mkdirSync(path.join(pluginDir, 'Binaries', 'Win64'), { recursive: true });
    fs.writeFileSync(path.join(pluginDir, 'Binaries', 'Win64', 'stale.dll'), 'DLL', 'utf-8');
  }
  return pluginDir;
}

/** A minimal UE project dir with a `.uproject`. */
function makeProject(name = 'MyGame'): string {
  const dir = tmp();
  writeUProject(dir, name, '5.7');
  return dir;
}

/** A `Response`-like stub whose body is a zip of `files`. */
function zipResponse(files: Record<string, Uint8Array>): Response {
  const bytes = zipSync(files);
  return {
    ok: true,
    status: 200,
    statusText: 'OK',
    arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
  } as unknown as Response;
}

function enc(s: string): Uint8Array {
  return new TextEncoder().encode(s);
}

// ===========================================================================
// catalog
// ===========================================================================

describe('extensions-catalog', () => {
  const sample: ExtensionDescriptor = {
    extensionId: 'com.foo.niagara',
    name: 'Niagara Tools',
    description: 'AI tools for Niagara.',
    pluginName: 'FooNiagara',
    repo: 'foo/Unreal-AI-Niagara',
    version: '1.2.0',
    minCoreVersion: '0.5.0',
    enginePlugins: ['Niagara'],
    tools: [{ name: 'niagara-create', description: 'Create a Niagara system.' }],
  };

  it('ships empty by default', () => {
    expect(EXTENSIONS_CATALOG).toHaveLength(0);
  });

  it('findExtension matches by extensionId, name, or pluginName (case-insensitive)', () => {
    const cat = [sample];
    expect(findExtension('com.foo.niagara', cat)).toBe(sample);
    expect(findExtension('NIAGARA TOOLS', cat)).toBe(sample);
    expect(findExtension('fooniagara', cat)).toBe(sample);
    expect(findExtension('nope', cat)).toBeNull();
    expect(findExtension('', cat)).toBeNull();
    expect(findExtension(undefined, cat)).toBeNull();
  });

  it('hasVersion reflects the pin', () => {
    expect(hasVersion(sample)).toBe(true);
    expect(hasVersion({ ...sample, version: null })).toBe(false);
    expect(hasVersion({ ...sample, version: '  ' })).toBe(false);
  });

  it('unknownExtensionMessage explains the empty catalog + the --source escape hatch', () => {
    const msg = unknownExtensionMessage('com.x.y', []);
    expect(msg).toContain('currently empty');
    expect(msg).toContain('--source');
  });
});

// ===========================================================================
// extension-source resolver (the pluggable channel seam)
// ===========================================================================

describe('extension-source', () => {
  it('builds a github.com release URL', () => {
    expect(stripLeadingV('v1.2.0')).toBe('1.2.0');
    expect(releaseTag('1.2.0')).toBe('v1.2.0');
    expect(extensionAssetName('FooNiagara', 'v1.2.0')).toBe('FooNiagara-1.2.0.zip');
    expect(extensionDownloadUrl('foo/Unreal-AI-Niagara', 'FooNiagara', '1.2.0')).toBe(
      'https://github.com/foo/Unreal-AI-Niagara/releases/download/v1.2.0/FooNiagara-1.2.0.zip',
    );
  });

  it('assertTrustedDownloadUrl accepts github https and rejects everything else', () => {
    expect(() => assertTrustedDownloadUrl('https://github.com/a/b/releases/download/v1/x.zip')).not.toThrow();
    expect(() => assertTrustedDownloadUrl('http://github.com/a/b')).toThrow(/only https/);
    expect(() => assertTrustedDownloadUrl('https://evil.test/a/b')).toThrow(/untrusted host/);
    expect(() => assertTrustedDownloadUrl('https://github.com.evil.test/a')).toThrow(/untrusted host/);
    expect(() => assertTrustedDownloadUrl('not a url')).toThrow(/malformed/);
  });

  it('resolveInstallSource prefers --source (local) over a release', () => {
    const s = resolveInstallSource({ source: '/some/dir', repo: 'a/b', pluginName: 'P', version: '1.0.0' });
    expect(s).toEqual({ kind: 'local', dir: '/some/dir' });
  });

  it('resolveInstallSource falls back to github-release when repo + version known', () => {
    const s = resolveInstallSource({ repo: 'a/b', pluginName: 'P', version: '1.0.0' });
    expect(s.kind).toBe('github-release');
    if (s.kind === 'github-release') {
      expect(s.url).toBe('https://github.com/a/b/releases/download/v1.0.0/P-1.0.0.zip');
      expect(s.version).toBe('1.0.0');
    }
  });

  it('resolveInstallSource throws clearly when neither a source nor a resolvable release exists', () => {
    expect(() => resolveInstallSource({ repo: null, pluginName: 'P', version: '1.0.0' })).toThrow(/no release repo/);
    expect(() => resolveInstallSource({ repo: 'a/b', pluginName: 'P', version: null })).toThrow(/No version/);
  });
});

// ===========================================================================
// uproject-plugins pure transforms
// ===========================================================================

describe('uproject-plugins', () => {
  it('parses declared (enabled) plugin deps and the version name', () => {
    const json = {
      VersionName: '2.3.4',
      Plugins: [
        { Name: 'Niagara', Enabled: true },
        { Name: 'Disabled', Enabled: false },
        { Name: 'NoFlag' },
        { Bad: 1 },
      ],
    };
    expect(parseUPluginDependencies(json)).toEqual(['Niagara', 'NoFlag']);
    expect(readUPluginVersionName(json)).toBe('2.3.4');
    expect(parseUPluginDependencies({})).toEqual([]);
    expect(readUPluginVersionName({})).toBeNull();
  });

  it('enables plugins (add + flip), is idempotent, dedupes, and preserves a trailing newline', () => {
    const base = JSON.stringify({ FileVersion: 3, Plugins: [{ Name: 'Existing', Enabled: false }] }, null, '\t') + '\n';
    const r1 = enablePluginsInUProject(base, ['MyExt', 'Existing', 'myext']);
    expect(r1.changed).toBe(true);
    expect(r1.text.endsWith('\n')).toBe(true);
    expect(r1.enabledPlugins.sort()).toEqual(['Existing', 'MyExt']);
    // addedOrFlipped lists exactly what this call switched on (MyExt appended, Existing flipped).
    expect(r1.addedOrFlipped.sort()).toEqual(['Existing', 'MyExt']);
    const parsed = JSON.parse(r1.text) as { Plugins: { Name: string; Enabled: boolean }[] };
    expect(parsed.Plugins.find((p) => p.Name === 'Existing')!.Enabled).toBe(true);

    // Idempotent: re-running makes no change.
    const r2 = enablePluginsInUProject(r1.text, ['MyExt', 'Existing']);
    expect(r2.changed).toBe(false);
    expect(r2.text).toBe(r1.text);
    expect(r2.addedOrFlipped).toEqual([]);
  });

  it('addedOrFlipped excludes plugins already enabled before the call', () => {
    const base = JSON.stringify({ FileVersion: 3, Plugins: [{ Name: 'AlreadyOn', Enabled: true }] }, null, '\t');
    const r = enablePluginsInUProject(base, ['AlreadyOn', 'NewExt']);
    expect(r.changed).toBe(true);
    // The full enabled set still includes the pre-existing plugin ...
    expect(r.enabledPlugins.sort()).toEqual(['AlreadyOn', 'NewExt']);
    // ... but addedOrFlipped reports only the newly switched-on extension.
    expect(r.addedOrFlipped).toEqual(['NewExt']);
  });

  it('creates a Plugins array when absent', () => {
    const base = JSON.stringify({ FileVersion: 3 }, null, '\t');
    const r = enablePluginsInUProject(base, ['MyExt']);
    expect(r.changed).toBe(true);
    expect(JSON.parse(r.text).Plugins).toEqual([{ Name: 'MyExt', Enabled: true }]);
  });
});

// ===========================================================================
// installExtension — local --source path
// ===========================================================================

describe('installExtension (local --source)', () => {
  it('places the plugin, enables it + the gating plugin, reports added + rebuildRequired', async () => {
    const project = makeProject();
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0', plugins: ['Niagara'], intermediate: true });

    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;

    expect(r.outcome).toBe('added');
    expect(r.changed).toBe(true);
    expect(r.rebuildRequired).toBe(true);
    expect(r.built).toBe(false);
    expect(r.sourceKind).toBe('local');
    expect(r.toVersion).toBe('1.0.0');
    expect(r.fromVersion).toBeNull();

    // Files placed, build cache excluded.
    const installed = path.join(project, 'Plugins', 'FooNiagara');
    expect(fs.existsSync(path.join(installed, 'FooNiagara.uplugin'))).toBe(true);
    expect(fs.existsSync(path.join(installed, 'Source', 'FooNiagara', 'FooNiagara.cpp'))).toBe(true);
    expect(fs.existsSync(path.join(installed, 'Intermediate'))).toBe(false);
    expect(fs.existsSync(path.join(installed, 'Binaries'))).toBe(false);

    // .uproject enabled the extension + the gating plugin.
    const upj = JSON.parse(fs.readFileSync(r.uprojectPath, 'utf-8')) as { Plugins: { Name: string; Enabled: boolean }[] };
    const enabled = upj.Plugins.filter((p) => p.Enabled).map((p) => p.Name);
    expect(enabled).toContain('FooNiagara');
    expect(enabled).toContain('Niagara');
    expect(r.enabledPlugins).toContain('Niagara');
  });

  it('is idempotent — a second identical install is a no-op already-up-to-date', async () => {
    const project = makeProject();
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0', plugins: ['Niagara'] });
    await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source });

    const uprojectPath = path.join(project, 'MyGame.uproject');
    const before = fs.readFileSync(uprojectPath, 'utf-8');
    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.outcome).toBe('already-up-to-date');
    expect(r.changed).toBe(false);
    expect(r.rebuildRequired).toBe(false);
    // No write to the .uproject.
    expect(fs.readFileSync(uprojectPath, 'utf-8')).toBe(before);
  });

  it("reports 'enabled' (not 'updated') when files are current but the .uproject lost the enable entry", async () => {
    const project = makeProject();
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0', plugins: ['Niagara'] });
    await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source });

    // Drop the extension's enable entry while the materialized files stay at the
    // same version on disk → re-install should re-enable, not "update".
    const uprojectPath = path.join(project, 'MyGame.uproject');
    const upj = JSON.parse(fs.readFileSync(uprojectPath, 'utf-8')) as { Plugins: { Name: string; Enabled: boolean }[] };
    upj.Plugins = upj.Plugins.filter((p) => p.Name !== 'FooNiagara');
    fs.writeFileSync(uprojectPath, JSON.stringify(upj, null, '\t'));

    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.outcome).toBe('enabled');
    expect(r.changed).toBe(true);
    expect(r.fromVersion).toBe('1.0.0');
    expect(r.toVersion).toBe('1.0.0');
    expect(r.message).toMatch(/Enabled existing extension FooNiagara 1\.0\.0/);
    // Only the re-enabled extension is reported, not the pre-existing Niagara.
    expect(r.enabledPlugins).toContain('FooNiagara');
    expect(r.enabledPlugins).not.toContain('Niagara');
  });

  it('updates when the source version is newer (fromVersion → toVersion)', async () => {
    const project = makeProject();
    await installExtension({
      projectDir: project,
      extensionId: 'com.foo.niagara',
      source: makeExtensionSource('FooNiagara', { version: '1.0.0' }),
    });
    const r = await installExtension({
      projectDir: project,
      extensionId: 'com.foo.niagara',
      source: makeExtensionSource('FooNiagara', { version: '2.0.0' }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.outcome).toBe('updated');
    expect(r.fromVersion).toBe('1.0.0');
    expect(r.toVersion).toBe('2.0.0');
  });

  it('--force re-materializes even when already up to date', async () => {
    const project = makeProject();
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0' });
    await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source });
    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source, force: true });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.changed).toBe(true);
  });

  it('uses a catalog descriptor (pluginName + enginePlugins) when the id resolves, with --source for files', async () => {
    const project = makeProject();
    const catalog: ExtensionDescriptor[] = [
      {
        extensionId: 'com.foo.niagara',
        name: 'Niagara Tools',
        description: 'x',
        pluginName: 'FooNiagara',
        repo: 'foo/Unreal-AI-Niagara',
        version: '1.0.0',
        minCoreVersion: null,
        enginePlugins: ['Niagara', 'PluginBrowser'],
        tools: [],
      },
    ];
    // The source .uplugin only declares Niagara; the catalog adds PluginBrowser.
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0', plugins: ['Niagara'] });
    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source, catalog });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.enabledPlugins).toEqual(expect.arrayContaining(['FooNiagara', 'Niagara', 'PluginBrowser']));
  });
});

// ===========================================================================
// installExtension — download path + failures
// ===========================================================================

describe('installExtension (download + failures)', () => {
  const catalog: ExtensionDescriptor[] = [
    {
      extensionId: 'com.foo.niagara',
      name: 'Niagara Tools',
      description: 'x',
      pluginName: 'FooNiagara',
      repo: 'foo/Unreal-AI-Niagara',
      version: '1.0.0',
      minCoreVersion: null,
      enginePlugins: [],
      tools: [],
    },
  ];

  it('downloads + extracts the plugin zip from the (injected) GitHub release', async () => {
    const project = makeProject();
    const upluginJson = JSON.stringify({ FileVersion: 3, VersionName: '1.0.0', Plugins: [{ Name: 'Niagara', Enabled: true }] });
    const fetchImpl = (async (url: string) => {
      expect(url).toBe('https://github.com/foo/Unreal-AI-Niagara/releases/download/v1.0.0/FooNiagara-1.0.0.zip');
      return zipResponse({
        'FooNiagara/FooNiagara.uplugin': enc(upluginJson),
        'FooNiagara/Source/FooNiagara/FooNiagara.cpp': enc('// marker'),
      });
    }) as unknown as typeof fetch;

    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', catalog, fetchImpl });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.outcome).toBe('added');
    expect(r.sourceKind).toBe('github-release');
    const installed = path.join(project, 'Plugins', 'FooNiagara');
    expect(fs.existsSync(path.join(installed, 'FooNiagara.uplugin'))).toBe(true);
    expect(r.enabledPlugins).toContain('Niagara');
  });

  it('passes an abort signal (download timeout) to the release fetch', async () => {
    const project = makeProject();
    const upluginJson = JSON.stringify({ FileVersion: 3, VersionName: '1.0.0' });
    let seenSignal: unknown;
    const fetchImpl = (async (_url: string, init?: { signal?: AbortSignal }) => {
      seenSignal = init?.signal;
      return zipResponse({ 'FooNiagara/FooNiagara.uplugin': enc(upluginJson) });
    }) as unknown as typeof fetch;

    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', catalog, fetchImpl });
    expect(r.kind).toBe('success');
    expect(seenSignal).toBeInstanceOf(AbortSignal);
  });

  it('fails (no throw) on an HTTP error from the release', async () => {
    const project = makeProject();
    const fetchImpl = (async () => ({ ok: false, status: 404, statusText: 'Not Found' }) as unknown as Response) as unknown as typeof fetch;
    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', catalog, fetchImpl });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.error.message).toMatch(/HTTP 404/);
  });

  it('fails (no throw) for an unknown extension with no --source', async () => {
    const project = makeProject();
    const r = await installExtension({ projectDir: project, extensionId: 'com.unknown.ext' });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.error.message).toMatch(/Unknown extension/);
  });

  it('fails (no throw) when the directory is not an Unreal project', async () => {
    const notProject = tmp();
    const source = makeExtensionSource('FooNiagara');
    const r = await installExtension({ projectDir: notProject, extensionId: 'com.foo.niagara', source });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.error.message).toMatch(/No .uproject/);
  });
});

// ===========================================================================
// installExtension — --build (UBT) path
// ===========================================================================

describe('installExtension (--build)', () => {
  it('invokes the injected build runner and reports built + no rebuildRequired', async () => {
    const project = makeProject('MyGame');
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0' });
    const steps: ExtensionBuildStep[] = [];
    const r = await installExtension({
      projectDir: project,
      extensionId: 'com.foo.niagara',
      source,
      build: true,
      engineRoot: 'C:/Engine',
      buildImpl: async (step) => {
        steps.push(step);
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.built).toBe(true);
    expect(r.rebuildRequired).toBe(false);
    expect(steps).toHaveLength(1);
    expect(steps[0].editorTarget).toBe('MyGameEditor');
    expect(steps[0].args).toContain('-WaitMutex');
    expect(steps[0].ubtPath.replace(/\\/g, '/')).toContain('C:/Engine/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe');
  });

  it('warns + falls back to rebuild-on-next-open when no engine root resolves', async () => {
    const project = makeProject();
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0' });
    const r = await installExtension({
      projectDir: project,
      extensionId: 'com.foo.niagara',
      source,
      build: true,
      resolveEngineRootImpl: () => null,
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.built).toBe(false);
    expect(r.rebuildRequired).toBe(true);
    expect(r.warnings.join(' ')).toMatch(/no engine root/i);
  });

  it('a failing build does not throw — it warns and leaves rebuildRequired true', async () => {
    const project = makeProject();
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0' });
    const r = await installExtension({
      projectDir: project,
      extensionId: 'com.foo.niagara',
      source,
      build: true,
      engineRoot: 'C:/Engine',
      buildImpl: async () => {
        throw new Error('compile error');
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.built).toBe(false);
    expect(r.rebuildRequired).toBe(true);
    expect(r.warnings.join(' ')).toMatch(/UBT build failed/);
  });
});

// ===========================================================================
// installExtension — minCoreVersion advisory
// ===========================================================================

describe('installExtension (minCoreVersion advisory)', () => {
  function catalogWithMin(min: string): ExtensionDescriptor[] {
    return [
      {
        extensionId: 'com.foo.niagara',
        name: 'Niagara Tools',
        description: 'x',
        pluginName: 'FooNiagara',
        repo: null,
        version: '1.0.0',
        minCoreVersion: min,
        enginePlugins: [],
        tools: [],
      },
    ];
  }

  it('warns when the core UnrealMCP plugin is missing', async () => {
    const project = makeProject();
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0' });
    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source, catalog: catalogWithMin('0.5.0') });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.warnings.join(' ')).toMatch(/requires core Unreal-MCP >= 0.5.0/);
  });

  it('warns when the installed core plugin is older than min', async () => {
    const project = makeProject();
    // Install a fake core plugin at 0.4.0.
    const coreDir = path.join(project, 'Plugins', 'UnrealMCP');
    fs.mkdirSync(coreDir, { recursive: true });
    fs.writeFileSync(path.join(coreDir, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '0.4.0' }), 'utf-8');
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0' });
    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source, catalog: catalogWithMin('0.10.0') });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.warnings.join(' ')).toMatch(/installed core plugin is 0.4.0/);
  });

  it('does NOT warn when the installed core plugin satisfies min', async () => {
    const project = makeProject();
    const coreDir = path.join(project, 'Plugins', 'UnrealMCP');
    fs.mkdirSync(coreDir, { recursive: true });
    fs.writeFileSync(path.join(coreDir, 'UnrealMCP.uplugin'), JSON.stringify({ VersionName: '1.0.0' }), 'utf-8');
    const source = makeExtensionSource('FooNiagara', { version: '1.0.0' });
    const r = await installExtension({ projectDir: project, extensionId: 'com.foo.niagara', source, catalog: catalogWithMin('0.5.0') });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.warnings.join(' ')).not.toMatch(/requires core Unreal-MCP/);
  });
});
