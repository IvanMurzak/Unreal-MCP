import { afterEach, describe, expect, it, vi } from 'vitest';
import { writeFileSync } from 'fs';
import { join } from 'path';
import { buildRunToolCommand } from '../src/commands/run-tool-builder.js';
import { makeTempDir, rmTempDir } from './helpers.js';
import * as ui from '../src/utils/ui.js';

const dirs: string[] = [];
const originalExitCode = process.exitCode;
afterEach(() => {
  for (const dir of dirs.splice(0)) rmTempDir(dir);
  process.exitCode = originalExitCode;
  vi.restoreAllMocks();
});

function command() {
  vi.spyOn(ui, 'json').mockImplementation(() => {});
  vi.spyOn(ui, 'error').mockImplementation(() => {});
  const invoke = vi.fn(async () => ({ kind: 'success' as const, success: true as const,
    endpoint: 'http://localhost/api/tools/example', httpStatus: 200, data: {} }));
  return { invoke, cli: buildRunToolCommand({ name: 'run-tool', description: 'Test', errorNoun: 'tool', invoke }) };
}

describe('run-tool command input', () => {
  it('reads UTF-8 JSON from a path with spaces and defaults to the current project', async () => {
    const dir = makeTempDir(); dirs.push(dir);
    const file = join(dir, 'tool args.json');
    const input = JSON.stringify({ text: 'Bob\'s "hero" \\ path\nПривет' });
    writeFileSync(file, input, 'utf8');
    const { cli, invoke } = command();
    await cli.parseAsync(['node', 'cli', 'example', '--input-file', file]);
    expect(invoke).toHaveBeenCalledWith(expect.objectContaining({ input, projectDir: process.cwd() }));
  });

  it('rejects ambiguous input before invoking a tool', async () => {
    const { cli, invoke } = command();
    await cli.parseAsync(['node', 'cli', 'example', '--input', '{}', '--input-file', 'missing.json']);
    expect(invoke).not.toHaveBeenCalled();
    expect(process.exitCode).toBe(1);
    expect(ui.error).toHaveBeenCalledWith(expect.stringContaining('not both'));
  });

  it('rejects invalid file JSON before invoking a tool', async () => {
    const dir = makeTempDir(); dirs.push(dir);
    const file = join(dir, 'invalid.json'); writeFileSync(file, '{ ... }');
    const { cli, invoke } = command();
    await cli.parseAsync(['node', 'cli', 'example', '--input-file', file]);
    expect(invoke).not.toHaveBeenCalled();
    expect(process.exitCode).toBe(1);
  });
});
