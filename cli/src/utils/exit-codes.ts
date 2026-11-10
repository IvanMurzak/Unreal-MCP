// Process exit codes shared by unreal-mcp-cli, unity-mcp-cli and godot-cli (one contract for agents and CI).

export const ExitCode = {
  Success: 0,
  Error: 1,
  Usage: 2,
  CommandFailed: 6,
  Unreachable: 7,
  TestsFailed: 8,
  Timeout: 14,
  Busy: 17,
  AwaitingInput: 18,
} as const;

export type ExitCodeValue = (typeof ExitCode)[keyof typeof ExitCode];

/** What each exit code means — printed by `commands` (the manifest agents read). */
export const EXIT_CODE_DESCRIPTIONS: Readonly<Record<ExitCodeValue, string>> = {
  [ExitCode.Success]: 'Success',
  [ExitCode.Error]: 'Generic or unclassified error',
  [ExitCode.Usage]: 'Usage error: bad flag or argument',
  [ExitCode.CommandFailed]: 'The command ran and failed (tool error, compilation errors, test run without a verdict)',
  [ExitCode.Unreachable]: 'The editor or MCP server could not be started or reached',
  [ExitCode.TestsFailed]: 'The test run completed with failing tests',
  [ExitCode.Timeout]: 'Timed out',
  [ExitCode.Busy]: 'Busy: the server answered 503 and did not run the request',
  [ExitCode.AwaitingInput]: 'The editor is blocked by a modal dialog',
};

/** A bad flag or argument: the CLI entry point exits with {@link ExitCode.Usage}. */
export class UsageError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'UsageError';
  }
}
