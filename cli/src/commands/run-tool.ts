import { runTool } from '../lib/run-tool.js';
import { buildRunToolCommand } from './run-tool-builder.js';

export const runToolCommand = buildRunToolCommand({
  name: 'run-tool',
  description: 'Invoke an MCP tool via the project\'s local MCP server (HTTP)',
  errorNoun: 'tool',
  invoke: runTool,
});
