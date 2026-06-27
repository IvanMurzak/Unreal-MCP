// Small filesystem helpers shared across the CLI's lib modules.

import * as fs from 'fs';

/**
 * True when `p` is an existing symlink/junction (vs a real directory or file).
 * Returns `false` on any `lstat` error (missing path, permission). This guard
 * is load-bearing before destructive `rmSync`/`unlinkSync` of an installed
 * plugin path: a junction must be `unlink`ed (never recursed into), a real dir
 * must be `rm`ed. Centralised from the verbatim duplicates in
 * `install-plugin.ts` / `update.ts` / `clean-plugin.ts`. Pure-ish (one
 * `lstatSync` call).
 */
export function isSymlink(p: string): boolean {
  try {
    return fs.lstatSync(p).isSymbolicLink();
  } catch {
    return false;
  }
}
