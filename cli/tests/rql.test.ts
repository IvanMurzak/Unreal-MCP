import { afterEach, describe, expect, it, vi } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { decodeQueryFile, isRqlMutation, packagesWrittenSince, rqlErrorHint, rqlMaySave, rqlStages, saveReceipt, stripToolName } from '../src/lib/rql.js';
import { toolResponseText } from '../src/lib/tool-outcome.js';
import { rqlCall } from '../src/commands/rql.js';
import { reportToolOutcome } from '../src/commands/run-tool-builder.js';
import { CommandOutput } from '../src/utils/output.js';
import * as ui from '../src/utils/ui.js';
import { makeTempDir, rmTempDir } from './helpers.js';

const dirs: string[] = [];
afterEach(() => {
  for (const dir of dirs.splice(0)) rmTempDir(dir);
});

describe('rql query routing', () => {
  it('splits stages only on top-level pipes', () => {
    expect(rqlStages('actors | where Label == "a|b" | select [1, (2 | 3)]')).toEqual(['actors', 'where Label == "a|b"', 'select [1, (2 | 3)]']);
  });

  it('sends a query ending in set / call to rql-mutate, any other to rql-query', () => {
    expect(isRqlMutation('actors | where Label starts-with "Tree" | set Scale = Scale * 2')).toBe(true);
    expect(isRqlMutation('editor | call OpenLevel("/Game/Maps/Arena")')).toBe(true);
    expect(isRqlMutation('actors | where Label starts-with "Tree" | count')).toBe(false);
    expect(isRqlMutation('actors | where Label == "set | call" | select Label')).toBe(false);
    expect(isRqlMutation('actors | select settings')).toBe(false);
  });

  it('knows which calls may write packages', () => {
    expect(rqlMaySave('editor | call SaveLevel()')).toBe(true);
    expect(rqlMaySave('editor | call NewLevel("/Game/Maps/L")')).toBe(true);
    expect(rqlMaySave('editor | call OpenLevel("/Game/Maps/L")')).toBe(false);
  });
});

describe('toolResponseText', () => {
  it('returns the text of an all-text answer and leaves other answers to JSON', () => {
    expect(toolResponseText({ status: 'success', content: [{ type: 'text', text: '3 results' }, { type: 'text', text: 'x' }] })).toBe('3 results\nx');
    expect(toolResponseText({ content: [{ type: 'image', data: 'AAA' }] })).toBeUndefined();
    expect(toolResponseText({ content: [{ type: 'text', text: 'a' }, { type: 'image', data: 'AAA' }] })).toBeUndefined();
    expect(toolResponseText({ error: 'boom' })).toBeUndefined();
  });
});

describe('rqlErrorHint', () => {
  it('names the RQL spelling of an operator from another language', () => {
    expect(rqlErrorHint("error syntax-error: unexpected 'startsWith' in 'where' stage at 21")).toMatch(/starts-with/);
    expect(rqlErrorHint("error unknown-function: unknown method 'endsWith'")).toMatch(/ends-with/);
    expect(rqlErrorHint("error syntax-error: unexpected 'like' in 'where' stage at 21")).toMatch(/no like/);
  });

  it('explains a bare asset path and stays quiet on other errors', () => {
    expect(rqlErrorHint("error syntax-error: expected an expression, found '/' at 0")).toMatch(/OpenLevel/);
    expect(rqlErrorHint("error unknown-source: unknown source 'level'")).toBeUndefined();
  });
});

describe('packagesWrittenSince', () => {
  it('lists the .umap / .uasset files under Content written since a time', () => {
    const project = makeTempDir(); dirs.push(project);
    const maps = path.join(project, 'Content', 'Maps');
    fs.mkdirSync(maps, { recursive: true });
    const old = path.join(maps, 'Old.umap');
    fs.writeFileSync(old, '');
    const past = new Date(Date.now() - 60_000);
    fs.utimesSync(old, past, past);
    const since = Date.now() - 1000;
    fs.writeFileSync(path.join(maps, 'Arena.umap'), '');
    fs.writeFileSync(path.join(project, 'Content', 'M_Red.uasset'), '');
    fs.writeFileSync(path.join(maps, 'notes.txt'), '');
    expect(packagesWrittenSince(project, since)).toEqual(['Content/M_Red.uasset', 'Content/Maps/Arena.umap']);
  });

  it('prints the files a save wrote, and how many more past the limit', () => {
    expect(saveReceipt(['Content/A.umap'], true)).toBe('Saved to disk: Content/A.umap');
    expect(saveReceipt(['a', 'b', 'c'], false, 2)).toBe('Saved to disk: a, b, +1 more');
    expect(saveReceipt([], true)).toMatch(/No package file under Content/);
    expect(saveReceipt([], false)).toBeUndefined();
  });
});

