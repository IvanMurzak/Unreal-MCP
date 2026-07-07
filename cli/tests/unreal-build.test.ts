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
  it('resolves the Windows desktop build step', () => {
    const step = resolveDesktopBuildStep({
      platform: 'win32',
      engineRoot: 'C:/UE',
      projectName: 'MyGame',
      uprojectPath: 'C:/Proj/MyGame.uproject',
    });
    expect(step.ubtPath.replace(/\\/g, '/')).toContain('C:/UE/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe');
    expect(step.args).toEqual([
      'MyGameEditor',
      'Win64',
      'Development',
      '-project=C:/Proj/MyGame.uproject',
      '-WaitMutex',
    ]);
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
