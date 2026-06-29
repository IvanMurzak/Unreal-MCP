// `install-extension` — install an Unreal-MCP extension (a UE C++ Editor/Runtime
// plugin implementing `IUnrealMcpToolProvider`) into a user's Unreal project.
// Library-safe (the exported `installExtension`, parity with `installPlugin`):
// no stdout noise, no `process.exit`, no throws past the public boundary — every
// result is a `{ kind: 'success' | 'failure' }` discriminated union.
//
// The flow, mirroring godot-cli's `installExtension` shape but with UE placement:
//
//   1. RESOLVE the `<id>` to a descriptor in the shared catalog
//      (`extensions-catalog.ts`); or, when the id is unknown but `--source <dir>`
//      is given, SYNTHESIZE a descriptor from that dir's `.uplugin`.
//   2. RESOLVE the install source via the pluggable resolver
//      (`extension-source.ts`): `--source <dir>` (local/CI/offline) or the
//      extension's GitHub-release zip (github.com-only, fail-closed). A future
//      Fab channel slots in there without touching this file's callers.
//   3. PLACE the plugin into `<project>/Plugins/<pluginName>/` (stage-then-swap,
//      zip-slip-guarded, build-cache filtered) — idempotent.
//   4. ENABLE the extension AND its gating engine plugins (e.g. Niagara, read
//      from the extension's own `.uplugin` `Plugins[]` ∪ the catalog hint) in the
//      `.uproject` `Plugins[]` array.
//   5. COMPILE: the extension ships as SOURCE; by default UE recompiles it on the
//      next editor open (`rebuildRequired: true`). `--build` opts into an eager
//      UBT compile against the resolved engine.
//
// Idempotent: a re-run that finds the plugin present at the same version and the
// `.uproject` already enabled reports `already-up-to-date` and writes nothing.

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { spawn } from 'child_process';
import { unzipSync } from 'fflate';
import { asError } from '../utils/error.js';
import { readUProject } from '../utils/project.js';
import { discoverEngine } from './engine.js';
import {
  EXTENSIONS_CATALOG,
  findExtension,
  hasVersion,
  unknownExtensionMessage,
  type ExtensionDescriptor,
} from '../utils/extensions-catalog.js';
import { resolveInstallSource, type InstallSource } from '../utils/extension-source.js';
import {
  parseUPluginDependencies,
  readUPluginVersionName,
  enablePluginsInUProject,
} from '../utils/uproject-plugins.js';
import { emitProgress } from './progress.js';
import type {
  ExtensionBuildStep,
  ExtensionInstallOutcome,
  InstallExtensionOptions,
  InstallExtensionResult,
} from './types.js';

/** Plugin-source subtrees never copied into the target project (stale build cache / VCS noise). */
const EXCLUDED_DIRS = new Set([
  'Intermediate',
  'Binaries',
  'Saved',
  'DerivedDataCache',
  '.git',
  '.vs',
  'node_modules',
]);

