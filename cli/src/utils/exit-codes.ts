// Process exit codes shared by unreal-mcp-cli, unity-mcp-cli and godot-cli (one contract for agents and CI): the
// contract lives in @baizor/gamedev-cli-core.

import { EXIT_CODES, type ExitCodeValue } from '@baizor/gamedev-cli-core';

export { ExitCode, UsageError } from '@baizor/gamedev-cli-core';
export type { ExitCodeValue } from '@baizor/gamedev-cli-core';

/** What each exit code means — printed by `commands` (the manifest agents read). */
export const EXIT_CODE_DESCRIPTIONS = Object.fromEntries(EXIT_CODES.map((e) => [e.code, e.description])) as Readonly<Record<ExitCodeValue, string>>;
