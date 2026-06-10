import { Command } from 'commander';
import { configure } from '../lib/configure.js';
import type { AuthOption, ConnectionMode, McpTransport } from '../lib/types.js';
import * as ui from '../utils/ui.js';

export const configureCommand = new Command('configure')
  .description('Write UNREAL_MCP_* values into <project>/.env and gitignore .env (§8)')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)', process.cwd())
  .option('--mode <mode>', 'Connection mode: Cloud | Custom')
  .option('--host <url>', 'Server host (UNREAL_MCP_HOST)')
  .option('--cloud-url <url>', 'Cloud URL (UNREAL_MCP_CLOUD_URL)')
  .option('--token <token>', 'Auth token (UNREAL_MCP_TOKEN)')
  .option('--auth <option>', 'Auth option: none | required')
  .option('--keep-connected', 'Set UNREAL_MCP_KEEP_CONNECTED=true')
  .option('--tools <list>', 'Enabled-tools override (UNREAL_MCP_TOOLS)')
  .option('--start-server', 'Set UNREAL_MCP_START_SERVER=true')
  .option('--transport <t>', 'Transport: stdio | http')
  .option('--log-level <level>', 'Log level (UNREAL_MCP_LOG_LEVEL)')
  .option('--no-gitignore', 'Do not touch .gitignore')
  .action(async (opts) => {
    const result = await configure({
      projectDir: opts.path,
      connectionMode: opts.mode as ConnectionMode | undefined,
      host: opts.host,
      cloudUrl: opts.cloudUrl,
      token: opts.token,
      authOption: opts.auth as AuthOption | undefined,
      keepConnected: opts.keepConnected,
      tools: opts.tools,
      startServer: opts.startServer,
      transport: opts.transport as McpTransport | undefined,
      logLevel: opts.logLevel,
      ensureGitignore: opts.gitignore !== false,
    });
    if (result.kind === 'failure') {
      ui.printWarnings(result.warnings);
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    ui.printWarnings(result.warnings);
    ui.success(`Wrote ${result.keysWritten.length} key(s) to ${result.envPath}`);
    if (result.gitignoreAction && result.gitignoreAction !== 'skipped') {
      ui.info(`.gitignore: ${result.gitignoreAction} (${result.gitignorePath})`);
    }
  });
