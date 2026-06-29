// Pure helpers for the two UE descriptor files `install-extension` touches:
//
//  - a plugin's `*.uplugin` — to learn its declared `Plugins[]` dependencies
//    (the gating engine plugins, e.g. Niagara) and its `VersionName`.
//  - the project's `*.uproject` — to ENABLE the extension + its gating plugins in
//    the `Plugins[]` array, idempotently (a `read-modify-write` of the JSON).
//
// Both files are tab-indented JSON; writes round-trip through
// `JSON.stringify(obj, null, '\t')` and preserve a trailing newline. No I/O here
// (callers read/write); these are pure transforms so they are exhaustively
// unit-testable without a UE install.

/** One entry of a UE descriptor `Plugins[]` array. */
interface UPluginRef {
  Name?: string;
  Enabled?: boolean;
  [key: string]: unknown;
}

/**
 * Extract the names of the plugin dependencies a `.uplugin` declares as enabled
 * (`Plugins[]` entries whose `Enabled` is not `false`). These are the gating
 * plugins (e.g. `Niagara`) that must also be enabled in the consuming
 * `.uproject`. Tolerant of a missing/!object/!array `Plugins` field (→ `[]`).
 * Pure.
 */
export function parseUPluginDependencies(upluginJson: unknown): string[] {
  if (!upluginJson || typeof upluginJson !== 'object') return [];
  const plugins = (upluginJson as Record<string, unknown>)['Plugins'];
  if (!Array.isArray(plugins)) return [];
  const names: string[] = [];
  for (const entry of plugins) {
    if (entry && typeof entry === 'object') {
      const ref = entry as UPluginRef;
      if (typeof ref.Name === 'string' && ref.Name.trim() !== '' && ref.Enabled !== false) {
        names.push(ref.Name.trim());
      }
    }
  }
  return names;
}

/**
 * Read a `.uplugin`'s `VersionName` (the extension's own version string), or
 * `null` when absent / not a string. Pure.
 */
export function readUPluginVersionName(upluginJson: unknown): string | null {
  if (upluginJson && typeof upluginJson === 'object') {
    const v = (upluginJson as Record<string, unknown>)['VersionName'];
    if (typeof v === 'string' && v.trim() !== '') return v.trim();
  }
  return null;
}

/** Result of enabling plugins in a `.uproject`'s text. */
export interface EnablePluginsResult {
  /** True when the `.uproject` text was modified. */
  changed: boolean;
  /** The resulting `.uproject` text (unchanged when `changed === false`). */
  text: string;
  /** The names enabled in the project after the operation (full `Plugins[]` enabled set). */
  enabledPlugins: string[];
}

/**
 * Enable each plugin in `pluginNames` in a `.uproject`'s JSON text:
 *
 *  - a plugin already present and enabled is left untouched (idempotent);
 *  - a plugin present but `Enabled: false` is flipped to `true`;
 *  - a plugin absent is appended as `{ "Name": <n>, "Enabled": true }`.
 *
 * Names are de-duplicated case-insensitively (UE plugin names are
 * case-insensitive). The write preserves the file's tab indentation and a
 * trailing newline if the input had one. Throws on malformed JSON (the caller
 * converts it to a structured failure). Pure transform.
 */
export function enablePluginsInUProject(uprojectText: string, pluginNames: readonly string[]): EnablePluginsResult {
  const parsed = JSON.parse(uprojectText) as Record<string, unknown>;

  const existing: UPluginRef[] = Array.isArray(parsed['Plugins'])
    ? (parsed['Plugins'] as UPluginRef[])
    : [];

  // De-dupe the requested names case-insensitively, preserving first-seen order.
  const wanted: string[] = [];
  const seen = new Set<string>();
  for (const raw of pluginNames) {
    const name = (raw ?? '').trim();
    if (name === '') continue;
    const key = name.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    wanted.push(name);
  }

  let changed = false;
  const plugins = existing.slice();
  for (const name of wanted) {
    const idx = plugins.findIndex(
      (p) => typeof p.Name === 'string' && p.Name.toLowerCase() === name.toLowerCase(),
    );
    if (idx === -1) {
      plugins.push({ Name: name, Enabled: true });
      changed = true;
    } else if (plugins[idx].Enabled !== true) {
      plugins[idx] = { ...plugins[idx], Enabled: true };
      changed = true;
    }
  }

  const enabledPlugins = plugins
    .filter((p) => typeof p.Name === 'string' && p.Enabled !== false)
    .map((p) => p.Name as string);

  if (!changed) {
    return { changed: false, text: uprojectText, enabledPlugins };
  }

  parsed['Plugins'] = plugins;
  const trailingNewline = uprojectText.endsWith('\n') ? '\n' : '';
  const text = JSON.stringify(parsed, null, '\t') + trailingNewline;
  return { changed: true, text, enabledPlugins };
}
