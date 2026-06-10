// `bootstrap-local` — build the bridge + server from source into the
// project's `Intermediate/UnrealMCP/<leg>/<platform>/` layout
// (docs/ARCHITECTURE.md §6), the offline/dev alternative to the
// download-on-first-run flow. Library-safe.
//
// The build PLAN (which csproj, which RID, which output dir) is computed
// purely so it is unit-testable; the actual `dotnet publish` is delegated
// to an injectable `buildImpl` (defaults to spawning `dotnet`).

import * as path from 'path';
import { spawn } from 'child_process';
import { platform } from 'os';
import { emitProgress } from './progress.js';
import type { BootstrapLocalOptions, BootstrapLocalResult, BuildStep } from './types.js';

const BRIDGE_CSPROJ = path.join('bridge', 'src', 'com.IvanMurzak.Unreal.MCP.Bridge.csproj');
const SERVER_CSPROJ = path.join('Unreal-MCP-Server', 'com.IvanMurzak.Unreal.MCP.Server.csproj');

/** Map a platform to the .NET RID + the §6 platform folder name. Pure. */
export function ridForPlatform(os: NodeJS.Platform, arch: string = process.arch): string {
  switch (os) {
    case 'win32':
      return 'win-x64';
    case 'darwin':
      return arch === 'arm64' ? 'osx-arm64' : 'osx-x64';
    default:
      return 'linux-x64';
  }
}

/**
 * Compute the two build steps (bridge, server) for a project. Pure —
 * exported for tests so the path math is asserted without a real build.
 */
export function planBuildSteps(
  repoRoot: string,
  projectDir: string,
  os: NodeJS.Platform,
  arch: string = process.arch,
): { steps: BuildStep[]; outputRoot: string } {
  const rid = ridForPlatform(os, arch);
  const outputRoot = path.join(path.resolve(projectDir), 'Intermediate', 'UnrealMCP');
  const repo = path.resolve(repoRoot);
  const steps: BuildStep[] = [
    {
      label: 'bridge',
      projectFile: path.join(repo, BRIDGE_CSPROJ),
      outputDir: path.join(outputRoot, 'bridge', rid),
      rid,
    },
    {
      label: 'server',
      projectFile: path.join(repo, SERVER_CSPROJ),
      outputDir: path.join(outputRoot, 'server', rid),
      rid,
    },
  ];
  return { steps, outputRoot };
}

export async function bootstrapLocal(opts: BootstrapLocalOptions): Promise<BootstrapLocalResult> {
  const warnings: string[] = [];
  try {
    if (!opts?.projectDir) throw new Error('projectDir is required.');
    if (!opts?.repoRoot) throw new Error('repoRoot is required.');
    const os = opts.os ?? (platform() as NodeJS.Platform);

    const { steps, outputRoot } = planBuildSteps(opts.repoRoot, opts.projectDir, os);
    const build = opts.buildImpl ?? defaultDotnetPublish;

    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Bootstrapping bridge + server into ${outputRoot}`,
    });

    for (const step of steps) {
      emitProgress(opts.onProgress, { phase: 'info', message: `Building ${step.label} (${step.rid})` });
      await build(step);
      emitProgress(opts.onProgress, {
        phase: 'file-written',
        message: `Published ${step.label} to ${step.outputDir}`,
        filePath: step.outputDir,
      });
    }

    emitProgress(opts.onProgress, { phase: 'done', message: 'Bootstrap complete.' });
    return { kind: 'success', success: true, steps, outputRoot, warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      error: err instanceof Error ? err : new Error(String(err)),
    };
  }
}

function defaultDotnetPublish(step: BuildStep): Promise<void> {
  return new Promise((resolve, reject) => {
    const args = [
      'publish',
      step.projectFile,
      '-c',
      'Release',
      '-r',
      step.rid,
      '--self-contained',
      'true',
      '-p:PublishSingleFile=true',
      '-o',
      step.outputDir,
    ];
    const child = spawn('dotnet', args, { stdio: 'inherit' });
    child.on('error', (err) => reject(new Error(`dotnet publish (${step.label}) failed to start: ${err.message}`)));
    child.on('close', (code) => {
      if (code === 0) resolve();
      else reject(new Error(`dotnet publish (${step.label}) exited with code ${code}.`));
    });
  });
}
