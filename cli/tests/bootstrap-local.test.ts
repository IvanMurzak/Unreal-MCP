import { describe, it, expect, afterEach } from 'vitest';
import * as path from 'path';
import { bootstrapLocal, planBuildSteps, ridForPlatform } from '../src/lib/bootstrap-local.js';
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

describe('ridForPlatform', () => {
  it('maps platforms to RIDs', () => {
    expect(ridForPlatform('win32')).toBe('win-x64');
    expect(ridForPlatform('darwin', 'arm64')).toBe('osx-arm64');
    expect(ridForPlatform('darwin', 'x64')).toBe('osx-x64');
    expect(ridForPlatform('linux')).toBe('linux-x64');
  });
});

describe('planBuildSteps', () => {
  it('plans the bridge (only — the server is downloaded, not built) into Intermediate/UnrealMCP/bridge/<rid>', () => {
    const { steps, outputRoot } = planBuildSteps('/repo', '/proj', 'win32', 'x64');
    expect(outputRoot).toBe(path.join(path.resolve('/proj'), 'Intermediate', 'UnrealMCP'));
    expect(steps).toHaveLength(1);
    expect(steps[0].label).toBe('bridge');
    expect(steps[0].outputDir).toContain(path.join('Intermediate', 'UnrealMCP', 'bridge', 'win-x64'));
    expect(steps[0].projectFile).toContain(path.join('bridge', 'src'));
  });
});

describe('bootstrapLocal', () => {
  it('runs each build step via the injected builder', async () => {
    const built: string[] = [];
    const r = await bootstrapLocal({
      projectDir: tmp(),
      repoRoot: '/repo',
      os: 'win32',
      buildImpl: async (step) => {
        built.push(step.label);
      },
    });
    expect(r.kind).toBe('success');
    expect(built).toEqual(['bridge']);
  });

  it('returns failure (no throw) when a build step throws', async () => {
    const r = await bootstrapLocal({
      projectDir: tmp(),
      repoRoot: '/repo',
      os: 'win32',
      buildImpl: async () => {
        throw new Error('dotnet missing');
      },
    });
    expect(r.kind).toBe('failure');
    if (r.kind === 'failure') expect(r.error.message).toContain('dotnet missing');
  });
});
