// The one source of the agent-facing usage text: the root --help, `rql --help` and the `unreal-mcp-cli` agent skill
// (`setup-skills`) all render from these constants, so the three cannot drift apart.
// Example names stay neutral (Forest, Cave, Ball, Tree, Rock, Enemy): never a name a test or evaluation task uses.

/** RQL recipes: [query, what it does]. */
export type RqlRecipes = ReadonlyArray<readonly [string, string]>;
/** A featured command: [name, usage, what it does]. */
export type CommandRow = readonly [string, string, string];

/** The CLI's command name. */
export const CLI_NAME = 'unreal-mcp-cli';

/** The recipes `rql --help` and the skill show; the root help shows the first {@link ROOT_RQL_RECIPES}. */
export const RQL_RECIPES: RqlRecipes = [
  ['editor | call OpenLevel("/Game/Maps/Forest")', 'open a level (writes no file, drops unsaved edits): actors lists the open level and its loaded sublevels'],
  ['editor | call NewLevel("/Game/Maps/Cave")', 'a new empty level, saved at that path, now open'],
  ['editor | call SpawnActor("/Engine/BasicShapes/Sphere", "Ball", [100, 0, 50])', 'class / asset, label, location (cm)'],
  ['actors | where Label starts-with "Tree" | select Label Location Scale', 'read'],
  ['actors | where Label contains "Enemy" | count', 'count'],
  ['actors | where Label starts-with "Rock" | set Scale = Scale * 1.5, Location.Z = 0', 'change (vectors compute per axis)'],
  ['editor | call SaveLevel()', 'write the open level to disk; prints the files written (SaveAll() saves everything)'],
  ['actors | where Label == "Old" | call Destroy()', 'delete'],
  ['unreal.assets("StaticMesh", "/Game") | select Name Path', 'assets of a class under a folder'],
  ['actors | get Components | where $ is PointLightComponent | set Intensity = 5000', 'components'],
];

/** How many recipes the root help shows. */
export const ROOT_RQL_RECIPES = 7;

/** The RQL language in two lines (`rql --help`, the skill's RQL reference). */
export const RQL_LANGUAGE = [
  'Stages: where, select a b, sort x [desc], first N, count, get <member>, set a = expr, call Method(args).',
  'Operators: == != < <= > >= and or not, contains, starts-with, ends-with, matches "regex", x in [a, b], $ is PointLight.',
];

/** The editor commands the root help and the skill feature. */
export const EDITOR_COMMANDS: readonly CommandRow[] = [
  ['rql', "rql '<query>'", 'read or change levels, actors, components, assets (RQL; recipes below)'],
  ['exec', "exec '<python>'", 'Python with the full `unreal` API (the last expression is printed)'],
  ['tools', 'tools [--filter <regex>]', "the editor's AI tools; run-tool <tool> --input '<json>' calls one"],
];

/** The C++ commands (no editor needed). */
export const CODE_COMMANDS: readonly CommandRow[] = [
  ['compile', 'compile', 'build the C++; prints file:line errors'],
  ['test', 'test --filter <Prefix>', 'run Automation tests headless'],
];

/** How an editor command gets its editor (the root help's heading). */
export const EDITOR_START = 'the first call starts a headless editor, 1-2 min, and keeps it warm; no open / status needed';

/** Where the plugin lives in a project: absent means `install-plugin` comes first. */
export const PLUGIN_FOLDER = 'Plugins/UnrealMCP';

/**
 * The rules an agent needs beyond the recipes (the skill's list). Each answers a stumble seen in agent transcripts:
 * a query on the wrong level, unsaved work, re-checking a change its receipt already proves. Kept terse: the agent
 * re-reads every byte of the skill on every later turn, and a rule worded as a warning invites extra checks.
 */
export const RQL_RULES = [
  '`actors` lists only the open level (OpenLevel first). Edits stay unsaved until `SaveLevel()`; `NewLevel` saves itself.',
  'The output of `set` / `call` lists each change (old, new) and the files saved: that is the check, no re-read needed.',
  'Match actors on `Label` (`Name` is internal). A wrong source or member fails with the valid names.',
];
