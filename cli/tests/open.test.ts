import { describe, it, expect, afterEach } from 'vitest';
import { openProject, buildOpenEnv, _pollAndDismissStartupDialogsForTests } from '../src/lib/open.js';
import type { ProgressEvent } from '../src/lib/types.js';
import { editorBinaryPath } from '../src/utils/engine.js';
import type { EngineInstallation } from '../src/utils/launcher.js';
import * as fs from 'fs';
import * as path from 'path';
import { makeTempDir, rmTempDir, writeUProject } from './helpers.js';

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
 * Build a fake engine install whose UnrealEditor binary actually exists on
 * disk (for the host platform), so resolveEngine's existence check passes
 * deterministically regardless of what is installed on the test machine.
 */
function fakeEngine(association: string): EngineInstallation {
  const root = tmp();
  const bin = editorBinaryPath(root, process.platform);
  fs.mkdirSync(path.dirname(bin), { recursive: true });
  fs.writeFileSync(bin, '', 'utf-8');
  return { appName: `UE_${association}`, appVersion: `${association}.0`, installLocation: root, engineAssociation: association };
}

function writeBlueprintProject(dir: string, name: string, engineAssociation = '5.7'): string {
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, `${name}.uproject`);
  fs.writeFileSync(
    file,
    JSON.stringify(
      {
        FileVersion: 3,
        EngineAssociation: engineAssociation,
      },
      null,
      '\t',
    ),
    'utf-8',
  );
  return file;
}

function writeNativeSourceLayout(dir: string, name: string): void {
  const sourceDir = path.join(dir, 'Source', name);
  fs.mkdirSync(sourceDir, { recursive: true });
  fs.writeFileSync(path.join(sourceDir, `${name}.Build.cs`), '// build', 'utf-8');
  fs.writeFileSync(path.join(dir, 'Source', `${name}.Target.cs`), '// target', 'utf-8');
  fs.writeFileSync(path.join(dir, 'Source', `${name}Editor.Target.cs`), '// editor target', 'utf-8');
}

function writeInstalledPlugin(dir: string, opts?: { precompiled?: boolean }): void {
  const pluginDir = path.join(dir, 'Plugins', 'UnrealMCP');
  fs.mkdirSync(pluginDir, { recursive: true });
  fs.writeFileSync(path.join(pluginDir, 'UnrealMCP.uplugin'), JSON.stringify({ FileVersion: 3 }), 'utf-8');
  if (!opts?.precompiled) return;

  const { platformDir, runtimeExt } = currentPlatformBinaryLayout();
  const binariesDir = path.join(pluginDir, 'Binaries', platformDir);
  fs.mkdirSync(binariesDir, { recursive: true });
  fs.writeFileSync(path.join(binariesDir, 'UnrealEditor.modules'), '{}', 'utf-8');
  fs.writeFileSync(path.join(binariesDir, `UnrealEditor-UnrealMcpRuntime.${runtimeExt}`), '', 'utf-8');
  fs.writeFileSync(path.join(binariesDir, `UnrealEditor-UnrealMcpEditor.${runtimeExt}`), '', 'utf-8');
}

function currentPlatformBinaryLayout(): { platformDir: string; runtimeExt: string } {
  switch (process.platform) {
    case 'darwin':
      return { platformDir: 'Mac', runtimeExt: 'dylib' };
    case 'linux':
      return { platformDir: 'Linux', runtimeExt: 'so' };
    default:
      return { platformDir: 'Win64', runtimeExt: 'dll' };
  }
}

