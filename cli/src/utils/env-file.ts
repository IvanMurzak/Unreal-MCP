// `.env` parsing / writing + `.gitignore` append helpers.
//
// The C++ plugin parses `<Project>/.env` with GodotMcpEnvFile's rules
// (docs/ARCHITECTURE.md §8): skip blanks and `#` comments, split on the
// first `=`, trim, recognize only known keys, strip a single pair of
// matching surrounding quotes. These helpers stay 1:1 with that parser so
// what `unreal-mcp-cli configure` writes is exactly what the plugin reads.
//
// Pure where possible; the read/write functions touch the filesystem but
// never throw past their callers' try/catch — the lib boundary owns error
// shaping.

import * as fs from 'fs';
import * as path from 'path';
import { EOL } from 'os';

/**
 * The recognized `UNREAL_MCP_*` environment keys (docs/ARCHITECTURE.md §8).
 * Anything outside this set is ignored on read and rejected on write so a
 * typo never silently lands an unrecognized key in a project's `.env`.
 */
export const KNOWN_ENV_KEYS = [
  'UNREAL_MCP_CONNECTION_MODE',
  'UNREAL_MCP_HOST',
  'UNREAL_MCP_CLOUD_URL',
  'UNREAL_MCP_TOKEN',
  'UNREAL_MCP_AUTH_OPTION',
  'UNREAL_MCP_KEEP_CONNECTED',
  'UNREAL_MCP_TOOLS',
  'UNREAL_MCP_START_SERVER',
  'UNREAL_MCP_TRANSPORT',
  'UNREAL_MCP_LOG_LEVEL',
  'UNREAL_MCP_BRIDGE_PATH',
  'UNREAL_MCP_SERVER_PATH',
] as const;

export type KnownEnvKey = (typeof KNOWN_ENV_KEYS)[number];

const KNOWN_ENV_KEY_SET = new Set<string>(KNOWN_ENV_KEYS);

export function isKnownEnvKey(key: string): key is KnownEnvKey {
  return KNOWN_ENV_KEY_SET.has(key);
}

/**
 * Strip a single pair of matching surrounding quotes (single or double).
 * `"foo"` -> `foo`, `'bar'` -> `bar`, `"unbalanced` -> `"unbalanced`.
 * Pure.
 */
export function stripMatchingQuotes(value: string): string {
  if (value.length >= 2) {
    const first = value[0];
    const last = value[value.length - 1];
    if ((first === '"' || first === "'") && first === last) {
      return value.slice(1, -1);
    }
  }
  return value;
}

/**
 * Parse `.env` text into a key/value map, applying the plugin's rules:
 * skip blank lines and `#` comments, split on the FIRST `=`, trim both
 * sides, keep only {@link KNOWN_ENV_KEYS}, strip matching quotes from the
 * value. Later duplicates win. Pure / no I/O.
 */
