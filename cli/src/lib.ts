// Library entry point for `unreal-cli` (the dist/lib.js export the design
// commits to — docs/ARCHITECTURE.md §9.1).
//
// Constraints (same contract as unity-mcp-cli / godot-cli):
// - NO top-level side effects. Importing this file must not open sockets,
//   write to stdout/stderr, or parse argv.
// - NO `commander` import reachable from this file.
// - Every result is a discriminated union keyed on `kind`; errors are never
//   thrown past the public boundary.
//
// Consumers: `import { getStatus } from 'unreal-cli'`.

export { getStatus } from './lib/status.js';
export type { CliStatus } from './lib/status.js';
export { PACKAGE_NAME, PACKAGE_VERSION } from './version.js';