describe('buildOpenEnv', () => {
  it('maps options to UNREAL_MCP_* vars', () => {
    const env = buildOpenEnv({ host: 'http://h', token: 't', keepConnected: true, auth: 'required', transport: 'http' });
    expect(env).toEqual({
      UNREAL_MCP_HOST: 'http://h',
      UNREAL_MCP_TOKEN: 't',
      UNREAL_MCP_KEEP_CONNECTED: 'true',
      UNREAL_MCP_AUTH_OPTION: 'required',
      UNREAL_MCP_TRANSPORT: 'http',
      // host present + no explicit mode => Custom inferred (see emission matrix below)
      UNREAL_MCP_CONNECTION_MODE: 'Custom',
    });
  });

  it('returns undefined for --no-connect', () => {
    expect(buildOpenEnv({ noConnect: true, host: 'http://h' })).toBeUndefined();
  });

  it('throws on bad auth/transport enums', () => {
    expect(() => buildOpenEnv({ auth: 'bogus' as never })).toThrow();
    expect(() => buildOpenEnv({ transport: 'bogus' as never })).toThrow();
  });

  describe('UNREAL_MCP_CONNECTION_MODE emission matrix', () => {
    it('emits an explicit Custom mode', () => {
      expect(buildOpenEnv({ connectionMode: 'Custom' })).toEqual({
        UNREAL_MCP_CONNECTION_MODE: 'Custom',
      });
    });

    it('emits an explicit Cloud mode', () => {
      expect(buildOpenEnv({ connectionMode: 'Cloud' })).toEqual({
        UNREAL_MCP_CONNECTION_MODE: 'Cloud',
      });
    });

    it('infers Custom from a host when no mode is given', () => {
      expect(buildOpenEnv({ host: 'http://localhost:5220' })).toEqual({
        UNREAL_MCP_HOST: 'http://localhost:5220',
        UNREAL_MCP_CONNECTION_MODE: 'Custom',
      });
    });

    it('lets an explicit mode win over the host-presence inference', () => {
      expect(buildOpenEnv({ host: 'http://localhost:5220', connectionMode: 'Cloud' })).toEqual({
        UNREAL_MCP_HOST: 'http://localhost:5220',
        UNREAL_MCP_CONNECTION_MODE: 'Cloud',
      });
    });

    it('omits the var when neither host nor mode is given', () => {
      const env = buildOpenEnv({ token: 't' });
      expect(env).toEqual({ UNREAL_MCP_TOKEN: 't' });
      expect(env?.UNREAL_MCP_CONNECTION_MODE).toBeUndefined();
    });

    it('drops the var under --no-connect even with a host (no inference)', () => {
      expect(buildOpenEnv({ noConnect: true, host: 'http://h' })).toBeUndefined();
      expect(
        buildOpenEnv({ noConnect: true, host: 'http://h', connectionMode: 'Custom' }),
      ).toBeUndefined();
    });
  });
});

