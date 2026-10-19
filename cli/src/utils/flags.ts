import * as ui from './ui.js';

/** Parse a whole-number flag of at least `min`; prints and exits on a bad value (CLI commands only). */
export function parseIntFlag(rawValue: string, flagName: string, { min }: { min: number }): number {
  const parsed = /^\d+$/.test(rawValue.trim()) ? Number.parseInt(rawValue, 10) : Number.NaN;
  if (!Number.isSafeInteger(parsed) || parsed < min) {
    ui.error(`${flagName} must be a whole number${min > 0 ? ` of at least ${min}` : ''}`);
    process.exit(1);
  }
  return parsed;
}
