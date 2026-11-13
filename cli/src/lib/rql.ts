// `rql` — RQL text straight from the command line: which rql-* tool a query goes to, hints for the RQL slips agents
// make most, and which package files a save wrote. Never prints, never exits.

import * as fs from 'fs';
import * as path from 'path';
import { isRqlMutation, stripToolName } from '@baizor/gamedev-cli-core';

export { RQL_MUTATE_TOOL, RQL_QUERY_TOOL, RQL_SCHEMA_TOOL, decodeQueryFile, isRqlMutation, rqlStages, stripToolName } from '@baizor/gamedev-cli-core';

/**
 * The statements of a request text (RQL spec section 14, `RqlParser.SplitStatements`): split at `;` outside string
 * literals (backslash escapes honoured), `backtick` identifiers and brackets, trimmed, blank ones dropped. Text without
 * a separator is one statement, the whole text trimmed; text of nothing but separators and blanks is its first segment.
 */
export function rqlStatements(text: string): string[] {
  const segments: string[] = [];
  let depth = 0;
  let quote: string | undefined;
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (quote) {
      if (ch === '\\' && quote !== '`') i++;
      else if (ch === quote) quote = undefined;
    } else if (ch === '"' || ch === "'" || ch === '`') {
      quote = ch;
    } else if (ch === '(' || ch === '[' || ch === '{') {
      depth++;
    } else if (ch === ')' || ch === ']' || ch === '}') {
      depth = Math.max(0, depth - 1);
    } else if (ch === ';' && depth === 0) {
      segments.push(text.slice(start, i).trim());
      start = i + 1;
    }
  }
  segments.push(text.slice(start).trim());
  if (segments.length === 1) return segments;
  const statements = segments.filter((s) => s.length > 0);
  return statements.length > 0 ? statements : segments.slice(0, 1);
}

/**
 * Text that asks for a change goes to rql-mutate: a statement whose last stage is `set` or `call`. With several
 * statements one is enough, so mixed text reaches rql-mutate, which refuses it with the statement that does not end in
 * set / call (nothing runs); text of reads only goes to rql-query.
 */
export function isRqlChange(text: string): boolean {
  return rqlStatements(text).some(isRqlMutation);
}

/** The text without a leading tool name on any of its statements ("rql-query actors | count; rql-query levels | count"). */
export function stripToolNames(text: string): string {
  const statements = rqlStatements(text);
  return statements.length > 1 ? statements.map(stripToolName).join('; ') : stripToolName(text);
}

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