describe('openProject', () => {
  const expectsDesktopBuild =
    process.platform === 'win32' || process.platform === 'darwin' || process.platform === 'linux';

  function expectedBuildPathFragment(): string {
    switch (process.platform) {
      case 'win32':
        return path.join('Engine', 'Binaries', 'DotNET', 'UnrealBuildTool');
      case 'darwin':
        return path.join('Engine', 'Build', 'BatchFiles', 'Mac', 'Build.sh');
      case 'linux':
        return path.join('Engine', 'Build', 'BatchFiles', 'Linux', 'Build.sh');
      default:
        return 'Engine';
    }
  }

  it('resolves the engine and spawns the editor (injected engines + spawn)', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    let spawnedWith: { editorPath: string; args: string[]; env: NodeJS.ProcessEnv } | null = null;
    const r = await openProject({
      projectDir: dir,
      autoDismissStartupDialogs: false,
      host: 'http://localhost:5220',
      enginesImpl: () => [fakeEngine('5.7')],
      spawnImpl: (editorPath, args, env) => {
        spawnedWith = { editorPath, args, env };
        return { pid: 4242 };
      },
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.editorPid).toBe(4242);
    expect(r.editorPath).toContain('UnrealEditor');
    expect(r.envVars['UNREAL_MCP_HOST']).toBe('http://localhost:5220');
    expect(spawnedWith).not.toBeNull();
    expect(spawnedWith!.env['UNREAL_MCP_HOST']).toBe('http://localhost:5220');
  });

  it('dismisses known Unreal startup dialogs after launch on Windows', async () => {
    const dir = tmp();
    writeBlueprintProject(dir, 'MyGame', '5.7');
    writeInstalledPlugin(dir, { precompiled: true });
    const progress: ProgressEvent[] = [];
    let probeCalls = 0;
    const r = await openProject({
      projectDir: dir,
      enginesImpl: () => [fakeEngine('5.7')],
      spawnImpl: () => ({ pid: 55 }),
      dismissStartupDialogImpl: async () => {
        probeCalls += 1;
        return probeCalls === 1
          ? { kind: 'dismissed', dialog: 'missing-modules', button: 'Yes' }
          : { kind: 'not-found' };
      },
      startupDismissTimeoutMs: 250,
      startupDismissPollIntervalMs: 50,
      onProgress: (event) => {
        progress.push(event);
      },
    });
    expect(r.kind).toBe('success');
    if (expectsDesktopBuild) {
      expect(probeCalls).toBeGreaterThanOrEqual(2);
      expect(
        progress.some(
          (event) =>
            event.phase === 'startup-dialog-dismissed' &&
            event.dialog === 'missing-modules' &&
            event.button === 'Yes',
        ),
      ).toBe(true);
    } else {
      expect(probeCalls).toBe(0);
    }
  });

  it('skips startup-dialog auto-dismiss when disabled explicitly', async () => {
    const dir = tmp();
    writeBlueprintProject(dir, 'MyGame', '5.7');
    writeInstalledPlugin(dir, { precompiled: true });
    let probeCalls = 0;
    const r = await openProject({
      projectDir: dir,
      autoDismissStartupDialogs: false,
      enginesImpl: () => [fakeEngine('5.7')],
      spawnImpl: () => ({ pid: 56 }),
      dismissStartupDialogImpl: async () => {
        probeCalls += 1;
        return { kind: 'dismissed', dialog: 'missing-modules', button: 'Yes' };
      },
    });
    expect(r.kind).toBe('success');
    expect(probeCalls).toBe(0);
  });

  it('builds before launch for native projects on desktop platforms', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    writeNativeSourceLayout(dir, 'MyGame');
    let built: { ubtPath: string; args: string[]; editorTarget: string } | null = null;
    let spawned = false;
    const r = await openProject({
      projectDir: dir,
      autoDismissStartupDialogs: false,
      enginesImpl: () => [fakeEngine('5.7')],
      buildImpl: async (step) => {
        built = { ubtPath: step.ubtPath, args: step.args, editorTarget: step.editorTarget };
      },
      spawnImpl: () => {
        spawned = true;
        return { pid: 91 };
      },
    });
    expect(r.kind).toBe('success');
    if (expectsDesktopBuild) {
      expect(built).not.toBeNull();
      expect(built?.editorTarget).toBe('MyGameEditor');
      expect(built?.ubtPath).toContain(expectedBuildPathFragment());
      expect(built?.args).toContain('-WaitMutex');
    } else {
      expect(built).toBeNull();
    }
    expect(spawned).toBe(true);
  });

  it('builds via UBT before launch for source-installed UnrealMCP on blueprint projects', async () => {
    const dir = tmp();
    writeBlueprintProject(dir, 'MyGame', '5.7');
    writeInstalledPlugin(dir);
    let buildCount = 0;
    const r = await openProject({
      projectDir: dir,
      autoDismissStartupDialogs: false,
      enginesImpl: () => [fakeEngine('5.7')],
      buildImpl: async () => {
        buildCount += 1;
      },
      spawnImpl: () => ({ pid: 8 }),
    });
    expect(r.kind).toBe('success');
    expect(buildCount).toBe(expectsDesktopBuild ? 1 : 0);
  });

  it('skips the pre-launch build when disabled explicitly', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    writeNativeSourceLayout(dir, 'MyGame');
    let buildCalled = false;
    const r = await openProject({
      projectDir: dir,
      build: false,
      autoDismissStartupDialogs: false,
      enginesImpl: () => [fakeEngine('5.7')],
      buildImpl: async () => {
        buildCalled = true;
      },
      spawnImpl: () => ({ pid: 13 }),
    });
    expect(r.kind).toBe('success');
    expect(buildCalled).toBe(false);
  });

  it('skips the pre-launch build for blueprint projects with precompiled UnrealMCP binaries', async () => {
    const dir = tmp();
    writeBlueprintProject(dir, 'MyGame', '5.7');
    writeInstalledPlugin(dir, { precompiled: true });
    let buildCalled = false;
    const r = await openProject({
      projectDir: dir,
      autoDismissStartupDialogs: false,
      enginesImpl: () => [fakeEngine('5.7')],
      buildImpl: async () => {
        buildCalled = true;
      },
      spawnImpl: () => ({ pid: 21 }),
    });
    expect(r.kind).toBe('success');
    expect(buildCalled).toBe(false);
  });

  it('fails before launch when the pre-launch UBT build fails', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    writeNativeSourceLayout(dir, 'MyGame');
    let spawned = false;
    const r = await openProject({
      projectDir: dir,
      autoDismissStartupDialogs: false,
      enginesImpl: () => [fakeEngine('5.7')],
      buildImpl: async () => {
        throw new Error('compile broke');
      },
      spawnImpl: () => {
        spawned = true;
        return { pid: 34 };
      },
    });
    if (expectsDesktopBuild) {
      expect(r.kind).toBe('failure');
      if (r.kind === 'failure') expect(r.errorMessage).toContain('Pre-launch build failed: compile broke');
      expect(spawned).toBe(false);
    } else {
      expect(r.kind).toBe('success');
      expect(spawned).toBe(true);
    }
  });

  it('warns that connection options are ignored under --no-connect', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '5.7');
    const r = await openProject({
      projectDir: dir,
      noConnect: true,
      autoDismissStartupDialogs: false,
      host: 'http://h',
      auth: 'required',
      enginesImpl: () => [fakeEngine('5.7')],
      spawnImpl: () => ({ pid: 7 }),
    });
    expect(r.kind).toBe('success');
    if (r.kind !== 'success') return;
    expect(r.envVars).toEqual({}); // nothing propagated under --no-connect
    expect(r.warnings.some((w) => /ignored under --no-connect/.test(w))).toBe(true);
  });

  it('fails when no .uproject is present', async () => {
    const dir = tmp();
    const r = await openProject({ projectDir: dir, enginesImpl: () => [fakeEngine('5.7')] });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.errorMessage).toContain('.uproject');
  });

  it('fails when the engine association is not installed', async () => {
    const dir = tmp();
    writeUProject(dir, 'MyGame', '4.27');
    const r = await openProject({ projectDir: dir, enginesImpl: () => [fakeEngine('5.7')], spawnImpl: () => ({ pid: 1 }) });
    expect(r.kind).toBe('failure');
  });
});

