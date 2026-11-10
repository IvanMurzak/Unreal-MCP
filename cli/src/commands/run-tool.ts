import { runTool } from '../lib/run-tool.js';
import { buildRunToolCommand } from './run-tool-builder.js';

export const runToolCommand = buildRunToolCommand({
  name: 'run-tool',
  description: 'Call one of the plugin\'s AI tools (list them with `unreal-mcp-cli tools`); starts a headless editor when none answers',
  errorNoun: 'tool',
  invoke: runTool,
});
