import * as fs from 'fs';
import * as path from 'path';
import { spawn } from 'child_process';
import type { UbtBuildStep } from '../lib/types.js';

export type DesktopBuildPlatform = 'win32' | 'darwin' | 'linux';

interface DesktopBuildConfig {
  buildEntrySegments: readonly string[];
  targetPlatform: 'Win64' | 'Mac' | 'Linux';
  pluginBinariesDir: 'Win64' | 'Mac' | 'Linux';
  moduleLibraryPattern: RegExp;
}

const BUILD_CONFIG: Record<DesktopBuildPlatform, DesktopBuildConfig> = {
  win32: {
    buildEntrySegments: ['Engine', 'Binaries', 'DotNET', 'UnrealBuildTool', 'UnrealBuildTool.exe'],
    targetPlatform: 'Win64',
    pluginBinariesDir: 'Win64',
    moduleLibraryPattern: /^UnrealEditor-(?:UnrealMcpRuntime|UnrealMcpEditor)\.dll$/i,
  },
  darwin: {
    buildEntrySegments: ['Engine', 'Build', 'BatchFiles', 'Mac', 'Build.sh'],
    targetPlatform: 'Mac',
    pluginBinariesDir: 'Mac',
    moduleLibraryPattern: /^UnrealEditor-(?:UnrealMcpRuntime|UnrealMcpEditor)\.dylib$/i,
  },
  linux: {
    buildEntrySegments: ['Engine', 'Build', 'BatchFiles', 'Linux', 'Build.sh'],
    targetPlatform: 'Linux',
    pluginBinariesDir: 'Linux',
    moduleLibraryPattern: /^UnrealEditor-(?:UnrealMcpRuntime|UnrealMcpEditor)\.so$/i,
  },
};

export function isDesktopBuildPlatform(value: string): value is DesktopBuildPlatform {
  return value === 'win32' || value === 'darwin' || value === 'linux';
}

export function resolveDesktopBuildStep(input: {
  platform: DesktopBuildPlatform;
  engineRoot: string;
  projectName: string;
  uprojectPath: string;
}): UbtBuildStep {
  const config = BUILD_CONFIG[input.platform];
  const pathApi = input.platform === 'win32' ? path.win32 : path.posix;
  const editorTarget = `${input.projectName}Editor`;
  return {
    ubtPath: pathApi.join(input.engineRoot, ...config.buildEntrySegments),
    editorTarget,
    uprojectPath: input.uprojectPath,
    args: [
      editorTarget,
      config.targetPlatform,
      'Development',
      `-project=${input.uprojectPath}`,
      '-WaitMutex',
    ],
  };
}

export function corePluginNeedsBuild(
  projectDir: string,
  platform: DesktopBuildPlatform,
): boolean {
  const pluginRoot = path.join(projectDir, 'Plugins', 'UnrealMCP');
  if (!fs.existsSync(path.join(pluginRoot, 'UnrealMCP.uplugin'))) return false;
  const config = BUILD_CONFIG[platform];
  const binariesDir = path.join(pluginRoot, 'Binaries', config.pluginBinariesDir);
  if (!fs.existsSync(path.join(binariesDir, 'UnrealEditor.modules'))) return true;
  return !treeHasFileMatching(binariesDir, (name) => config.moduleLibraryPattern.test(name));
}

export function projectHasNativeCode(projectDir: string): boolean {
  const sourceRoot = path.join(projectDir, 'Source');
  return treeHasFileMatching(
    sourceRoot,
    (name) => name.endsWith('.Build.cs') || name.endsWith('.Target.cs'),
  );
}

export function runUnrealBuildStep(step: UbtBuildStep): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    const child = spawn(step.ubtPath, step.args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let combined = '';
    const append = (chunk: Buffer | string): void => {
      combined += chunk.toString();
      if (combined.length > 8192) combined = combined.slice(-8192);
    };
    child.stdout?.on('data', append);
    child.stderr?.on('data', append);
    child.on('error', reject);
    child.on('close', (code) => {
      if (code === 0) return resolve();
      const tail = combined.trim().slice(-4096);
      reject(new Error(`Unreal build exited with code ${code}${tail ? `:\n${tail}` : ''}`));
    });
  });
}

function treeHasFileMatching(
  root: string,
  matches: (fileName: string, fullPath: string) => boolean,
): boolean {
  if (!fs.existsSync(root)) return false;
  const stack: string[] = [root];
  while (stack.length > 0) {
    const dir = stack.pop()!;
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const entry of entries) {
      const fullPath = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        stack.push(fullPath);
        continue;
      }
      if (entry.isFile() && matches(entry.name, fullPath)) return true;
    }
  }
  return false;
}
