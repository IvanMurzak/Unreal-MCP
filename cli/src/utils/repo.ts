// Locate the Unreal-MCP repo root (and its `UnrealMCP/` plugin source) from
// the installed CLI's own location, so `install-plugin` / `update` /
// `bootstrap-local` have a sane default source without the user passing
// `--plugin-source`. Pure-ish (filesystem existence checks only).

import * as fs from 'fs';
import * as path from 'path';

/**
 * Walk up from `startDir` looking for a directory that contains
 * `UnrealMCP/UnrealMCP.uplugin` (the repo root). Returns it, or `null` when
 * the CLI was installed outside the repo (e.g. via npm global). Pure-ish.
 */
export function findRepoRoot(startDir: string): string | null {
  let dir = path.resolve(startDir);
  for (let i = 0; i < 8; i++) {
    if (fs.existsSync(path.join(dir, 'UnrealMCP', 'UnrealMCP.uplugin'))) return dir;
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return null;
}

/** Default plugin source = `<repoRoot>/UnrealMCP`, or `null`. */
export function defaultPluginSource(startDir: string): string | null {
  const root = findRepoRoot(startDir);
  return root ? path.join(root, 'UnrealMCP') : null;
}
