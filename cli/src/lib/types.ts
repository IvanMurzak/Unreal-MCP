// Shared public types for the `unreal-mcp-cli` library API.
//
// Re-exported from `lib.ts` — consumers import from `unreal-mcp-cli` (the
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

import type { MachineAuthOptions } from '../utils/config.js';
import type { ExtensionDescriptor } from '../utils/extensions-catalog.js';
import type { InstallSourceKind } from '../utils/extension-source.js';
import type { DismissOutcome, DismissPlatform, UnrealStartupDialogKey } from '../utils/startup-dialog-dismiss.js';
import type { EnrollOptions, EnrollResult } from './enroll.js';

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
  | {
      phase: 'startup-dialog-dismissed';
      message: string;
      button: string;
      dialog: UnrealStartupDialogKey;
      platform: DismissPlatform;
    }
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
  /**
   * On desktop platforms, compile native modules via Unreal build tooling before launch when the
   * project/plugin state requires it. Default `true`.
   */
  build?: boolean;
  /**
   * On desktop platforms, poll for known Unreal startup blocker dialogs after launch and
   * auto-click the affirmative button. Default `true`.
   */
  autoDismissStartupDialogs?: boolean;
  /** Overall timeout for the startup-dialog auto-dismiss polling loop. Default `12000`. */
  startupDismissTimeoutMs?: number;
  /** Polling interval for the startup-dialog auto-dismiss loop. Default `1000`. */
  startupDismissPollIntervalMs?: number;
  /** Skip wiring MCP connection env vars onto the editor process. */
  noConnect?: boolean;
  host?: string;
  token?: string;
  auth?: AuthOption;
  tools?: string;
  keepConnected?: boolean;
  transport?: McpTransport;
  startServer?: boolean;
  /**
   * UNREAL_MCP_CONNECTION_MODE — `"Custom"` for a local server, `"Cloud"` for
   * ai-game.dev. When omitted, an explicit `host` infers `"Custom"` (a host
   * implies a non-Cloud target; the plugin otherwise defaults to Cloud and
   * ignores the host). An explicit value always wins; with neither `host` nor
   * `connectionMode` the var is omitted and the plugin default applies.
   */
  connectionMode?: string;
  /** Skip the persistent engine-path cache (forces a fresh discovery chain). */
  noCache?: boolean;
  /** Test injection — installed engines (defaults to launcher manifest). */
  enginesImpl?: () => import('../utils/launcher.js').EngineInstallation[];
  /**
   * Test/embed injection — the persistent engine-path cache I/O surface
   * (defaults to the real `~/.unreal-mcp-cli-engine-cache.json`). Supply an
   * in-memory/temp surface so a unit suite or library embedder never reads or
   * writes the user's real home cache file.
   */
  cacheIo?: import('../utils/engine-cache.js').EngineCacheIo;
  /**
   * Test/embed injection — the common-location scan's filesystem surface
   * (defaults to the real `fs`). Supply a fake to keep discovery hermetic
   * (no `fs.readdirSync` of real engine-install roots).
   */
  discoveryFs?: import('../utils/engine-discovery.js').DiscoveryFs;
  /**
   * Test/embed injection — the Windows-registry reader for source builds
   * (defaults to the real `reg query`; no-op off Windows). Supply a fake so
   * discovery never shells out to the host registry.
   */
  registryQueryImpl?: import('../utils/engine-discovery.js').RegistryQueryImpl;
  /** Test/embed injection — binary existence check (defaults to `fs.existsSync`). */
  existsImpl?: (p: string) => boolean;
  /** Test injection — spawn (defaults to detached `child_process.spawn`). */
  spawnImpl?: (editorPath: string, args: string[], env: NodeJS.ProcessEnv) => { pid?: number };
  /** Test injection — build runner used by the pre-launch build. */
  buildImpl?: (step: UbtBuildStep) => Promise<void>;
  /** Test injection — startup-dialog probe used by the post-launch dismiss loop. */
  dismissStartupDialogImpl?: (platform: DismissPlatform) => Promise<DismissOutcome>;
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
  /**
   * Optional source `UnrealMCP/` plugin directory to install. When omitted, the
   * library uses a local repo checkout if one is present, otherwise it downloads
   * the GitHub Release asset that matches the CLI package version.
   */
  pluginSourceDir?: string;
  /**
   * `--version` escape hatch: the plugin-source release version to download when
   * no local source is used. Defaults to the CLI's own `PACKAGE_VERSION`.
   */
  version?: string;
  /** Junction (dev) instead of copy. Windows only. Default `false`. */
  junction?: boolean;
  /**
   * `--with-server`: after installing the plugin, download the RID-matched
   * `gamedev-mcp-server` binary into the CLI-managed dir (checksum-verified via
   * the existing SHA256SUMS flow), so an offline user needs no .NET SDK (D12).
   */
  withServer?: boolean;
  /** `--server-version <v>`: server version to download for `--with-server`. Defaults to the pinned `SERVER_VERSION`. */
  serverVersion?: string;
  /** `--server-source <path-or-url>`: offline/CI escape hatch for the `--with-server` download (see `DownloadServerOptions.source`). */
  serverSource?: string;
  /**
   * `--enroll <code>` / `--enroll-stdin`: redeem a D13 enrollment code for a
   * plugin credential (→ shared machine store + project marker + pin upsert). No
   * browser hop. The code is single-use and burns on the first attempt.
   */
  enrollCode?: string;
  /** Auth base URL for `--enroll` (defaults to `https://ai-game.dev`). Test injection. */
  baseUrl?: string;
  /** Override the machine credential store base dir for `--enroll` (default `~/.ai-game-dev`). Test injection. */
  storeBaseDir?: string;
  /**
   * `--yes`: confirm replacing the machine's stored account when the redeemed
   * enrollment credential resolves to a DIFFERENT subject (D6/F7). Without it a
   * mismatch is declined fail-closed and nothing is written.
   */
  assumeYes?: boolean;
  /** Injectable fetch for tests (plugin download + server download + enroll). */
  fetchImpl?: typeof fetch;
  /**
   * Test/injection seam for the pinned publisher key the downloaded plugin
   * source's `.minisig` is verified against (see `ResolvePluginSourceOptions`).
   * Production callers leave this unset (the baked-in key is used).
   */
  publicKeyOverride?: string;
  /** Injectable clock for the enrollment credential's `expiresAt`. Test injection. */
  nowImpl?: () => number;
  /** Inject the `--with-server` acquisition (defaults to `downloadServer`). Test injection. */
  downloadServerImpl?: (opts: DownloadServerOptions) => Promise<DownloadServerResult>;
  /** Inject the `--enroll` redemption (defaults to `enrollPlugin`). Test injection. */
  enrollImpl?: (opts: EnrollOptions) => Promise<EnrollResult>;
  onProgress?: ProgressCallback;
}

