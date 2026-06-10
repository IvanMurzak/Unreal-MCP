// Minimal console UI helpers for the CLI command layer. The library layer
// (`lib/*`, `utils/*` other than this file) must NEVER import this — all
// stdout/stderr writes live here so the library stays side-effect-free.

let verboseEnabled = false;

export function setVerbose(on: boolean): void {
  verboseEnabled = on;
}

export function verbose(message: string): void {
  if (verboseEnabled) process.stderr.write(`[verbose] ${message}\n`);
}

export function info(message: string): void {
  process.stdout.write(`${message}\n`);
}

export function success(message: string): void {
  process.stdout.write(`✓ ${message}\n`);
}

export function warn(message: string): void {
  process.stderr.write(`! ${message}\n`);
}

export function error(message: string): void {
  process.stderr.write(`✗ ${message}\n`);
}

export function heading(message: string): void {
  process.stdout.write(`\n${message}\n`);
}

export function label(key: string, value: string): void {
  process.stdout.write(`  ${key}: ${value}\n`);
}

/** Print a list of warnings (no-op when empty). */
export function printWarnings(warnings: string[]): void {
  for (const w of warnings) warn(w);
}

/** Pretty-print a JSON value. */
export function json(value: unknown): void {
  process.stdout.write(`${JSON.stringify(value, null, 2)}\n`);
}
