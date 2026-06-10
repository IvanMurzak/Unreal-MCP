import { Command } from 'commander';
import { getStatus } from '../lib/status.js';
import * as ui from '../utils/ui.js';

export const statusCommand = new Command('status')
  .description('Report package, project, plugin, and live connection status')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('--url <url>', 'Explicit MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .option('--no-probe', 'Skip the live HTTP reachability probe')
  .action(async (opts: { path?: string; url?: string; token?: string; probe?: boolean }) => {
    const report = await getStatus({
      projectDir: opts.path,
      url: opts.url,
      token: opts.token,
      noProbe: opts.probe === false,
    });

    ui.info(`${report.name} v${report.version}`);
    if (report.project) {
      ui.heading('Project:');
      ui.label('name', report.project.projectName);
      ui.label('dir', report.project.projectDir);
      ui.label('engine', report.project.engineAssociation);
      ui.label('plugin', report.project.pluginInstalled ? 'installed' : 'not installed');
    }
    if (report.connection.url) {
      ui.heading('Connection:');
      ui.label('url', report.connection.url);
      ui.label('source', report.connection.source);
      ui.label('token', report.connection.hasToken ? 'present' : 'none');
    } else if (report.connection.source === 'unresolved') {
      // A connection could not be resolved at all — surface why instead of
      // printing nothing (the reason is carried on probeReason).
      ui.heading('Connection:');
      ui.label('status', `could not resolve${report.probeReason ? ` (${report.probeReason})` : ''}`);
    }
    if (report.reachable !== undefined) {
      ui.heading('MCP server:');
      ui.label('reachable', report.reachable ? 'yes' : `no (${report.probeReason})`);
    }
  });