export async function installExtension(
  opts: InstallExtensionOptions,
): Promise<InstallExtensionResult> {
  const warnings: string[] = [];
  try {
    if (!opts?.projectDir) throw new Error('projectDir is required.');
    if (!opts?.extensionId || opts.extensionId.trim() === '') throw new Error('extensionId is required.');

    const projectDir = path.resolve(opts.projectDir);
    if (!fs.existsSync(projectDir)) throw new Error(`Project directory does not exist: ${projectDir}`);

    // An extension install MUST target a real UE project — we enable plugins in
    // its `.uproject`. (install-plugin only WARNS, but it has no .uproject edit.)
    const uproject = readUProject(projectDir);
    if (uproject === null) {
      throw new Error(
        `No .uproject found in ${projectDir} — run install-extension inside an Unreal project directory.`,
      );
    }

    // 1. Resolve the descriptor (catalog, or synthesized from --source).
    const catalog = opts.catalog ?? EXTENSIONS_CATALOG;
    const sourceArg = (opts.source ?? '').trim();
    const versionOverride = (opts.version ?? '').trim();

    let descriptor: ExtensionDescriptor;
    const fromCatalog = findExtension(opts.extensionId, catalog);
    if (fromCatalog) {
      descriptor = applyVersionOverride(fromCatalog, versionOverride, warnings);
    } else if (sourceArg !== '') {
      descriptor = synthesizeDescriptorFromSource(opts.extensionId, sourceArg, versionOverride, warnings);
    } else {
      return {
        kind: 'failure',
        success: false,
        warnings,
        error: new Error(unknownExtensionMessage(opts.extensionId, catalog)),
      };
    }

    const pluginName = descriptor.pluginName;
    const installedPath = path.join(projectDir, 'Plugins', pluginName);
    const toVersion = hasVersion(descriptor) ? (descriptor.version as string) : null;

    // What is already on disk? (drives idempotency + add-vs-update.)
    const installedUplugin = findUPluginFile(installedPath);
    const fromVersion = installedUplugin
      ? readUPluginVersionName(safeReadJson(installedUplugin))
      : null;
    const installedPresent = installedUplugin !== null;

    // 2. Resolve the install source (the pluggable channel seam).
    const source = resolveInstallSource({
      source: sourceArg,
      repo: descriptor.repo,
      pluginName,
      version: toVersion,
    });

    // 3. Materialize the plugin files — only when needed (force, not present, or a
    //    version mismatch when a target version is known).
    const materializeNeeded =
      opts.force === true ||
      !installedPresent ||
      (toVersion !== null && fromVersion !== toVersion);

    if (materializeNeeded) {
      emitProgress(opts.onProgress, {
        phase: 'start',
        message: `Installing extension ${pluginName}${toVersion ? ` ${toVersion}` : ''} into ${installedPath}`,
      });
      await materialize(source, pluginName, installedPath, opts, warnings);
      // The folder name MUST match the descriptor basename or UE will not load it.
      const placed = findUPluginFile(installedPath);
      if (placed === null) {
        throw new Error(`No .uplugin found after materializing ${pluginName} into ${installedPath}.`);
      }
      const placedName = path.basename(placed, '.uplugin');
      if (placedName.toLowerCase() !== pluginName.toLowerCase()) {
        throw new Error(
          `Materialized descriptor "${placedName}.uplugin" does not match the expected plugin name "${pluginName}". ` +
            'Unreal requires the plugin folder and its .uplugin to share a name.',
        );
      }
    }

    // 4. Enable the extension + its gating engine plugins in the .uproject.
    const effectiveUplugin = findUPluginFile(installedPath);
    const declaredDeps = effectiveUplugin ? parseUPluginDependencies(safeReadJson(effectiveUplugin)) : [];
    const gatingPlugins = unique([...declaredDeps, ...descriptor.enginePlugins]);
    const toEnable = [pluginName, ...gatingPlugins];

    const beforeText = fs.readFileSync(uproject.uprojectPath, 'utf-8');
    const enable = enablePluginsInUProject(beforeText, toEnable);
    if (enable.changed) {
      fs.writeFileSync(uproject.uprojectPath, enable.text);
      emitProgress(opts.onProgress, {
        phase: 'file-written',
        message: `Enabled ${toEnable.join(', ')} in ${uproject.uprojectPath}`,
        filePath: uproject.uprojectPath,
      });
    }

    const changed = materializeNeeded || enable.changed;

    // minCoreVersion advisory (warn-only — never block an install on it).
    warnOnCoreVersionFloor(projectDir, descriptor, warnings);

    // 5. Compile. Default: rely on UE's compile-on-next-open. `--build` runs UBT now.
    let built = false;
    if (opts.build === true) {
      built = await runUbtBuild(uproject.uprojectPath, uproject.projectName, uproject.engineAssociation, opts, warnings);
    }
    const rebuildRequired = changed && !built;

    // !materializeNeeded with changed === true means the files were already at
    // the target version and only the .uproject enable entry was (re)written —
    // an "enabled" outcome, distinct from a real version "updated".
    const outcome: ExtensionInstallOutcome = !changed
      ? 'already-up-to-date'
      : !materializeNeeded
        ? 'enabled'
        : fromVersion === null
          ? 'added'
          : 'updated';
    const message = buildMessage(outcome, pluginName, fromVersion, toVersion, built, rebuildRequired, gatingPlugins);

    emitProgress(opts.onProgress, { phase: 'done', message });

    return {
      kind: 'success',
      success: true,
      outcome,
      changed,
      rebuildRequired,
      extensionId: opts.extensionId,
      pluginName,
      installedPath,
      sourceKind: source.kind,
      fromVersion,
      toVersion,
      enabledPlugins: enable.addedOrFlipped,
      uprojectPath: uproject.uprojectPath,
      built,
      message,
      warnings,
    };
  } catch (err: unknown) {
    return { kind: 'failure', success: false, warnings, error: asError(err) };
  }
}

// ---------------------------------------------------------------------------
// Descriptor resolution
// ---------------------------------------------------------------------------

