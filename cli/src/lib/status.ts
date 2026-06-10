// Side-effect-free `status` implementation: report package identity,
// resolved project + plugin state, the resolved connection, and an
// optional live reachability probe.

import * as fs from 'fs';
import * as path from 'path';
import { PACKAGE_NAME, PACKAGE_VERSION } from '../version.js';
import { readUProject } from '../utils/project.js';
import { resolveConnection } from '../utils/config.js';
import { probePing } from '../utils/probe.js';
import type { StatusOptions, StatusReport } from './types.js';

/**
 * Gather status for an optional project. With no `projectDir`/`url`, returns
 * just package identity. With a project, resolves project + connection info
 * and (unless `noProbe`) probes the local MCP server. Never throws.
 */
export async function getStatus(opts: StatusOptions = {}): Promise<StatusReport> {
  const report: StatusReport = {
    kind: 'success',
    success: true,
    name: PACKAGE_NAME,
    version: PACKAGE_VERSION,
    connection: { url: '', source: 'none', hasToken: false },
  };

  let projectDir: string | undefined;
  if (opts.projectDir) {
    projectDir = path.resolve(opts.projectDir);
    const uproject = readUProject(projectDir);
    if (uproject) {
      // Key on the `.uplugin` descriptor, not the directory: a bare
      // `Plugins/UnrealMCP/` folder without the descriptor is not a usable
      // install (the OR'd dir-exists check was a strict superset and so
      // degenerated to dir-exists).
      const pluginInstalled = fs.existsSync(
        path.join(projectDir, 'Plugins', 'UnrealMCP', 'UnrealMCP.uplugin'),
      );
      report.project = {
        projectDir: uproject.projectDir,
        projectName: uproject.projectName,
        engineAssociation: uproject.engineAssociation || '(none)',
        pluginInstalled,
      };
    }
  }

  // Resolve a connection when we have either a project or an explicit URL.
  if (projectDir || opts.url) {
    try {
      const conn = resolveConnection({ projectDir, url: opts.url, token: opts.token });
      report.connection = { url: conn.url, source: conn.source, hasToken: conn.token !== undefined };

      if (!opts.noProbe) {
        const probe = await probePing(conn.url, {
          token: conn.token,
          timeoutMs: opts.probeTimeoutMs,
          fetchImpl: opts.fetchImpl,
        });
        report.reachable = probe.ok;
        report.probeReason = probe.ok ? undefined : probe.reason;
      }
    } catch (err) {
      report.connection = { url: '', source: 'unresolved', hasToken: false };
      report.probeReason = err instanceof Error ? err.message : String(err);
    }
  }

  return report;
}
