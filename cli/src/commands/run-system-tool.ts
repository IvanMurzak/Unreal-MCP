import { runSystemTool } from '../lib/run-tool.js';
import { buildRunToolCommand } from './run-tool-builder.js';

export const runSystemToolCommand = buildRunToolCommand({
  name: 'run-system-tool',
  description: 'Invoke a system tool via the project\'s local MCP server (HTTP)',
  errorNoun: 'system tool',
  invoke: runSystemTool,
});