/** Apply the `--version` override to a catalog descriptor (empty override is ignored). */
function applyVersionOverride(
  descriptor: ExtensionDescriptor,
  version: string,
  warnings: string[],
): ExtensionDescriptor {
  if (version === '') return descriptor;
  if (descriptor.version !== null && descriptor.version !== version) {
    warnings.push(
      `--version ${version} overrides the catalog pin ${descriptor.pluginName}@${descriptor.version} for this install.`,
    );
  }
  return { ...descriptor, version };
}

/**
 * Build a descriptor for an extension NOT in the catalog, from a local `--source`
 * dir's `.uplugin`. This is the unpublished / dev / CI install path: the plugin
 * name and gating deps come straight from the descriptor file on disk.
 */
function synthesizeDescriptorFromSource(
  extensionId: string,
  sourceArg: string,
  versionOverride: string,
  warnings: string[],
): ExtensionDescriptor {
  const pluginRoot = resolveLocalPluginRoot(sourceArg);
  const upluginFile = findUPluginFile(pluginRoot);
  if (upluginFile === null) {
    throw new Error(`--source ${sourceArg} does not contain a .uplugin (resolved to ${pluginRoot}).`);
  }
  const json = safeReadJson(upluginFile);
  const pluginName = path.basename(upluginFile, '.uplugin');
  const sourceVersion = versionOverride !== '' ? versionOverride : readUPluginVersionName(json);
  const enginePlugins = parseUPluginDependencies(json);
  warnings.push(
    `Extension "${extensionId}" is not in the catalog — installing from --source as plugin "${pluginName}".`,
  );
  return {
    extensionId,
    name: pluginName,
    description: `Local extension installed from ${sourceArg}.`,
    pluginName,
    repo: null,
    version: sourceVersion,
    minCoreVersion: null,
    enginePlugins,
    tools: [],
  };
}

// ---------------------------------------------------------------------------
// Materialization (local copy / GitHub-release download)
// ---------------------------------------------------------------------------

async function materialize(
  source: InstallSource,
  pluginName: string,
  installedPath: string,
  opts: InstallExtensionOptions,
  warnings: string[],
): Promise<void> {
  if (source.kind === 'local') {
    const pluginRoot = resolveLocalPluginRoot(source.dir);
    emitProgress(opts.onProgress, { phase: 'info', message: `Copying extension from ${pluginRoot}` });
    placePlugin(pluginRoot, installedPath);
    return;
  }

  // github-release
  emitProgress(opts.onProgress, {
    phase: 'info',
    message: `Downloading extension ${pluginName} ${source.version} from ${source.url}`,
  });
  const fetchImpl = opts.fetchImpl ?? fetch;
  const response = await fetchImpl(source.url, {
    signal: AbortSignal.timeout(opts.downloadTimeoutMs ?? 60_000),
  });
  if (!response.ok) {
    throw new Error(
      `Failed to download extension ${pluginName} ${source.version}: HTTP ${response.status} ${response.statusText} ` +
        `from ${source.url}. Verify the release exists, or use --source <dir> for an offline/dev install.`,
    );
  }
  const zipBytes = new Uint8Array(await response.arrayBuffer());
  const staging = fs.mkdtempSync(path.join(os.tmpdir(), `unreal-mcp-ext-extract-`));
  try {
    extractZip(zipBytes, staging, warnings);
    const upluginFile = findUPluginFile(staging);
    if (upluginFile === null) {
      throw new Error(
        `Downloaded zip did not contain a .uplugin — ${source.url} is not a valid extension plugin release.`,
      );
    }
    placePlugin(path.dirname(upluginFile), installedPath);
  } finally {
    fs.rmSync(staging, { recursive: true, force: true });
  }
}

/**
 * Resolve a `--source` argument to the plugin root: a dir that directly contains
 * a `.uplugin`, else the parent of the shallowest `.uplugin` beneath it. Throws
 * when neither exists.
 */
function resolveLocalPluginRoot(source: string): string {
  const resolved = path.resolve(source);
  if (!fs.existsSync(resolved)) throw new Error(`--source directory does not exist: ${resolved}`);
  const direct = hasUPluginDirectlyIn(resolved);
  if (direct) return resolved;
  const found = findUPluginFile(resolved);
  if (found) return path.dirname(found);
  throw new Error(`--source ${resolved} does not contain a .uplugin.`);
}

/** True when `dir` directly holds a `*.uplugin` (no descent). */
function hasUPluginDirectlyIn(dir: string): boolean {
  try {
    return fs.readdirSync(dir).some((n) => n.toLowerCase().endsWith('.uplugin'));
  } catch {
    return false;
  }
}

