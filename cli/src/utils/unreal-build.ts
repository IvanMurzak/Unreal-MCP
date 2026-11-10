import * as fs from 'fs';
import * as path from 'path';
import { spawn } from 'child_process';
import type { Readable } from 'stream';
import type { UbtBuildStep } from '../lib/types.js';
import { createUnrealBuildDiagnostics } from './build-diagnostics.js';

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
  const diagnostics = createUnrealBuildDiagnostics();
  return spawnUnrealBuild(step, (line) => diagnostics.line(line)).then(({ code, tail }) => {
    if (code !== 0) throw new Error(diagnostics.message(code, tail));
  });
}

/** How a UBT run ended: its exit code and the last 8 KB of output. */
export interface UnrealBuildExit {
  code: number | null;
  tail: string;
}

/**
 * Hand every line of a child's output stream (without the line break, decoded as UTF-8 across chunk boundaries) to
 * `onLine`; `onText` sees the raw text. The START of an over-long line is kept: a diagnostic's `file:line: error:`
 * prefix comes first.
 */
export function forEachLine(stream: Readable | null | undefined, onLine: (line: string) => void, onText?: (text: string) => void): void {
  if (!stream) return;
  let pending = '';
  stream.setEncoding('utf8');
  stream.on('data', (text: string) => {
    onText?.(text);
    const lines = (pending + text).split(/[\r\n]/);
    pending = (lines.pop() ?? '').slice(0, 8192);
    for (const line of lines) onLine(line);
  });
  stream.on('end', () => {
    if (pending) onLine(pending);
    pending = '';
  });
}

/**
 * Run UBT and hand every output line (stdout and stderr, without the line break) to `onLine`. Resolves with the exit
 * code (never rejects on a non-zero one); rejects when UBT cannot be started.
 */
export function spawnUnrealBuild(step: UbtBuildStep, onLine: (line: string) => void): Promise<UnrealBuildExit> {
  return new Promise<UnrealBuildExit>((resolve, reject) => {
    const child = spawn(step.ubtPath, step.args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let tail = '';
    const keepTail = (text: string): void => { tail = (tail + text).slice(-8192); };
    forEachLine(child.stdout, onLine, keepTail);
    forEachLine(child.stderr, onLine, keepTail);
    child.on('error', reject);
    child.on('close', (code) => resolve({ code, tail }));
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
