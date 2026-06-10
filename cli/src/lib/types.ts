// Shared public types for the `unreal-cli` library API.
//
// Re-exported from `lib.ts` — consumers import from `unreal-cli` (the
// package root), never from deep paths.
//
// Contract (same as unity-mcp-cli / godot-cli):
// - Every result is a discriminated union keyed on `kind`. Successes are
//   `{ kind: 'success', success: true, ... }`; failures are
//   `{ kind: 'failure', success: false, error }`. `success === (kind ===
//   'success')` always holds for wire compatibility.
// - Errors are never thrown past the public boundary.
// - Progress is surfaced via an optional `onProgress` callback.
//
// No top-level side effects; no runtime deps beyond TypeScript types.

export type ResultKind = 'success' | 'failure';

// ---------------------------------------------------------------------------
// Progress events
// ---------------------------------------------------------------------------

export type ProgressEvent =
  | { phase: 'start'; message: string }
  | { phase: 'info'; message: string }
  | { phase: 'file-written'; message: string; filePath: string }
  | { phase: 'engine-resolved'; message: string; editorPath: string; engineRoot: string }
  | { phase: 'launching'; message: string; editorPath: string; projectDir: string }
  | { phase: 'launched'; message: string; pid?: number }
  | { phase: 'done'; message: string };

export type ProgressCallback = (event: ProgressEvent) => void;

// ---------------------------------------------------------------------------
// configure
// ---------------------------------------------------------------------------

/** Connection mode written to `UNREAL_MCP_CONNECTION_MODE`. */
export type ConnectionMode = 'Cloud' | 'Custom';
export type AuthOption = 'none' | 'required';
export type McpTransport = 'stdio' | 'http';

export interface ConfigureOptions {
  /** Target Unreal project root (where `.env` / `.gitignore` live). */
  projectDir: string;
  connectionMode?: ConnectionMode;
  host?: string;
  cloudUrl?: string;
  token?: string;
  authOption?: AuthOption;
  keepConnected?: boolean;
  tools?: string;
  startServer?: boolean;
  transport?: McpTransport;
  logLevel?: string;
  /**
   * When `true` (default), append `.env` to the project's `.gitignore`,
   * creating it if absent. Set `false` only for tests that assert the
   * env-write path in isolation.
   */
  ensureGitignore?: boolean;
  onProgress?: ProgressCallback;
}

export interface ConfigureSuccess {
  kind: 'success';
  success: true;
  /** Absolute path to the `.env` that was written. */
  envPath: string;
  /** Keys added / updated this run. */
  keysWritten: string[];
  /** Absolute path to the `.gitignore` touched (if `ensureGitignore`). */
  gitignorePath?: string;
  /** What happened to `.gitignore`. */
  gitignoreAction?: 'created' | 'appended' | 'already-ignored' | 'skipped';
  warnings: string[];
}

export interface ConfigureFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type ConfigureResult = ConfigureSuccess | ConfigureFailure;

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

export interface OpenProjectOptions {
  /** Project root or `.uproject` path; defaults to `process.cwd()`. */
  projectDir?: string;
  /** Explicit engine root override (source builds). */
  engineRoot?: string;
  /** Skip wiring MCP connection env vars onto the editor process. */
  noConnect?: boolean;
  host?: string;
  token?: string;
  auth?: AuthOption;
  tools?: string;
  keepConnected?: boolean;
  transport?: McpTransport;
  startServer?: boolean;
  /** Test injection — installed engines (defaults to launcher manifest). */
  enginesImpl?: () => import('../utils/launcher.js').EngineInstallation[];
  /** Test injection — spawn (defaults to detached `child_process.spawn`). */
  spawnImpl?: (editorPath: string, args: string[], env: NodeJS.ProcessEnv) => { pid?: number };
  onProgress?: ProgressCallback;
}

export interface OpenProjectSuccess {
  kind: 'success';
  success: true;
  editorPath: string;
  engineRoot: string;
  projectDir: string;
  editorPid?: number;
  envVars: Record<string, string>;
  warnings: string[];
}

export interface OpenProjectFailure {
  kind: 'failure';
  success: false;
  projectDir?: string;
  warnings: string[];
  errorMessage: string;
  error: Error;
}

export type OpenProjectResult = OpenProjectSuccess | OpenProjectFailure;

// ---------------------------------------------------------------------------
// create-project
// ---------------------------------------------------------------------------

