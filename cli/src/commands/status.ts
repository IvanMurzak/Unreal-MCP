import { Command } from 'commander';
import { getStatus } from '../lib/status.js';
import { awaitingInputMessage, describeDialog } from '../lib/editor-dialogs.js';
import { isUnrealProjectDir } from '../utils/project.js';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, FAILURE, formatOption } from '../utils/output.js';
import * as ui from '../utils/ui.js';

export const statusCommand = new Command('status')
  .description('Report package, project, plugin, running editor and live connection status (exit 18 when a modal dialog blocks the editor)')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd when it holds a .uproject)')
  .option('--url <url>', 'Explicit MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .option('--no-probe', 'Skip the live HTTP reachability probe')
  .addOption(formatOption())
  .action(async (opts: { path?: string; url?: string; token?: string; probe?: boolean; format: string }) => {
    const out = new CommandOutput('status', opts.format);
    const report = await getStatus({
      // Inside a project folder, report that project (as every other command does) without needing --path.
      projectDir: opts.path ?? (isUnrealProjectDir(process.cwd()) ? process.cwd() : undefined),
      url: opts.url,
      token: opts.token,
      noProbe: opts.probe === false,
    });
    const { kind: _kind, success: _success, ...data } = report;
    const modal = report.editor?.modalDialog;
    const dialogs = report.editor?.dialogs ?? [];

    if (!out.machine) {
      ui.info(`${report.name} v${report.version}`);
      if (report.project) {
        ui.heading('Project:');
        ui.label('name', report.project.projectName);
        ui.label('dir', report.project.projectDir);
        ui.label('engine', report.project.engineAssociation);
        ui.label('plugin', report.project.pluginInstalled ? 'installed' : 'not installed');
      }
      if (report.editor) {
        ui.heading('Editor:');
        ui.label('running', report.editor.running ? `yes (pid ${report.editor.pids.join(', ')})` : 'no');
        if (dialogs.length > 0) for (const d of dialogs) ui.label('blocked by', `modal dialog ${describeDialog(d)} (id ${d.id})`);
        else if (modal !== undefined) ui.label('blocked by', `modal dialog "${modal}"`);
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
    }

    if (modal !== undefined) {
      out.fail(
        FAILURE.awaitingInput,
        dialogs.length > 0 ? awaitingInputMessage(dialogs)
          : `The Unreal Editor is blocked by the modal dialog "${modal}": answer or close it in the editor, or restart the editor.`,
        data,
      );
      return;
    }
    out.finish(ExitCode.Success, data);
  });
