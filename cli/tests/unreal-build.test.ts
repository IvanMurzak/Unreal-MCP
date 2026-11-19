import { describe, it, expect } from 'vitest';
import {
  corePluginNeedsBuild,
  projectHasNativeCode,
  resolveDesktopBuildStep,
} from '../src/utils/unreal-build.js';
import { makeTempDir, rmTempDir } from './helpers.js';
import * as fs from 'fs';
import * as path from 'path';

describe('unreal-build', () => {
  it('resolves the Windows desktop build step of a C++ project to its <Project>Editor target', () => {
    const dir = makeTempDir();
    try {
      fs.mkdirSync(path.join(dir, 'Source'), { recursive: true });
      fs.writeFileSync(path.join(dir, 'Source', 'MyGameEditor.Target.cs'), '// target', 'utf-8');
      const uprojectPath = path.join(dir, 'MyGame.uproject');
      const step = resolveDesktopBuildStep({ platform: 'win32', engineRoot: 'C:/UE', projectName: 'MyGame', uprojectPath });
      expect(step.ubtPath.replace(/\\/g, '/')).toContain('C:/UE/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe');
      expect(step.editorTarget).toBe('MyGameEditor');
      expect(step.args).toEqual(['MyGameEditor', 'Win64', 'Development', `-project=${uprojectPath}`, '-WaitMutex']);
    } finally {
      rmTempDir(dir);
    }
  });

  it('builds a content-only (Blueprint-only) project with the engine UnrealEditor target and -project=', () => {
    const dir = makeTempDir();
    try {
      // A Source/ folder without target rules (e.g. leftovers) is still content-only.
      fs.mkdirSync(path.join(dir, 'Source'), { recursive: true });
      const uprojectPath = path.join(dir, 'MyGame.uproject');
      const step = resolveDesktopBuildStep({ platform: 'win32', engineRoot: 'C:/UE', projectName: 'MyGame', uprojectPath });
      expect(step.editorTarget).toBe('UnrealEditor');
      expect(step.args).toEqual(['UnrealEditor', 'Win64', 'Development', `-project=${uprojectPath}`, '-WaitMutex']);
    } finally {
      rmTempDir(dir);
    }
  });

  it('resolves the macOS desktop build step', () => {
    const step = resolveDesktopBuildStep({
      platform: 'darwin',
      engineRoot: '/UE',
      projectName: 'MyGame',
      uprojectPath: '/Proj/MyGame.uproject',
    });
    expect(step.ubtPath).toBe('/UE/Engine/Build/BatchFiles/Mac/Build.sh');
    expect(step.args[1]).toBe('Mac');
  });

  it('resolves the Linux desktop build step', () => {
    const step = resolveDesktopBuildStep({
      platform: 'linux',
      engineRoot: '/UE',
      projectName: 'MyGame',
      uprojectPath: '/Proj/MyGame.uproject',
    });
    expect(step.ubtPath).toBe('/UE/Engine/Build/BatchFiles/Linux/Build.sh');
    expect(step.args[1]).toBe('Linux');
  });

  it('detects native source layout from Build.cs / Target.cs files', () => {
    const dir = makeTempDir();
    try {
      fs.mkdirSync(path.join(dir, 'Source', 'MyGame'), { recursive: true });
      fs.writeFileSync(path.join(dir, 'Source', 'MyGame', 'MyGame.Build.cs'), '// build', 'utf-8');
      expect(projectHasNativeCode(dir)).toBe(true);
    } finally {
      rmTempDir(dir);
    }
  });

  it('detects missing core plugin binaries on macOS and Linux separately', () => {
    const dir = makeTempDir();
    try {
      const pluginDir = path.join(dir, 'Plugins', 'UnrealMCP');
      fs.mkdirSync(pluginDir, { recursive: true });
      fs.writeFileSync(path.join(pluginDir, 'UnrealMCP.uplugin'), '{}', 'utf-8');

      fs.mkdirSync(path.join(pluginDir, 'Binaries', 'Mac'), { recursive: true });
      fs.writeFileSync(path.join(pluginDir, 'Binaries', 'Mac', 'UnrealEditor.modules'), '{}', 'utf-8');
      fs.writeFileSync(path.join(pluginDir, 'Binaries', 'Mac', 'UnrealEditor-UnrealMcpRuntime.dylib'), '', 'utf-8');
      fs.writeFileSync(path.join(pluginDir, 'Binaries', 'Mac', 'UnrealEditor-UnrealMcpEditor.dylib'), '', 'utf-8');
      expect(corePluginNeedsBuild(dir, 'darwin')).toBe(false);

      fs.mkdirSync(path.join(pluginDir, 'Binaries', 'Linux'), { recursive: true });
      fs.writeFileSync(path.join(pluginDir, 'Binaries', 'Linux', 'UnrealEditor.modules'), '{}', 'utf-8');
      expect(corePluginNeedsBuild(dir, 'linux')).toBe(true);
      fs.writeFileSync(path.join(pluginDir, 'Binaries', 'Linux', 'UnrealEditor-UnrealMcpRuntime.so'), '', 'utf-8');
      fs.writeFileSync(path.join(pluginDir, 'Binaries', 'Linux', 'UnrealEditor-UnrealMcpEditor.so'), '', 'utf-8');
      expect(corePluginNeedsBuild(dir, 'linux')).toBe(false);
    } finally {
      rmTempDir(dir);
    }
  });
});