describe('stripToolName', () => {
  it('drops a pasted rql-query / rql-mutate prefix only', () => {
    expect(stripToolName('rql-query diagnostics | count')).toBe('diagnostics | count');
    expect(stripToolName('  rql-mutate editor | call SaveAll()')).toBe('editor | call SaveAll()');
    expect(stripToolName('actors | where Label == "rql-query x"')).toBe('actors | where Label == "rql-query x"');
  });
});

describe('rql command line', () => {
  it('routes a query, and refuses a query with --schema or read-only with mutate flags', () => {
    expect(rqlCall('actors | count', {})).toEqual({ toolName: 'rql-query', input: { query: 'actors | count' } });
    expect(rqlCall('actors | set Scale = Scale * 2', { dryRun: true, maxTargets: '300' }))
      .toEqual({ toolName: 'rql-mutate', input: { query: 'actors | set Scale = Scale * 2', dry_run: true, max_targets: 300 } });
    expect(rqlCall(undefined, { schema: 'Actor' })).toEqual({ toolName: 'rql-schema', input: { type: 'Actor' } });
    expect(rqlCall(undefined, { schema: true })).toEqual({ toolName: 'rql-schema', input: {} });
    expect(rqlCall('actors | count', { schema: true })).toHaveProperty('usage');
    expect(rqlCall(undefined, { schema: 'actors | count' })).toHaveProperty('usage');
    expect(rqlCall('actors | count', { dryRun: true })).toHaveProperty('usage');
    expect(rqlCall(undefined, {})).toHaveProperty('usage');
  });

  it('reads || as or, not as a stage break', () => {
    expect(isRqlMutation('actors | where Label == "a" || Label == "b" | set X = 1')).toBe(true);
    expect(rqlStages('actors | where a || b')).toEqual(['actors', 'where a || b']);
  });

  it('decodes a UTF-16 query file (Windows PowerShell 5.1 redirection)', () => {
    const text = 'actors | count';
    expect(decodeQueryFile(Buffer.concat([Buffer.from([0xff, 0xfe]), Buffer.from(text, 'utf16le')]))).toBe(text);
    expect(decodeQueryFile(Buffer.from(text, 'utf-8'))).toBe(text);
  });

  it('treats backtick identifiers and string literals as opaque', () => {
    expect(isRqlMutation('actors | where `Bob\'s` == 1 | set X = 1')).toBe(true);
    expect(rqlMaySave('actors | where Label == "SaveAll(" | set X = 1')).toBe(false);
  });
});

describe('tool answers in text mode', () => {
  const answer = { status: 'success', content: [{ type: 'text', text: '3' }] };
  const outcome = { result: { kind: 'success' as const, success: true as const, endpoint: 'http://x/api/tools/rql-query', httpStatus: 200, data: answer } };

  function printed(text: boolean): string {
    const log = vi.spyOn(console, 'log').mockImplementation(() => {});
    const json = vi.spyOn(ui, 'json').mockImplementation((data) => { console.log(JSON.stringify(data)); });
    reportToolOutcome('tool', 'rql-query', outcome, new CommandOutput('rql', 'text'), { text, withDialogs: (f) => f });
    const lines = log.mock.calls.map((c) => String(c[0]));
    log.mockRestore();
    json.mockRestore();
    return lines.join('\n');
  }

  it('run-tool keeps printing the response JSON (scripts parse it); rql prints the text', () => {
    expect(JSON.parse(printed(false))).toEqual(answer);
    expect(printed(true)).toBe('3');
  });
});
