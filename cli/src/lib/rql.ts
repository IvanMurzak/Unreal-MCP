// `rql` — RQL text straight from the command line: which rql-* tool a query goes to, hints for the RQL slips agents
// make most, and which package files a save wrote. Never prints, never exits.

import * as fs from 'fs';
import * as path from 'path';

import { RQL_MUTATE_TOOL, RQL_QUERY_TOOL, RQL_SCHEMA_TOOL, decodeQueryFile, isRqlMutation, rqlStages, stripToolName } from '@baizor/gamedev-cli-core';

export { RQL_MUTATE_TOOL, RQL_QUERY_TOOL, RQL_SCHEMA_TOOL, decodeQueryFile, isRqlMutation, rqlStages, stripToolName };

/** Operators agents bring from other languages, with RQL's spelling. */
const OPERATOR_HINTS: ReadonlyArray<readonly [RegExp, string]> = [
  [/^startswith$/i, 'RQL spells it starts-with: where Label starts-with "Tree"'],
  [/^endswith$/i, 'RQL spells it ends-with: where Label ends-with "_LOD0"'],
  [/^(like|ilike)$/i, 'RQL has no like: use starts-with / ends-with / contains, or matches "regex"'],
  [/^(includes|has)$/i, 'RQL spells it contains: where Label contains "Tree"'],
  [/^(isnull|is_null)$/i, 'RQL spells it x is null / x is not null'],
  [/^(notin|not_in)$/i, 'RQL spells it not (x in [a, b])'],
  [/^(regex|regexp|match|rlike)$/i, 'RQL spells it matches "regex": where Label matches "^Tree_[0-9]+$"'],
];

/** A hint for an RQL error whose cause is a familiar slip (an operator spelled as in another language), else `undefined`. */
export function rqlErrorHint(message: string): string | undefined {
  const token = /unexpected '([^']+)'|unknown method '([^']+)'/.exec(message);
  const word = token?.[1] ?? token?.[2];
  if (word) {
    for (const [pattern, hint] of OPERATOR_HINTS) if (pattern.test(word)) return hint;
  }
  if (/expected an expression, found '\/'/.test(message)) {
    return 'an asset path is a string: unreal.asset("/Game/Path"); a level\'s actors: rql \'editor | call OpenLevel("/Game/Maps/Name")\', then actors | ...';
  }
  return undefined;
}

/** A query that may write assets to disk: a save, or a new level saved at its path. */
export function rqlMaySave(query: string): boolean {
  // Outside string literals and `identifiers`: where Label == "SaveAll(" saves nothing.
  const code = query.replace(/"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`[^`]*`/g, '""');
  return /\b(SaveLevel|SaveAll|SaveAsset|NewLevel)\s*\(/.test(code);
}

/**
 * The package files (.umap / .uasset) under `<project>/Content` modified at or after `sinceMs`, relative to the project
 * with forward slashes, sorted — what a save call actually wrote.
 */
export function packagesWrittenSince(projectDir: string, sinceMs: number): string[] {
  const found: string[] = [];
  const walk = (dir: string): void => {
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        if (!entry.name.startsWith('.')) walk(full);
      } else if (/\.(umap|uasset)$/i.test(entry.name)) {
        try {
          if (fs.statSync(full).mtimeMs >= sinceMs) found.push(path.relative(projectDir, full).split(path.sep).join('/'));
        } catch {
          /* removed meanwhile */
        }
      }
    }
  };
  walk(path.join(projectDir, 'Content'));
  return found.sort();
}

/**
 * The line a change prints: the files it wrote (the first `limit`, then how many more). When it wrote none: that, for a
 * save (`maySave`); nothing for any other change (most edits stay unsaved on purpose).
 */
export function saveReceipt(written: readonly string[], maySave: boolean, limit = 20): string | undefined {
  if (written.length === 0) return maySave ? 'No package file under Content/ changed on disk.' : undefined;
  const more = written.length > limit ? `, +${written.length - limit} more` : '';
  return `Saved to disk: ${written.slice(0, limit).join(', ')}${more}`;
}
