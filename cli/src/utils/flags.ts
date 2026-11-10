import { parseWholeNumber } from './output.js';

/** Parse a whole-number flag of at least `min`; a bad value throws a UsageError (exit 2, CLI commands only). */
export function parseIntFlag(rawValue: string, flagName: string, { min }: { min: number }): number {
  return parseWholeNumber(rawValue, flagName, min);
}