export function parseEnvContent(content: string): Partial<Record<KnownEnvKey, string>> {
  const out: Partial<Record<KnownEnvKey, string>> = {};
  for (const rawLine of content.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (line.length === 0 || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    if (!isKnownEnvKey(key)) continue;
    const value = stripMatchingQuotes(line.slice(eq + 1).trim());
    out[key] = value;
  }
  return out;
}

/** Read + parse `<file>` if present; returns `{}` when absent or unreadable. */
export function readEnvFile(envPath: string): Partial<Record<KnownEnvKey, string>> {
  if (!fs.existsSync(envPath)) return {};
  let raw: string;
  try {
    raw = fs.readFileSync(envPath, 'utf-8');
  } catch {
    return {};
  }
  return parseEnvContent(raw);
}

/**
 * Quote a value for `.env` only when it needs it — i.e. it contains
 * whitespace, `#`, or a quote character. Bare tokens stay bare so a
 * human-edited `.env` reads cleanly. Pure.
 *
 * The value is NOT backslash-escaped: the plugin's GodotMcpEnvFile parser
 * (and {@link stripMatchingQuotes}) only strips a single surrounding quote
 * pair and never unescapes, so emitting `\"` would read back corrupted.
 * Wrapping-without-escaping round-trips correctly for embedded quotes
 * (`a"b` -> `"a"b"` -> `a"b`).
 */
export function quoteEnvValueIfNeeded(value: string): string {
  if (value.length === 0) return '""';
  if (/[\s#"']/.test(value)) {
    return `"${value}"`;
  }
  return value;
}

/**
 * Merge `updates` into the `.env` at `envPath`, preserving any existing
 * lines (comments, blank lines, unknown keys, ordering) and rewriting only
 * the keys present in `updates`. New keys are appended in a stable block.
 * Returns the keys that were added vs updated. Touches the filesystem.
 *
 * A `null`/`undefined` value in `updates` deletes that key from the file.
 */
export function writeEnvFile(
  envPath: string,
  updates: Partial<Record<KnownEnvKey, string | null | undefined>>,
): { added: KnownEnvKey[]; updated: KnownEnvKey[]; removed: KnownEnvKey[] } {
  const existingLines = fs.existsSync(envPath)
    ? fs.readFileSync(envPath, 'utf-8').split(/\r?\n/)
    : [];

  const added: KnownEnvKey[] = [];
  const updated: KnownEnvKey[] = [];
  const removed: KnownEnvKey[] = [];
  const handled = new Set<KnownEnvKey>();

  const rewritten: string[] = [];
  for (const line of existingLines) {
    const trimmed = line.trim();
    if (trimmed.length === 0 || trimmed.startsWith('#')) {
      rewritten.push(line);
      continue;
    }
    const eq = trimmed.indexOf('=');
    const key = eq >= 0 ? trimmed.slice(0, eq).trim() : '';
    if (isKnownEnvKey(key) && key in updates) {
      // A key may appear more than once in the file. Only the FIRST occurrence
      // is rewritten with the new value; later duplicates are dropped so the
      // result holds a single line per key and `updated`/`removed` are not
      // double-counted.
      if (handled.has(key)) continue;
      handled.add(key);
      const value = updates[key];
      if (value === null || value === undefined) {
        removed.push(key);
        continue; // drop the line
      }
      rewritten.push(`${key}=${quoteEnvValueIfNeeded(value)}`);
      updated.push(key);
      continue;
    }
    rewritten.push(line);
  }

  // Append any keys not already present.
  const toAppend: string[] = [];
  for (const key of KNOWN_ENV_KEYS) {
    if (!(key in updates) || handled.has(key)) continue;
    const value = updates[key];
    if (value === null || value === undefined) continue;
    toAppend.push(`${key}=${quoteEnvValueIfNeeded(value)}`);
    added.push(key);
  }

  // Drop a trailing empty line so we don't accumulate blank lines on each
  // round-trip, then re-join with a single trailing newline.
  while (rewritten.length > 0 && rewritten[rewritten.length - 1].trim() === '') {
    rewritten.pop();
  }
  const body = [...rewritten, ...toAppend];
  const text = body.join(EOL) + EOL;

  fs.mkdirSync(path.dirname(envPath), { recursive: true });
  fs.writeFileSync(envPath, text, 'utf-8');

  return { added, updated, removed };
}

/**
 * Ensure `<gitignoreDir>/.gitignore` ignores `.env`, creating the file if
 * absent. Idempotent: a line that already ignores `.env` (`/.env`, `.env`,
 * or `*.env`) short-circuits without a write. Returns what happened so the
 * caller can surface an accurate progress message.
 */
export function ensureEnvGitignored(
  gitignoreDir: string,
): { gitignorePath: string; action: 'created' | 'appended' | 'already-ignored' } {
  const gitignorePath = path.join(gitignoreDir, '.gitignore');

  if (!fs.existsSync(gitignorePath)) {
    fs.mkdirSync(gitignoreDir, { recursive: true });
    fs.writeFileSync(
      gitignorePath,
      `# Added by unreal-mcp-cli configure — never commit local MCP secrets.${EOL}.env${EOL}`,
      'utf-8',
    );
    return { gitignorePath, action: 'created' };
  }

  const content = fs.readFileSync(gitignorePath, 'utf-8');
  if (gitignoreAlreadyIgnoresEnv(content)) {
    return { gitignorePath, action: 'already-ignored' };
  }

  const needsLeadingNewline = content.length > 0 && !content.endsWith('\n');
  const appendix = `${needsLeadingNewline ? EOL : ''}.env${EOL}`;
  fs.appendFileSync(gitignorePath, appendix, 'utf-8');
  return { gitignorePath, action: 'appended' };
}

/**
 * True when a `.gitignore` body already ignores `.env` via a bare `.env`,
 * a root-anchored `/.env`, or a `*.env` glob. Negations (`!.env`) do NOT
 * count. Pure.
 */
export function gitignoreAlreadyIgnoresEnv(content: string): boolean {
  for (const rawLine of content.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (line.length === 0 || line.startsWith('#') || line.startsWith('!')) continue;
    const normalized = line.replace(/^\//, '');
    if (normalized === '.env' || normalized === '*.env') return true;
  }
  return false;
}