/**
 * Copy a plugin source root into `<project>/Plugins/<pluginName>/` via a
 * stage-then-swap (a sibling staging dir, swapped in only after a fully
 * successful filtered copy) so a mid-copy failure leaves any existing install
 * untouched. Build-cache / VCS subtrees are excluded (`EXCLUDED_DIRS`).
 */
function placePlugin(sourcePluginRoot: string, installedPath: string): void {
  const installReal = path.resolve(installedPath);
  const pluginsDir = path.dirname(installReal);
  fs.mkdirSync(pluginsDir, { recursive: true });
  const staging = path.join(pluginsDir, `.unreal-mcp-ext-staging-${process.pid}-${Date.now()}`);
  fs.rmSync(staging, { recursive: true, force: true });
  try {
    fs.cpSync(sourcePluginRoot, staging, {
      recursive: true,
      filter: (src) => copyFilter(src, sourcePluginRoot),
    });
    if (findUPluginFile(staging) === null) {
      throw new Error(`Copy from ${sourcePluginRoot} produced no .uplugin.`);
    }
    fs.rmSync(installReal, { recursive: true, force: true });
    fs.renameSync(staging, installReal);
  } finally {
    fs.rmSync(staging, { recursive: true, force: true });
  }
}

/** Keep everything except the excluded build-cache / VCS subtrees. */
function copyFilter(srcAbs: string, root: string): boolean {
  const rel = path.relative(root, srcAbs);
  if (rel === '' || rel === '.') return true;
  const top = rel.split(path.sep)[0];
  return !EXCLUDED_DIRS.has(top);
}

/** Extract a zip into `destDir` (fflate), guarding against zip-slip traversal. */
function extractZip(zipBytes: Uint8Array, destDir: string, warnings: string[]): void {
  const entries = unzipSync(zipBytes);
  const destReal = path.resolve(destDir);
  for (const [entryName, data] of Object.entries(entries)) {
    if (entryName.endsWith('/')) continue; // directory entry
    const target = path.resolve(destReal, entryName);
    if (target !== destReal && !target.startsWith(destReal + path.sep)) {
      warnings.push(`Skipped suspicious zip entry escaping the extract dir: ${entryName}`);
      continue;
    }
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, data);
  }
}

// ---------------------------------------------------------------------------
// minCoreVersion + UBT build
// ---------------------------------------------------------------------------

/** Warn (never fail) when the installed core Unreal-MCP plugin is below `minCoreVersion`. */
function warnOnCoreVersionFloor(
  projectDir: string,
  descriptor: ExtensionDescriptor,
  warnings: string[],
): void {
  const min = (descriptor.minCoreVersion ?? '').trim();
  if (min === '') return;
  const coreUplugin = path.join(projectDir, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin');
  if (!fs.existsSync(coreUplugin)) {
    warnings.push(
      `Extension requires core Unreal-MCP >= ${min}, but no UnrealMCP plugin was found at ${coreUplugin}. ` +
        'Install the core plugin (unreal-mcp-cli install-plugin) first.',
    );
    return;
  }
  const coreVersion = readUPluginVersionName(safeReadJson(coreUplugin));
  if (coreVersion && compareSemver(coreVersion, min) < 0) {
    warnings.push(
      `Extension requires core Unreal-MCP >= ${min}, but the installed core plugin is ${coreVersion}. Update it.`,
    );
  }
}

/** Run UBT to compile the project's editor target. Returns true on success; warns + returns false otherwise. */
async function runUbtBuild(
  uprojectPath: string,
  projectName: string,
  engineAssociation: string,
  opts: InstallExtensionOptions,
  warnings: string[],
): Promise<boolean> {
  const engineRoot = (opts.engineRoot ?? '').trim() || resolveEngineRoot(engineAssociation, opts);
  if (!engineRoot) {
    warnings.push(
      `--build requested but no engine root could be resolved for "${engineAssociation}". ` +
        'Skipping compile — the editor will recompile the extension on next open. Pass --engine-root <dir> to build now.',
    );
    return false;
  }
  const ubtPath = path.join(
    engineRoot,
    'Engine',
    'Binaries',
    'DotNET',
    'UnrealBuildTool',
    'UnrealBuildTool.exe',
  );
  const editorTarget = `${projectName}Editor`;
  const step: ExtensionBuildStep = {
    ubtPath,
    editorTarget,
    uprojectPath,
    args: [editorTarget, 'Win64', 'Development', `-project=${uprojectPath}`, '-WaitMutex'],
  };
  emitProgress(opts.onProgress, { phase: 'start', message: `Compiling ${editorTarget} via UBT` });
  const buildImpl = opts.buildImpl ?? defaultBuildImpl;
  try {
    await buildImpl(step);
    emitProgress(opts.onProgress, { phase: 'done', message: 'UBT build succeeded.' });
    return true;
  } catch (err: unknown) {
    warnings.push(
      `UBT build failed: ${asError(err).message}. The editor will recompile the extension on next open.`,
    );
    return false;
  }
}

/** Resolve an engine root from the project's EngineAssociation via the discovery chain. */
function resolveEngineRoot(engineAssociation: string, opts: InstallExtensionOptions): string | null {
  if (opts.resolveEngineRootImpl) return opts.resolveEngineRootImpl(engineAssociation);
  const r = discoverEngine({ engineAssociation, noCache: true });
  return r.kind === 'resolved' ? r.engineRoot : null;
}

/** Default UBT runner: spawn UnrealBuildTool, resolve on exit 0, reject otherwise. */
function defaultBuildImpl(step: ExtensionBuildStep): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    const child = spawn(step.ubtPath, step.args, { stdio: 'ignore' });
    child.on('error', reject);
    child.on('close', (code) => {
      if (code === 0) resolve();
      else reject(new Error(`UnrealBuildTool exited with code ${code}`));
    });
  });
}