export interface InstallPluginSuccess {
  kind: 'success';
  success: true;
  /** Absolute path to `<project>/Plugins/UnrealMCP`. */
  installedPath: string;
  mode: 'copy' | 'junction';
  /** `--with-server`: absolute path of the downloaded server binary, when acquired. */
  serverPath?: string;
  /** `--with-server`: installed server version, when acquired. */
  serverVersion?: string | null;
  /** `--enroll`: `true` when a credential was redeemed + persisted. */
  enrolled?: boolean;
  /** `--enroll`: the server-target URL the code was minted for. */
  serverTarget?: string;
  /** `--enroll`: the D14 routing pin derived for this project. */
  pin?: string;
  /** `--enroll`: project-local agent config files whose URL was pinned. */
  pinnedConfigFiles?: string[];
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
  | 'aborted'
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
  /** Machine-store token-fallback injection (tests). See `MachineAuthOptions`. */
  machineAuth?: MachineAuthOptions;
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
  /**
   * `--no-pin`: write an UNPINNED `<base>/mcp` URL instead of the default pinned
   * `<base>/mcp/p/<pin-v2>` (T4 escape hatch). Default `false` (pinned).
   */
  noPin?: boolean;
  /** Return the snippet instead of writing it. Default `false` (write). */
  dryRun?: boolean;
  /**
   * Env source for the `UNREAL_MCP_SERVER_PATH` override (stdio transport).
   * Defaults to `process.env`. Test injection.
   */
  env?: NodeJS.ProcessEnv;
  /**
   * Inject the stdio server-binary acquisition (defaults to the real
   * `downloadServer`). Test injection.
   */
  downloadServerImpl?: (opts: DownloadServerOptions) => Promise<DownloadServerResult>;
  /** Machine-store token-fallback injection (tests). See `MachineAuthOptions`. */
  machineAuth?: MachineAuthOptions;
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
// download-server
// ---------------------------------------------------------------------------

export interface DownloadServerOptions {
  /** Project root — the server lands under `Intermediate/UnrealMCP/server/<rid>/`. */
  projectDir: string;
  os?: NodeJS.Platform;
  arch?: string;
  /** Server version to download. Defaults to the pinned `SERVER_VERSION`. */
  version?: string;
  /**
   * Offline / CI escape hatch (the `install-plugin --server-source` flag): a
   * local path or URL supplying the server binary directly instead of the
   * pinned GitHub release. Accepts a local `.zip`, an already-extracted
   * directory, a bare binary file, or an `http(s)://` URL to a `.zip`. Because
   * it is an EXPLICIT user-provided artifact, the release `SHA256SUMS` gate is
   * skipped for this path (the gate protects the DEFAULT download only). Wins
   * over the network download; the `UNREAL_MCP_SERVER_PATH` env override still
   * wins over this.
   */
  source?: string;
  /** Env source for the `UNREAL_MCP_SERVER_PATH` override (test injection). */
  env?: NodeJS.ProcessEnv;
  /** Inject the HTTP client (defaults to global `fetch`). */
  fetchImpl?: typeof fetch;
  /** Re-download even when the cached `version` marker matches. Default `false`. */
  force?: boolean;
  onProgress?: ProgressCallback;
}

export interface DownloadServerSuccess {
  kind: 'success';
  success: true;
  /** Absolute path to the resolved server binary. */
  serverPath: string;
  /** How the binary was resolved. */
  source: 'override' | 'cache' | 'download' | 'source';
  /** Installed server version (`null` for an override — version unknown/unchecked). */
  version: string | null;
  warnings: string[];
}

export interface DownloadServerFailure {
  kind: 'failure';
  success: false;
  /** The release-asset URL involved, when the failure happened at/after download. */
  url?: string;
  warnings: string[];
  error: Error;
}

export type DownloadServerResult = DownloadServerSuccess | DownloadServerFailure;

// ---------------------------------------------------------------------------
// install-engine
// ---------------------------------------------------------------------------

export interface DetectEnginesOptions {
  /** Explicit manifest path (defaults to the platform location). */
  manifestPath?: string;
  os?: NodeJS.Platform;
  /**
   * Test injection — the common-location engine scan. Defaults to scanning the
   * host's popular Epic-install roots, so an engine that is installed on disk
   * but not (yet) listed in the launcher manifest is still detected. Tests pass
   * `() => []` to isolate the manifest, or a stub to assert the union/dedup.
   */
  scanImpl?: (os: NodeJS.Platform) => import('../utils/launcher.js').EngineInstallation[];
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
  /** Test injection — common-location scan (see DetectEnginesOptions.scanImpl). */
  scanImpl?: (os: NodeJS.Platform) => import('../utils/launcher.js').EngineInstallation[];
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
// auto-install-engine (best-effort, consent-gated)
// ---------------------------------------------------------------------------

/**
 * Why an auto-install attempt did NOT download/install an engine. Drives both
 * the result message and the caller's exit behaviour.
 */
export type AutoInstallEngineOutcome =
  | 'already-installed'
  /** No unattended install path on this OS → returned guided steps + deep link. */
  | 'guidance-only'
  /** Consent was required but not granted (no `--yes`, non-interactive). */
  | 'consent-required'
  /** An install was attempted but the result did not resolve a real binary. */
  | 'install-unverified';

export interface AutoInstallEngineOptions {
  /** Engine version/association to ensure (e.g. `"5.7"`). */
  version: string;
  /** Target OS; defaults to the host. */
  os?: NodeJS.Platform;
  /**
   * Explicit consent to spend bandwidth/disk on a multi-GB engine install.
   * Without it, an OS that COULD download refuses and returns guidance —
   * never a silent multi-GB download. (Today every OS degrades to guidance,
   * but the gate is wired so a future real download path stays opt-in.)
   */
  consent?: boolean;
  /** Whether the caller is interactive (a TTY). Defaults to `false` (CI-safe). */
  interactive?: boolean;
  /**
   * Injected check for "is the requested engine resolvable now?" — used both to
   * short-circuit when already installed and to VALIDATE after any install
   * attempt (never report success unless this returns a real binary path).
   */
  resolveInstalledImpl?: (version: string, os: NodeJS.Platform) => string | null;
}

export interface AutoInstallEngineResult {
  kind: 'success';
  success: true;
  version: string;
  outcome: AutoInstallEngineOutcome;
  /** Resolved editor binary when the engine is (or became) installed. */
  editorPath: string | null;
  /** `com.epicgames.launcher://` deep link (always present for guidance). */
  launcherUrl: string;
  /** Per-OS guided install steps to print when no unattended path exists. */
  guidance: string[];
  /** One-line human-facing summary. */
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
  /** Machine-store token-fallback injection (tests). See `MachineAuthOptions`. */
  machineAuth?: MachineAuthOptions;
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
  /** Machine-store token-fallback injection (tests). See `MachineAuthOptions`. */
  machineAuth?: MachineAuthOptions;
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
  /** Project root — the bridge lands under `Intermediate/UnrealMCP/bridge/<rid>/`. */
  projectDir: string;
  /** Path to the Unreal-MCP repo root holding `bridge/`. */
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

// ---------------------------------------------------------------------------
// install-extension
// ---------------------------------------------------------------------------

/** How an `install-extension` run ended (a `kind: 'success'` classification). */
export type ExtensionInstallOutcome = 'added' | 'updated' | 'enabled' | 'already-up-to-date';

/** Re-export of the install-source channel discriminant for the public result. */
export type { InstallSourceKind };

/** A single desktop Unreal build invocation. */
export interface UbtBuildStep {
  /** Absolute path to the build entrypoint (`UnrealBuildTool.exe` or platform `Build.sh`). */
  ubtPath: string;
  /** Editor target name, e.g. `MyProjectEditor`. */
  editorTarget: string;
  /** Absolute path to the `.uproject`. */
  uprojectPath: string;
  /** The full build argument vector (target, platform, config, `-project=...`, `-WaitMutex`). */
  args: string[];
}

/**
 * A single UBT compile invocation `install-extension --build` runs. Exposed so a
 * test can inject `buildImpl` and assert the build was requested without a real
 * UnrealBuildTool / engine on the machine.
 */
export interface ExtensionBuildStep extends UbtBuildStep {}

export interface InstallExtensionOptions {
  /** Target Unreal project root (holds the `.uproject` + `Plugins/`). */
  projectDir: string;
  /**
   * The extension `<id>` to install — matched against the catalog by
   * `extensionId`, then `name`, then `pluginName` (case-insensitive). When the id
   * is NOT in the catalog, `source` is required and the descriptor is synthesized
   * from the `--source` dir's `.uplugin`.
   */
  extensionId: string;
  /**
   * Local source dir (offline / CI / dev): a UE plugin directory containing
   * `<pluginName>.uplugin`, or a directory that contains one. Wins over the
   * GitHub-release download channel. Mirrors godot-cli `install-plugin --source`.
   */
  source?: string;
  /** Override the catalog-pinned version (also the version requested from the GitHub release). Ignored when empty. */
  version?: string;
  /**
   * Compile the project via UBT after install. Default `false` — the extension
   * ships as SOURCE and UE recompiles it on the next editor open (the documented
   * compile-on-install strategy); `--build` opts into eager compilation.
   */
  build?: boolean;
  /** Engine root for the `--build` UBT invocation. Defaults to discovery from the project's `EngineAssociation`. */
  engineRoot?: string;
  /** Re-materialize + re-enable even when already up to date. Default `false`. */
  force?: boolean;
  /**
   * Catalog to resolve `extensionId` against. Defaults to the bundled
   * `EXTENSIONS_CATALOG`. Injectable for tests.
   */
  catalog?: readonly ExtensionDescriptor[];
  /** Inject the HTTP client (defaults to global `fetch`). Test injection. */
  fetchImpl?: typeof fetch;
  /**
   * Abort the GitHub-release download if no response arrives within this many
   * milliseconds, so a stalled/slow server cannot hang the CLI indefinitely.
   * Default 60000. Ignored for `--source` (local) installs.
   */
  downloadTimeoutMs?: number;
  /**
   * Inject the UBT build runner (defaults to spawning UnrealBuildTool). Test
   * injection — a fake records the `ExtensionBuildStep` without a real engine.
   */
  buildImpl?: (step: ExtensionBuildStep) => Promise<void>;
  /** Inject engine-root resolution for `--build` (defaults to the engine-discovery chain). Test injection. */
  resolveEngineRootImpl?: (engineAssociation: string) => string | null;
  onProgress?: ProgressCallback;
}

export interface InstallExtensionSuccess {
  kind: 'success';
  success: true;
  /** Classified outcome (added / updated / enabled / already-up-to-date). */
  outcome: ExtensionInstallOutcome;
  /** True when files were materialized and/or the `.uproject` was written. */
  changed: boolean;
  /** True when the project must be (re)compiled — i.e. files changed and `--build` did NOT run. */
  rebuildRequired: boolean;
  /** The user-supplied id that resolved to the descriptor. */
  extensionId: string;
  /** Resolved plugin folder/descriptor name (`Plugins/<pluginName>`). */
  pluginName: string;
  /** Absolute path the extension was installed to (`<project>/Plugins/<pluginName>`). */
  installedPath: string;
  /** Which channel the files came from. */
  sourceKind: InstallSourceKind;
  /** Version installed before this run: `null` when not previously installed. */
  fromVersion: string | null;
  /** Version this install targeted: catalog pin / `--version` / the source `.uplugin`'s `VersionName`, or `null`. */
  toVersion: string | null;
  /**
   * Plugins this run newly enabled in the `.uproject` — the extension and any
   * gating engine plugins it appended or flipped from disabled. Plugins that
   * were already enabled before the run are NOT listed.
   */
  enabledPlugins: string[];
  /** Absolute path to the `.uproject` touched. */
  uprojectPath: string;
  /** True when UBT compiled the project this run (`--build`). */
  built: boolean;
  /** A short human-readable status line, safe to print. */
  message: string;
  warnings: string[];
}

export interface InstallExtensionFailure {
  kind: 'failure';
  success: false;
  warnings: string[];
  error: Error;
}

export type InstallExtensionResult = InstallExtensionSuccess | InstallExtensionFailure;
