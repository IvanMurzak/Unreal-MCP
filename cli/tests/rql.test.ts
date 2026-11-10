import { afterEach, describe, expect, it } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { isRqlMutation, packagesWrittenSince, rqlErrorHint, rqlMaySave, rqlStages, stripToolName, toolResponseText } from '../src/lib/rql.js';
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
});

describe('stripToolName', () => {
  it('drops a pasted rql-query / rql-mutate prefix only', () => {
    expect(stripToolName('rql-query diagnostics | count')).toBe('diagnostics | count');
    expect(stripToolName('  rql-mutate editor | call SaveAll()')).toBe('editor | call SaveAll()');
    expect(stripToolName('actors | where Label == "rql-query x"')).toBe('actors | where Label == "rql-query x"');
  });
});
