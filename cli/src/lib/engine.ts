// Library-facing engine discovery: read the launcher manifest and resolve
// a project's engine. Thin orchestration over utils/launcher + utils/engine
// + utils/project so consumers don't reach into `utils/`.

import { platform } from 'os';
import {
  getDefaultLauncherManifestPath,
  readLauncherManifest,
  type EngineInstallation,
} from '../utils/launcher.js';
import { resolveEngine, type ResolveEngineResult } from '../utils/engine.js';
import { readUProject } from '../utils/project.js';

/** Read the installed engines from the (platform-default) launcher manifest. */
export function listInstalledEngines(manifestPath?: string, os?: NodeJS.Platform): {
  manifestPath: string | null;
  engines: EngineInstallation[];
} {
  const targetOs = os ?? (platform() as NodeJS.Platform);
  const resolvedPath = manifestPath ?? getDefaultLauncherManifestPath(targetOs);
  const engines = resolvedPath ? readLauncherManifest(resolvedPath) : [];
  return { manifestPath: resolvedPath, engines };
}

/**
 * Resolve the engine for a project on disk: read its `.uproject`
 * `EngineAssociation`, then match it against the installed engines.
 * Returns the resolution result plus the project info that fed it.
 */
export function resolveEngineForProject(opts: {
  projectDir: string;
  engineRootOverride?: string;
  manifestPath?: string;
  os?: NodeJS.Platform;
}): {
  uproject: ReturnType<typeof readUProject>;
  resolution: ResolveEngineResult;
} {
  const os = opts.os ?? (platform() as NodeJS.Platform);
  const uproject = readUProject(opts.projectDir);
  const { engines } = listInstalledEngines(opts.manifestPath, os);
  const resolution = resolveEngine({
    engineAssociation: uproject?.engineAssociation ?? '',
    engines,
    engineRootOverride: opts.engineRootOverride,
    os,
  });
  return { uproject, resolution };
}
