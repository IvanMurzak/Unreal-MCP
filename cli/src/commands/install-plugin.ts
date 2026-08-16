import { Command } from 'commander';
import { installPlugin } from '../lib/install-plugin.js';
import * as ui from '../utils/ui.js';

/**
 * Read the enrollment code from stdin (for `--enroll-stdin`) so it never lands
 * in argv / shell history (mcp-authorize design 06, D13). Reads to EOF, trims.
 */
function readStdin(): Promise<string> {
  return new Promise((resolve, reject) => {
    let data = '';
    process.stdin.setEncoding('utf-8');
    process.stdin.on('data', (chunk) => {
      data += chunk;
    });
    process.stdin.on('end', () => resolve(data.trim()));
    process.stdin.on('error', reject);
    process.stdin.resume();
  });
}

export const installPluginCommand = new Command('install-plugin')
  .description('Install the UnrealMCP plugin into <project>/Plugins (copy or --junction). Defaults to the local repo source when present, otherwise downloads the matching GitHub release. Optionally bundles the local MCP server (--with-server) and/or redeems an agent enrollment code (--enroll).')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--plugin-source <dir>', 'UnrealMCP plugin source dir (offline / CI / dev override)')
  .option('--version <v>', 'Plugin-source release version to download (defaults to this CLI version)')
  .option('--junction', 'Create a dev junction instead of copying (Windows)')
  .option('--with-server', 'Also download the RID-matched gamedev-mcp-server binary (checksum-verified) into the managed dir')
  .option('--server-version <v>', 'Server version to download with --with-server (defaults to the pinned SERVER_VERSION)')
  .option('--server-source <path-or-url>', 'Offline/CI escape hatch for --with-server: a local zip/dir/binary or a URL')
  .option('--enroll <code>', 'Redeem a D13 agent enrollment code → shared machine store + project marker + pin (no browser)')
  .option('--enroll-stdin', 'Read the enrollment code from stdin instead of argv (keeps it out of shell history)')
  .option('--base-url <url>', 'Auth base URL for --enroll (defaults to https://ai-game.dev)')
  .option('-y, --yes', 'Confirm switching this machine to a different ai-game.dev account when the --enroll credential is a different user (otherwise a mismatch aborts unchanged)')
  .action(
    async (
      pathArg: string | undefined,
      opts: {
        pluginSource?: string;
        version?: string;
        junction?: boolean;
        withServer?: boolean;
        serverVersion?: string;
        serverSource?: string;
        enroll?: string;
        enrollStdin?: boolean;
        baseUrl?: string;
        yes?: boolean;
      },
    ) => {
      const projectDir = pathArg ?? process.cwd();

      if (opts.enroll !== undefined && opts.enrollStdin) {
        ui.error('Use either --enroll <code> or --enroll-stdin, not both.');
        process.exitCode = 1;
        return;
      }

      let enrollCode: string | undefined;
      if (opts.enrollStdin) {
        enrollCode = await readStdin();
        if (enrollCode.length === 0) {
          ui.error('--enroll-stdin was set but no enrollment code was received on stdin.');
          process.exitCode = 1;
          return;
        }
      } else if (opts.enroll !== undefined) {
        enrollCode = opts.enroll;
      }

      const spinner = ui.startSpinner('Installing UnrealMCP plugin...');
      const result = await installPlugin({
        projectDir,
        pluginSourceDir: opts.pluginSource,
        version: opts.version,
        junction: opts.junction,
        withServer: opts.withServer,
        serverVersion: opts.serverVersion,
        serverSource: opts.serverSource,
        enrollCode,
        baseUrl: opts.baseUrl,
        assumeYes: opts.yes,
        onProgress: (event) => {
          spinner.text = event.message;
        },
      });
      if (result.kind === 'failure') {
        spinner.error(result.error.message);
        ui.printWarnings(result.warnings);
        process.exitCode = 1;
        return;
      }
      spinner.success(`Installed plugin (${result.mode}) at ${result.installedPath}`);
      if (result.serverPath) {
        ui.info(`Server binary ready: ${result.serverPath}`);
      }
      if (result.enrolled) {
        ui.info(`Enrolled — credential saved to the machine store; server target ${result.serverTarget} (pin ${result.pin}).`);
        if (result.pinnedConfigFiles && result.pinnedConfigFiles.length > 0) {
          ui.info(`Pinned existing agent config(s): ${result.pinnedConfigFiles.join(', ')}`);
        }
      }
      ui.printWarnings(result.warnings);
    },
  );
