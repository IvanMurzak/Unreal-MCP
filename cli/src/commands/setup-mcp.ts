import { Command } from 'commander';
import { setupMcp } from '../lib/setup-mcp.js';
import { getAgentById, getAgentIds, listAgentTable } from '../utils/agents.js';
import type { McpTransport } from '../lib/types.js';
import * as ui from '../utils/ui.js';

export const setupMcpCommand = new Command('setup-mcp')
  .description('Write an MCP client config snippet for an AI agent (use --list to see all)')
  .argument('[agent]', 'Agent id (use --list to see all supported agents)')
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('--transport <t>', 'Transport: http | stdio (default http)')
  .option('--url <url>', 'Explicit MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .option('--no-pin', 'Write an unpinned <base>/mcp URL instead of the default pinned <base>/mcp/p/<pin>')
  .option('--dry-run', 'Print the snippet instead of writing it')
  .option('--list', 'List all supported agent ids and their config paths')
  .action(async (agent: string | undefined, opts) => {
    if (opts.list) {
      listAgentTable('Available AI Agents', 'Config Path', (a) => a.configPathDisplay);
      return;
    }
    if (!agent) {
      ui.error('Missing required argument: agent');
      ui.info(`Available agents: ${getAgentIds().join(', ')} (or use --list)`);
      process.exitCode = 1;
      return;
    }
    if (!getAgentById(agent)) {
      ui.error(`Unknown agent "${agent}".`);
      ui.info(`Available agents: ${getAgentIds().join(', ')} (or use --list)`);
      process.exitCode = 1;
      return;
    }
    const result = await setupMcp({
      agentId: agent,
      projectDir: opts.path,
      transport: opts.transport as McpTransport | undefined,
      url: opts.url,
      token: opts.token,
      // Commander maps `--no-pin` to `pin === false`; the default (pinned) is `pin !== false`.
      noPin: opts.pin === false,
      dryRun: opts.dryRun,
    });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    if (opts.dryRun) {
      ui.info(result.snippet);
    } else {
      ui.success(`Wrote ${result.agentId} MCP config (${result.transport}) to ${result.configPath}`);
    }
    for (const step of result.nextSteps) ui.info(`→ ${step}`);
  });
