import { afterEach, describe, expect, it } from 'vitest';
import * as path from 'path';
import { compileProject } from '../src/lib/compile.js';
import type { UbtBuildStep } from '../src/lib/types.js';
import { makeTempDir, rmTempDir, writeFakeEngine, writeUProject } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});

const os = process.platform as NodeJS.Platform;

function fixture(): { projectDir: string; engineRoot: string } {
  const projectDir = makeTempDir();
  const engineRoot = writeFakeEngine(makeTempDir('unreal-mcp-cli-engine-'));
  dirs.push(projectDir, engineRoot);
  writeUProject(projectDir, 'Game');
  return { projectDir, engineRoot };
}

function build(lines: string[], code = 0) {
  const steps: UbtBuildStep[] = [];
  const impl = async (step: UbtBuildStep, onLine: (line: string) => void) => {
    steps.push(step);
    lines.forEach(onLine);
    return { code, tail: lines.join('\n') };
  };
  return { steps, impl };
}

const base = { os, listProcessesImpl: () => [], existsImpl: () => true };

describe('compileProject', () => {
  it('succeeds with the warnings it saw, building the editor target', async () => {
    const { projectDir, engineRoot } = fixture();
    const ubt = build(['[1/2] Compile Foo.cpp', 'C:\\P\\Foo.cpp(3,1): warning C4100: unreferenced parameter', 'Result: Succeeded']);
    const r = await compileProject({ ...base, projectDir, engineRoot, buildImpl: ubt.impl });
    expect(r.kind).toBe('success');
    expect(r.target).toBe('GameEditor');
    expect(r.warnings).toHaveLength(1);
    expect(ubt.steps[0]!.args).toContain('GameEditor');
  });

  it('fails with the errors and their locations', async () => {
    const { projectDir, engineRoot } = fixture();
    const ubt = build(["C:\\P\\Foo.cpp(11,5): error C2065: 'x': undeclared identifier", 'Result: Failed'], 6);
    const r = await compileProject({ ...base, projectDir, engineRoot, buildImpl: ubt.impl });
    expect(r).toMatchObject({ kind: 'failure', reason: 'compilation-failed', exitCode: 6 });
    expect(r.errors[0]).toMatchObject({ file: 'C:\\P\\Foo.cpp', line: 11, column: 5 });
  });

  it('--strict fails on warnings', async () => {
    const { projectDir, engineRoot } = fixture();
    const ubt = build(['/p/Foo.cpp:3:7: warning: unused variable \'y\' [-Wunused-variable]']);
    const r = await compileProject({ ...base, projectDir, engineRoot, strict: true, buildImpl: ubt.impl });
    expect(r).toMatchObject({ kind: 'failure', reason: 'compilation-failed' });
    expect(r.kind === 'failure' && r.message).toMatch(/1 warning \(--strict\)/);
  });

  it('refuses while an editor has the project open, without building', async () => {
    const { projectDir, engineRoot } = fixture();
    const ubt = build([]);
    const uproject = path.join(projectDir, 'Game.uproject');
    const r = await compileProject({
      ...base,
      projectDir,
      engineRoot,
      buildImpl: ubt.impl,
      listProcessesImpl: () => [{ pid: 42, commandLine: `"C:\\UE\\UnrealEditor.exe" "${uproject}" -RenderOffscreen` }],
    });
    expect(r).toMatchObject({ kind: 'failure', reason: 'project-open', editorPids: [42] });
    expect(ubt.steps).toEqual([]);
  });

  it('reports a missing project and a missing UnrealBuildTool', async () => {
    const { engineRoot } = fixture();
    const empty = makeTempDir();
    dirs.push(empty);
    expect(await compileProject({ ...base, projectDir: empty, engineRoot })).toMatchObject({ kind: 'failure', reason: 'not-found' });
    const { projectDir } = fixture();
    expect(await compileProject({ ...base, projectDir, engineRoot, existsImpl: () => false }))
      .toMatchObject({ kind: 'failure', reason: 'unreachable' });
  });
});
