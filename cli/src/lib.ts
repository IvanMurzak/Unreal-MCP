// Library entry point for `unreal-mcp-cli` (the `dist/lib.js` export — see
// docs/ARCHITECTURE.md §9.1).
//
// Contract (same as unity-mcp-cli / godot-cli):
// - NO top-level side effects. Importing this file must not open sockets,
//   write to stdout/stderr, or parse argv.
// - NO `commander` import reachable from this file.
// - Every result is a discriminated union keyed on `kind`; errors are never
//   thrown past the public boundary.
//
// Consumers: `import { configure, openProject } from 'unreal-mcp-cli'`.

// --- command logic ---------------------------------------------------------
export { configure } from './lib/configure.js';
export { openProject, buildOpenEnv } from './lib/open.js';
export { close, selectEditorProcesses } from './lib/close.js';
export { createProject, renderProjectTemplate, validateModuleName } from './lib/create-project.js';
export { installPlugin, removePlugin } from './lib/install-plugin.js';
export { runTool, runSystemTool } from './lib/run-tool.js';
export { setupMcp, listAgentIds, buildServerEntry } from './lib/setup-mcp.js';
export {
  downloadServer,
  serverDownloadUrl,
  serverExecutableName,
  serverInstallDir,
  resolveServerBinaryPath,
  resolveServerOverride,
  SERVER_BINARY_BASENAME,
  SERVER_PATH_ENV_VAR,
} from './lib/download-server.js';
export { SERVER_VERSION } from './lib/server-version.js';
export { login } from './lib/login.js';
export { getStatus } from './lib/status.js';
export { waitForReady } from './lib/wait-for-ready.js';
export { bootstrapLocal, planBuildSteps, ridForPlatform } from './lib/bootstrap-local.js';
export { update, readPluginVersion } from './lib/update.js';
export { cleanPluginBuildCache, BUNDLED_BRIDGE_DIRNAME } from './lib/clean-plugin.js';
export { setupSkills, renderSkill } from './lib/setup-skills.js';
export {
  detectInstalledEngines,
  planEngineInstall,
  launcherInstallUrl,
} from './lib/install-engine.js';
export { autoInstallEngine, engineInstallGuidance } from './lib/auto-install-engine.js';

// --- engine resolution -----------------------------------------------------
export {
  listInstalledEngines,
  resolveEngineForProject,
  discoverEngine,
  invalidateCachedEngine,
  engineRootFromEditorPath,
} from './lib/engine.js';
export { resolveEngine, editorBinaryPath } from './utils/engine.js';
export {
  parseLauncherManifest,
  matchEngineForAssociation,
  getDefaultLauncherManifestPath,
} from './utils/launcher.js';
export {
  readCachedEnginePath,
  writeCachedEnginePath,
  clearCachedEnginePath,
  defaultCacheFilePath,
  keyFor as engineCacheKeyFor,
  AUTO_KEY as ENGINE_CACHE_AUTO_KEY,
} from './utils/engine-cache.js';
export {
  scanCommonLocationEngines,
  commonEngineRoots,
  readEngineAssociationFromBuildVersion,
  readRegistryEngineBuilds,
  parseRegistryBuilds,
  matchRegistryBuild,
  linuxEngineDirCandidates,
  UE_BUILDS_REGISTRY_KEY,
} from './utils/engine-discovery.js';
export { readUProject, readEngineAssociation, findUProjectFile } from './utils/project.js';
export { generatePortFromDirectory } from './utils/port.js';
export { resolveConnection } from './utils/config.js';
export {
  parseEnvContent,
  writeEnvFile,
  ensureEnvGitignored,
  gitignoreAlreadyIgnoresEnv,
  KNOWN_ENV_KEYS,
} from './utils/env-file.js';

export { PACKAGE_NAME, PACKAGE_VERSION } from './version.js';

// --- types -----------------------------------------------------------------
export type {
  ResultKind,
  ProgressEvent,
  ProgressCallback,
  ConnectionMode,
  AuthOption,
  McpTransport,
  ConfigureOptions,
  ConfigureResult,
  ConfigureSuccess,
  ConfigureFailure,
  OpenProjectOptions,
  OpenProjectResult,
  OpenProjectSuccess,
  OpenProjectFailure,
  CreateProjectOptions,
  CreateProjectResult,
  CreateProjectSuccess,
  CreateProjectFailure,
  InstallPluginOptions,
  InstallPluginResult,
  InstallPluginSuccess,
  InstallPluginFailure,
  RemovePluginOptions,
  RemovePluginResult,
  RemovePluginSuccess,
  RemovePluginFailure,
  RunToolOptions,
  RunToolResult,
  RunToolSuccess,
  RunToolFailure,
  RunToolFailureReason,
  SetupMcpOptions,
  SetupMcpResult,
  SetupMcpSuccess,
  SetupMcpFailure,
  DownloadServerOptions,
  DownloadServerResult,
  DownloadServerSuccess,
  DownloadServerFailure,
  DetectEnginesOptions,
  DetectEnginesResult,
  PlanEngineInstallOptions,
  PlanEngineInstallResult,
  AutoInstallEngineOptions,
  AutoInstallEngineResult,
  AutoInstallEngineOutcome,
  WaitForReadyOptions,
  WaitForReadyResult,
  WaitForReadySuccess,
  WaitForReadyFailure,
  StatusOptions,
  StatusReport,
  BootstrapLocalOptions,
  BootstrapLocalResult,
  BootstrapLocalSuccess,
  BootstrapLocalFailure,
  BuildStep,
} from './lib/types.js';
export type { EngineInstallation } from './utils/launcher.js';
export type {
  EngineCacheEntry,
  EngineCache,
  EngineCacheIo,
} from './utils/engine-cache.js';
export type {
  DiscoveryFs,
  RegistryEngineBuild,
  RegistryQueryImpl,
} from './utils/engine-discovery.js';
export type {
  DiscoverEngineInput,
  DiscoverEngineResult,
  EngineDiscoverySource,
} from './lib/engine.js';
export type { UProjectInfo } from './utils/project.js';
export type { ResolvedConnection } from './utils/config.js';
export type { LoginOptions, LoginResult } from './lib/login.js';
export type { UpdateOptions, UpdateResult } from './lib/update.js';
export type {
  CleanPluginOptions,
  CleanPluginResult,
  CleanPluginSuccess,
  CleanPluginFailure,
} from './lib/clean-plugin.js';
export type { SetupSkillsOptions, SetupSkillsResult } from './lib/setup-skills.js';
export type { CloseOptions, CloseResult, RunningProcess } from './lib/close.js';