// ---------------------------------------------------------------------------
// Small pure helpers
// ---------------------------------------------------------------------------

/** Shallowest `*.uplugin` under `dir` (the plugin descriptor), or `null`. */
function findUPluginFile(dir: string): string | null {
  let best: string | null = null;
  let bestDepth = Number.MAX_SAFE_INTEGER;
  const walk = (d: string, depth: number): void => {
    if (depth >= bestDepth) return;
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(d, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(d, entry.name);
      if (entry.isDirectory()) {
        if (!EXCLUDED_DIRS.has(entry.name)) walk(full, depth + 1);
      } else if (entry.isFile() && entry.name.toLowerCase().endsWith('.uplugin') && depth < bestDepth) {
        best = full;
        bestDepth = depth;
      }
    }
  };
  walk(dir, 0);
  return best;
}

/** Read + JSON.parse a file; throws a clear error on malformed JSON. */
function safeReadJson(file: string): unknown {
  const text = fs.readFileSync(file, 'utf-8');
  try {
    return JSON.parse(text);
  } catch {
    throw new Error(`Malformed JSON in ${file}.`);
  }
}

/** De-dupe strings case-insensitively, preserving first-seen order. */
function unique(values: readonly string[]): string[] {
  const seen = new Set<string>();
  const out: string[] = [];
  for (const v of values) {
    const key = v.toLowerCase();
    if (!seen.has(key)) {
      seen.add(key);
      out.push(v);
    }
  }
  return out;
}

/** Numeric dotted-version compare (`1.2.0` vs `1.10.0`). Returns -1 / 0 / 1. Non-numeric parts compare as 0. */
function compareSemver(a: string, b: string): number {
  const pa = a.split('.').map((x) => parseInt(x, 10) || 0);
  const pb = b.split('.').map((x) => parseInt(x, 10) || 0);
  const len = Math.max(pa.length, pb.length);
  for (let i = 0; i < len; i++) {
    const da = pa[i] ?? 0;
    const db = pb[i] ?? 0;
    if (da !== db) return da < db ? -1 : 1;
  }
  return 0;
}

/** A short, printable status line for the result. */
function buildMessage(
  outcome: ExtensionInstallOutcome,
  pluginName: string,
  fromVersion: string | null,
  toVersion: string | null,
  built: boolean,
  rebuildRequired: boolean,
  gatingPlugins: string[],
): string {
  const gating = gatingPlugins.length > 0 ? ` (also enabled: ${gatingPlugins.join(', ')})` : '';
  const compile = built
    ? ' Compiled via UBT.'
    : rebuildRequired
      ? ' Rebuild required — open the project (or pass --build) to compile.'
      : '';
  switch (outcome) {
    case 'added':
      return `Installed extension ${pluginName}${toVersion ? ` ${toVersion}` : ''} and enabled it${gating}.${compile}`;
    case 'updated':
      return `Updated extension ${pluginName}${fromVersion ? ` ${fromVersion}` : ''} → ${toVersion ?? '(unpinned)'}${gating}.${compile}`;
    case 'enabled':
      return `Enabled existing extension ${pluginName}${fromVersion ? ` ${fromVersion}` : ''} in the project${gating}.${compile}`;
    case 'already-up-to-date':
      return `Extension ${pluginName}${toVersion ? ` ${toVersion}` : ''} is already installed and enabled.`;
  }
}
