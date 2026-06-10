import { Command } from 'commander';
import { setupMcp, listAgentIds } from '../lib/setup-mcp.js';
import type { McpTransport } from '../lib/types.js';
import * as ui from '../utils/ui.js';

export const setupMcpCommand = new Command('setup-mcp')
  .description(`Write an MCP client config snippet for an agent (${listAgentIds().join(', ')})`)
  .argument('<agent>', `Agent id: ${listAgentIds().join(' | ')}`)
  .option('-p, --path <dir>', 'Unreal project directory (defaults to cwd)')
  .option('--transport <t>', 'Transport: http | stdio (default http)')
  .option('--url <url>', 'Explicit MCP server URL override')
  .option('--token <token>', 'Bearer token override')
  .option('--dry-run', 'Print the snippet instead of writing it')
  .action(async (agent: string, opts) => {
    const result = await setupMcp({
      agentId: agent,
      projectDir: opts.path,
      transport: opts.transport as McpTransport | undefined,
      url: opts.url,
      token: opts.token,
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
