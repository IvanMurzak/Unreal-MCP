import { Command } from 'commander';
import { configure } from '../lib/configure.js';
import { configureAgent } from '../lib/configure-agent.js';
import type { AuthOption, ConnectionMode, McpTransport } from '../lib/types.js';
import * as ui from '../utils/ui.js';

export const configureCommand = new Command('configure')
  .description('Configure MCP for a project. With --agent <id>, proxies to the managed gamedev-mcp-server configurator to write a pinned client config; otherwise writes UNREAL_MCP_* values into <project>/.env (§8).')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)', process.cwd())
  .option('--agent <id>', 'Proxy to `gamedev-mcp-server configure --agent <id>` to write a pinned MCP client config')
  .option('--url <url>', 'Server URL for the --agent proxy (omit for the derived local host)')
  .option('--server-version <v>', 'Server version to acquire for the --agent proxy (defaults to the pinned SERVER_VERSION)')
  .option('--server-source <path-or-url>', 'Offline/CI escape hatch for the --agent proxy server binary (local zip/dir/binary or URL)')
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
    // --agent routes to the shared C# configurator registry inside the managed
    // gamedev-mcp-server binary (mcp-authorize design 06/09, D12) — writing a
    // project-scoped, pinned client config. This is a distinct surface from the
    // legacy `.env` writer below.
    if (opts.agent !== undefined) {
      const spinner = ui.startSpinner(`Configuring ${opts.agent} via gamedev-mcp-server...`);
      const result = await configureAgent({
        agentId: opts.agent,
        projectDir: opts.path,
        url: opts.url,
        transport: opts.transport as McpTransport | undefined,
        serverVersion: opts.serverVersion,
        serverSource: opts.serverSource,
        onProgress: (e) => {
          if (e.phase === 'start' || e.phase === 'info') spinner.text = e.message;
        },
      });
      if (result.kind === 'failure') {
        spinner.error(result.error.message);
        ui.printWarnings(result.warnings);
        process.exitCode = 1;
        return;
      }
      spinner.success(`Configured ${result.agentId} via ${result.serverPath}.`);
      ui.printWarnings(result.warnings);
      return;
    }

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