export interface CreateProjectOptions {
  /** Where to create the project (the directory is created). */
  projectDir: string;
  /** Project / primary module name. Defaults to the directory basename. */
  projectName?: string;
  /** `EngineAssociation` to bake into the `.uproject` (e.g. `"5.7"`). */
  engineAssociation?: string;
  /** Overwrite an existing non-empty directory. Default `false`. */
  force?: boolean;
  onProgress?: ProgressCallback;
}

export interface CreateProjectSuccess {
  kind: 'success';
  success: true;
  projectDir: string;
  projectName: string;
  uprojectPath: string;
  filesWritten: string[];
  warnings: string[];
}

export interface CreateProjectFailure {
  kind: 'failure';
  success: false;
  projectDir?: string;
  warnings: string[];
  errorMessage: string;
  error: Error;
}

export type CreateProjectResult = CreateProjectSuccess | CreateProjectFailure;

// ---------------------------------------------------------------------------
// install-plugin / remove-plugin
// ---------------------------------------------------------------------------

export interface InstallPluginOptions {
  /** Target Unreal project root. */
  projectDir: string;
  /** Source `UnrealMCP/` plugin directory to install. */
  pluginSourceDir: string;
  /** Junction (dev) instead of copy. Windows only. Default `false`. */
  junction?: boolean;
  onProgress?: ProgressCallback;
}

export interface InstallPluginSuccess {
  kind: 'success';
  success: true;
  /** Absolute path to `<project>/Plugins/UnrealMCP`. */
  installedPath: string;
  mode: 'copy' | 'junction';
  warnings: string[];
}

export interface InstallPluginFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type InstallPluginResult = InstallPluginSuccess | InstallPluginFailure;

export interface RemovePluginOptions {
  projectDir: string;
  onProgress?: ProgressCallback;
}

export interface RemovePluginSuccess {
  kind: 'success';
  success: true;
  /** `true` when a plugin was present and removed. */
  removed: boolean;
  installedPath: string;
  warnings: string[];
}

export interface RemovePluginFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type RemovePluginResult = RemovePluginSuccess | RemovePluginFailure;

// ---------------------------------------------------------------------------
// run-tool / run-system-tool
// ---------------------------------------------------------------------------

export type RunToolFailureReason =
  | 'invalid-input'
  | 'connection-refused'
  | 'connection-reset'
  | 'network-error'
  | 'timeout'
  | 'http-error'
  | 'unknown';

export interface RunToolOptions {
  toolName: string;
  /** Project dir — resolves URL + token. Either this or `url` is required. */
  projectDir?: string;
  url?: string;
  token?: string;
  /** Tool arguments — JSON object, JSON string, or undefined (→ `{}`). */
  input?: unknown;
  timeoutMs?: number;
  signal?: AbortSignal;
  fetchImpl?: typeof fetch;
}

export interface RunToolSuccess {
  kind: 'success';
  success: true;
  endpoint: string;
  httpStatus: number;
  data: unknown;
}

export interface RunToolFailure {
  kind: 'failure';
  success: false;
  endpoint: string;
  reason: RunToolFailureReason;
  httpStatus?: number;
  data?: unknown;
  message: string;
  error?: Error;
}

export type RunToolResult = RunToolSuccess | RunToolFailure;

// ---------------------------------------------------------------------------
// setup-mcp
// ---------------------------------------------------------------------------

export interface SetupMcpOptions {
  /** Agent id — use `listAgentIds()` for valid values. */
  agentId: string;
  /** Project dir (defaults to `process.cwd()`). */
  projectDir?: string;
  transport?: McpTransport;
  url?: string;
  token?: string;
  /** Return the snippet instead of writing it. Default `false` (write). */
  dryRun?: boolean;
  onProgress?: ProgressCallback;
}

export interface SetupMcpSuccess {
  kind: 'success';
  success: true;
  agentId: string;
  /** Path the config was (or would be) written to. */
  configPath: string;
  transport: McpTransport;
  /** The JSON snippet that was written. */
  snippet: string;
  warnings: string[];
  nextSteps: string[];
}

export interface SetupMcpFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  nextSteps: string[];
  error: Error;
}

export type SetupMcpResult = SetupMcpSuccess | SetupMcpFailure;

// ---------------------------------------------------------------------------
// install-engine
// ---------------------------------------------------------------------------

export interface DetectEnginesOptions {
  /** Explicit manifest path (defaults to the platform location). */
  manifestPath?: string;
  os?: NodeJS.Platform;
}