describe('startup-dialog dismiss polling', () => {
  it('records the dismiss event and exits after a quiet settle window', async () => {
    const progress: ProgressEvent[] = [];
    let calls = 0;
    await _pollAndDismissStartupDialogsForTests({
      timeoutMs: 500,
      intervalMs: 10,
      noDialogGraceMs: 20,
      settleAfterDismissMs: 30,
      platform: 'win32',
      warnings: [],
      onProgress: (event) => {
        progress.push(event);
      },
      probe: async () => {
        calls += 1;
        return calls === 1
          ? { kind: 'dismissed', dialog: 'plugin-incompatible', button: 'Yes' }
          : { kind: 'not-found' };
      },
    });
    expect(
      progress.some(
        (event) =>
          event.phase === 'startup-dialog-dismissed' &&
          event.dialog === 'plugin-incompatible' &&
          event.button === 'Yes',
      ),
    ).toBe(true);
    expect(calls).toBeGreaterThanOrEqual(2);
  });

  it('deduplicates repeated probe errors into one warning', async () => {
    const warnings: string[] = [];
    await _pollAndDismissStartupDialogsForTests({
      timeoutMs: 60,
      intervalMs: 10,
      noDialogGraceMs: 20,
      settleAfterDismissMs: 20,
      platform: 'win32',
      warnings,
      onProgress: undefined,
      probe: async () => ({ kind: 'error', message: 'probe broke' }),
    });
    expect(warnings.filter((warning) => warning.includes('probe broke'))).toHaveLength(1);
  });
});
