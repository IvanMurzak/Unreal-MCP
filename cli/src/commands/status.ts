import { Command } from 'commander';
import { getStatus } from '../lib/status.js';

// Scaffold stub. The real command (port of godot-cli's `status`) will detect a
// running Unreal Editor for a project, probe the MCP server URL, and report the
// bridge/sidecar state. For now it proves the command plumbing end-to-end.
export const statusCommand = new Command('status')
  .description('Show unreal-cli status (scaffold stub: prints name and version)')
  .action(() => {
    const status = getStatus();
    console.log(`${status.name} v${status.version}`);
    console.log(`status: ${status.stage}`);
  });
