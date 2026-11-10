// `tools` — the plugin's AI tools (every tool, including the ones hidden from MCP tools/list), read from a running
// editor through `unreal-tool-list` and cached in `<project>/.ai-game-dev/tools.json`, so an agent can learn the tool
// set while the editor is closed. Library-safe: never prints, never exits.

import * as fs from 'fs';
import * as path from 'path';
import { ensureAiGameDevGitignore } from '@baizor/gamedev-cli-core';
import { runToolWithSession, type RunToolSessionOptions, type RunToolSessionResult } from './run-tool-session.js';
import { runTool } from './run-tool.js';
import { readPluginVersion } from './update.js';
import { readUProject } from '../utils/project.js';

export const TOOL_LIST_TOOL = 'unreal-tool-list';
export const TOOLS_CACHE_FILE = 'tools.json';
const CACHE_SCHEMA = 1;

export interface ToolInfo {
  name: string;
  title: string;
  /** `false`: left out of MCP tools/list, but still runs when called by name. */
  visible: boolean;
  extensionId?: string;
  description?: string;
  /** `name: type (required), ...` */
  inputs?: string;
}

export interface ToolsCache {
  schema: number;
  /** Plugin version + installed plugins: a cache made for another set is stale. */
  key: string;
  savedAt: string;
  tools: ToolInfo[];
  /** Extensions this editor does not have, with how to get them. */
  availableExtensions?: Array<{ id: string; state?: string; line: string }>;
}

export type ToolsResult =
  | { kind: 'success'; source: 'cache' | 'editor'; cachePath?: string; cache: ToolsCache; session?: RunToolSessionResult }
  | { kind: 'failure'; message: string; session: RunToolSessionResult; cachePath?: string };

export interface ListToolsOptions extends Omit<RunToolSessionOptions, 'invoke' | 'toolName' | 'input'> {
  /** Ask the editor even when the cache is current. */
  refresh?: boolean;
  invoke?: RunToolSessionOptions['invoke'];
  /** Test seam: replaces the session-aware call. */
  run?: typeof runToolWithSession;
}

export function toolsCachePath(projectDir: string): string {
  return path.join(projectDir, '.ai-game-dev', TOOLS_CACHE_FILE);
}

/** What the tool set depends on: the core plugin's version and the plugins (extensions) installed next to it. */
export function toolsCacheKey(projectDir: string): string {
  const plugins = path.join(projectDir, 'Plugins');
  let names: string[] = [];
  try {
    names = fs.readdirSync(plugins).sort();
  } catch {
    /* no Plugins folder */
  }
  const versions = names.map((name) => {
    const uplugin = path.join(plugins, name, `${name}.uplugin`);
    return `${name}@${readPluginVersion(uplugin) ?? '?'}`;
  });
  return versions.join(',');
}

export function readToolsCache(projectDir: string): ToolsCache | undefined {
  try {
    const cache = JSON.parse(fs.readFileSync(toolsCachePath(projectDir), 'utf-8')) as ToolsCache;
    return cache.schema === CACHE_SCHEMA && Array.isArray(cache.tools) ? cache : undefined;
  } catch {
    return undefined;
  }
}

/** The tool rows of an `unreal-tool-list` response (`structured.tools`). */
export function parseToolList(data: unknown): Pick<ToolsCache, 'tools' | 'availableExtensions'> | undefined {
  const structured = (data as { structured?: { tools?: unknown; availableExtensions?: unknown } } | null)?.structured;
  if (!structured || !Array.isArray(structured.tools)) return undefined;
  const tools = (structured.tools as Array<Record<string, unknown>>)
    .filter((t) => typeof t.name === 'string')
    .map((t): ToolInfo => ({
      name: t.name as string,
      title: typeof t.title === 'string' ? t.title : '',
      visible: t.visible !== false,
      ...(typeof t.extensionId === 'string' && t.extensionId !== 'core' ? { extensionId: t.extensionId } : {}),
      ...(typeof t.description === 'string' ? { description: t.description } : {}),
      ...(typeof t.inputs === 'string' ? { inputs: t.inputs } : {}),
    }));
  const extensions = Array.isArray(structured.availableExtensions)
    ? (structured.availableExtensions as Array<Record<string, unknown>>)
      .filter((e) => typeof e.id === 'string' && typeof e.line === 'string')
      .map((e) => ({ id: e.id as string, line: e.line as string, ...(typeof e.state === 'string' ? { state: e.state } : {}) }))
    : undefined;
  return { tools, ...(extensions && extensions.length > 0 ? { availableExtensions: extensions } : {}) };
}

/**
 * The project's tools: from the cache when it was made for the installed plugins (no editor needed), else from the
 * editor (started headless when none runs, as `run-tool` does), refreshing the cache.
 */
export async function listTools(options: ListToolsOptions): Promise<ToolsResult> {
  const project = options.projectDir ? readUProject(path.resolve(options.projectDir)) : null;
  const projectDir = project?.projectDir;
  const key = projectDir ? toolsCacheKey(projectDir) : undefined;
  if (projectDir && !options.refresh && options.url === undefined) {
    const cache = readToolsCache(projectDir);
    if (cache && cache.key === key) return { kind: 'success', source: 'cache', cachePath: toolsCachePath(projectDir), cache };
  }

  const { refresh: _refresh, run, invoke, ...rest } = options;
  const session = await (run ?? runToolWithSession)({
    ...rest,
    invoke: invoke ?? runTool,
    toolName: TOOL_LIST_TOOL,
    input: { includeDescription: true, includeInputs: true },
  });
  const parsed = session.result?.kind === 'success' ? parseToolList(session.result.data) : undefined;
  if (!parsed) return { kind: 'failure', message: 'The editor did not return its tool list.', session };

  const cache: ToolsCache = { schema: CACHE_SCHEMA, key: key ?? '', savedAt: new Date().toISOString(), ...parsed };
  let cachePath: string | undefined;
  if (projectDir) {
    cachePath = toolsCachePath(projectDir);
    try {
      fs.mkdirSync(path.dirname(cachePath), { recursive: true });
      fs.writeFileSync(cachePath, `${JSON.stringify(cache, null, 2)}\n`, 'utf-8');
      ensureAiGameDevGitignore(projectDir, [TOOLS_CACHE_FILE]);
    } catch {
      cachePath = undefined; // a read-only project still gets its answer
    }
  }
  return { kind: 'success', source: 'editor', cachePath, cache, session };
}

/** Tools whose name, title or description matches `filter` (case-insensitive regex; a bad regex is a substring). */
export function filterTools(tools: ToolInfo[], filter: string | undefined): ToolInfo[] {
  if (!filter) return tools;
  // A tool's exact name asks for that tool (not the others whose descriptions mention it).
  const exact = tools.filter((t) => t.name.toLowerCase() === filter.trim().toLowerCase());
  if (exact.length > 0) return exact;
  let test: (text: string) => boolean;
  try {
    const re = new RegExp(filter, 'i');
    test = (text) => re.test(text);
  } catch {
    const needle = filter.toLowerCase();
    test = (text) => text.toLowerCase().includes(needle);
  }
  return tools.filter((t) => test(`${t.name}\n${t.title}\n${t.description ?? ''}`));
}
