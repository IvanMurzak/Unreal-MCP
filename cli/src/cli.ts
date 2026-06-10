// CLI entry-point re-exports (`unreal-cli/cli`).
//
// Exposes the commander Command instances so a consumer can compose them into
// their own program. The runnable program lives in `index.ts` (imported by
// `bin/unreal-cli.js`).

export { statusCommand } from './commands/status.js';