export interface DetectEnginesResult {
  kind: 'success';
  success: true;
  /** Path the manifest was read from (or would be, when absent). */
  manifestPath: string | null;
  engines: import('../utils/launcher.js').EngineInstallation[];
}

export interface PlanEngineInstallOptions {
  /** Engine version to install, e.g. `"5.7"`. */
  version: string;
  manifestPath?: string;
  os?: NodeJS.Platform;
}

/**
 * `install-engine` never installs the engine itself — that is a multi-GB
 * Epic-launcher operation. It detects whether the requested version is
 * already present and otherwise hands back a launcher deep-link + a clear
 * user-facing message.
 */
export interface PlanEngineInstallResult {
  kind: 'success';
  success: true;
  version: string;
  /** `true` when the requested version is already installed. */
  alreadyInstalled: boolean;
  /** The matching installation when `alreadyInstalled`. */
  installation: import('../utils/launcher.js').EngineInstallation | null;
  /** `com.epicgames.launcher://` deep link that opens the install page. */
  launcherUrl: string;
  /** Human-facing guidance to print. */
  message: string;
}

// ---------------------------------------------------------------------------
// wait-for-ready
// ---------------------------------------------------------------------------

export interface WaitForReadyOptions {
  projectDir?: string;
  url?: string;
  token?: string;
  /** Overall timeout. Default 120000 ms. */
  timeoutMs?: number;
  /** Poll interval. Default 2000 ms. */
  intervalMs?: number;
  /** Per-probe timeout. Default 4000 ms. */
  probeTimeoutMs?: number;
  fetchImpl?: typeof fetch;
  /** Injectable clock for tests. */
  nowImpl?: () => number;
  /** Injectable sleep for tests. */
  sleepImpl?: (ms: number) => Promise<void>;
  onProgress?: ProgressCallback;
}

export interface WaitForReadySuccess {
  kind: 'success';
  success: true;
  url: string;
  elapsedMs: number;
  attempts: number;
}

export interface WaitForReadyFailure {
  kind: 'failure';
  success: false;
  url: string;
  elapsedMs: number;
  attempts: number;
  lastReason: string;
  error: Error;
}

export type WaitForReadyResult = WaitForReadySuccess | WaitForReadyFailure;

// ---------------------------------------------------------------------------
// status
// ---------------------------------------------------------------------------

export interface StatusOptions {
  projectDir?: string;
  url?: string;
  token?: string;
  /** Skip the live HTTP probe (offline status). Default `false`. */
  noProbe?: boolean;
  probeTimeoutMs?: number;
  fetchImpl?: typeof fetch;
}

export interface StatusReport {
  kind: 'success';
  success: true;
  name: string;
  version: string;
  /** Project info, when a project dir was provided + a `.uproject` found. */
  project?: {
    projectDir: string;
    projectName: string;
    engineAssociation: string;
    pluginInstalled: boolean;
  };
  connection: {
    url: string;
    source: string;
    hasToken: boolean;
  };
  /** Live probe result; absent when `noProbe`. */
  reachable?: boolean;
  probeReason?: string;
}

// ---------------------------------------------------------------------------
// bootstrap-local
// ---------------------------------------------------------------------------

export interface BootstrapLocalOptions {
  /** Project root — the bridge/server land under `Intermediate/UnrealMCP/`. */
  projectDir: string;
  /** Path to the Unreal-MCP repo root holding `bridge/` + `Unreal-MCP-Server/`. */
  repoRoot: string;
  os?: NodeJS.Platform;
  /** Inject the build runner (defaults to spawning `dotnet`). */
  buildImpl?: (step: BuildStep) => Promise<void>;
  onProgress?: ProgressCallback;
}

export interface BuildStep {
  label: string;
  /** Solution/project file to publish. */
  projectFile: string;
  /** Output directory under the project's `Intermediate/UnrealMCP/`. */
  outputDir: string;
  /** Runtime identifier, e.g. `win-x64`. */
  rid: string;
}

export interface BootstrapLocalSuccess {
  kind: 'success';
  success: true;
  /** The build steps that ran (or would run). */
  steps: BuildStep[];
  /** `<project>/Intermediate/UnrealMCP`. */
  outputRoot: string;
  warnings: string[];
}

export interface BootstrapLocalFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type BootstrapLocalResult = BootstrapLocalSuccess | BootstrapLocalFailure;
