// AI-agent MCP-client configurator registry for `unreal-mcp-cli`.
//
// Ported from the authoritative Unity registry
// (`Unity-MCP/cli/src/utils/agents.ts`) and brought to parity with the Godot
// registry (`Godot-MCP/cli/src/utils/agents.ts`) so the three CLIs stay
// consistent. Each agent knows where its MCP-client config file lives, which
// serialization format it uses (`json` | `toml`), the top-level body key its
// servers nest under, and how to render both an HTTP-transport and a
// stdio-transport server entry (plus the stale keys to strip when switching
// transports on a re-run).
//
// Unreal specifics vs the siblings:
// - The server entry key is `unreal-mcp` (Unity/Godot use `ai-game-developer`)
//   — kept so existing users' configs stay stable (no behaviour change).
// - stdio launches the local `gamedev-mcp-server` binary that lands under the
//   project's `Intermediate/UnrealMCP/server/<rid>/` (§6 — resolved by
//   `download-server.ts`), NOT Unity's `Library/mcp-server/<platform>/` path.
// - The stdio arg vector mirrors the existing Unreal contract
//   (`port`/`client-transport`/`authorization`/`token`) and deliberately omits
//   Unity's `plugin-timeout` to preserve the current behaviour for the shared
//   `gamedev-mcp-server` binary.
// - Roster = the 14 shared ids brought to naming parity with unity-mcp-cli:
//   the VS Code id is `vscode-copilot` (renamed from `vscode`), includes
//   `custom`, omits the Unity-only `unity-ai`.

import chalk from 'chalk';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

// ---------------------------------------------------------------------------
// Agent Definition
// ---------------------------------------------------------------------------

export interface AgentDefinition {
  id: string;
  name: string;
  /** Per-agent project-relative skills directory, or `null` if the agent has none. */
  skillsPath: string | null;
  configPathDisplay: string;
  /** Config-file serialization format. `toml` is the Codex branch. */
  configFormat: 'json' | 'toml';
  bodyPath: string;
  /** Resolve the absolute config-file path for a given project root. */
  getConfigPath(projectPath: string): string;
  /** Build the stdio server entry launching the local `gamedev-mcp-server`. */
  getStdioProps(
    serverPath: string,
    port: number,
    auth: string,
    token: string,
  ): Record<string, unknown>;
  /** Build the HTTP server entry pointing at the resolved `<host>/mcp` URL. */
  getHttpProps(
    url: string,
    token: string,
    authRequired: boolean,
  ): Record<string, unknown>;
  /** Keys to delete from a pre-existing entry before merging stdio props. */
  stdioRemoveKeys: string[];
  /** Keys to delete from a pre-existing entry before merging http props. */
  httpRemoveKeys: string[];
}

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

function appData(): string {
  return process.env['APPDATA'] ?? path.join(os.homedir(), 'AppData', 'Roaming');
}

function home(): string {
  return os.homedir();
}

function isWindows(): boolean {
  return process.platform === 'win32';
}

function isMac(): boolean {
  return process.platform === 'darwin';
}

// ---------------------------------------------------------------------------
// Shared stdio / http arg builders
// ---------------------------------------------------------------------------

/**
 * The stdio arg vector passed to the local `gamedev-mcp-server` binary. Matches
 * the existing Unreal contract (no `plugin-timeout` — see the file header).
 */
function stdioArgs(port: number, auth: string, token: string): string[] {
  return [
    `port=${port}`,
    `client-transport=stdio`,
    `authorization=${auth}`,
    `token=${token}`,
  ];
}

function authHeaders(
  token: string,
  authRequired: boolean,
): Record<string, string> | undefined {
  if (authRequired && token) {
    return { Authorization: `Bearer ${token}` };
  }
  return undefined;
}

// ---------------------------------------------------------------------------
// Agent Registry
// ---------------------------------------------------------------------------

const MCP_SERVER_NAME = 'unreal-mcp';

export const agentRegistry: readonly AgentDefinition[] = [
  // ── Claude Code ──────────────────────────────────────────────
  {
    id: 'claude-code',
    name: 'Claude Code',
    skillsPath: '.claude/skills',
    configPathDisplay: '.mcp.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: (p) => path.join(p, '.mcp.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'http',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['type', 'url'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Claude Desktop ───────────────────────────────────────────
  {
    id: 'claude-desktop',
    name: 'Claude Desktop',
    skillsPath: null,
    configPathDisplay: '~/Claude/claude_desktop_config.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: () => {
      if (isWindows()) {
        return path.join(appData(), 'Claude', 'claude_desktop_config.json');
      }
      if (isMac()) {
        return path.join(home(), 'Library', 'Application Support', 'Claude', 'claude_desktop_config.json');
      }
      return path.join(home(), '.config', 'Claude', 'claude_desktop_config.json');
    },
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'stdio',
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'http',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Cursor ───────────────────────────────────────────────────
  {
    id: 'cursor',
    name: 'Cursor',
    skillsPath: '.cursor/skills',
    configPathDisplay: '.cursor/mcp.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: (p) => path.join(p, '.cursor', 'mcp.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'stdio',
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'http',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── VS Code (Copilot) ────────────────────────────────────────
  {
    id: 'vscode-copilot',
    name: 'Visual Studio Code (Copilot)',
    skillsPath: '.github/skills',
    configPathDisplay: '.vscode/mcp.json',
    configFormat: 'json',
    bodyPath: 'servers',
    getConfigPath: (p) => path.join(p, '.vscode', 'mcp.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'stdio',
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'http',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Visual Studio (Copilot) ──────────────────────────────────
  {
    id: 'vs-copilot',
    name: 'Visual Studio (Copilot)',
    skillsPath: '.github/skills',
    configPathDisplay: '.vs/mcp.json',
    configFormat: 'json',
    bodyPath: 'servers',
    getConfigPath: (p) => path.join(p, '.vs', 'mcp.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'stdio',
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'http',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Rider (Junie) ───────────────────────────────────────────
  {
    id: 'rider-junie',
    name: 'Rider (Junie)',
    skillsPath: '.junie/skills',
    configPathDisplay: '.junie/mcp/mcp.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: (p) => path.join(p, '.junie', 'mcp', 'mcp.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      enabled: true,
      type: 'stdio',
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      enabled: true,
      type: 'http',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['disabled', 'url'],
    httpRemoveKeys: ['disabled', 'command', 'args'],
  },

  // ── GitHub Copilot CLI ──────────────────────────────────────
  {
    id: 'github-copilot-cli',
    name: 'GitHub Copilot CLI',
    skillsPath: '.github/skills',
    configPathDisplay: '~/.copilot/mcp-config.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: () => path.join(home(), '.copilot', 'mcp-config.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      command: serverPath,
      args: stdioArgs(port, auth, token),
      tools: ['*'],
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'http',
      url,
      tools: ['*'],
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url', 'type'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Gemini ──────────────────────────────────────────────────
  {
    id: 'gemini',
    name: 'Gemini',
    skillsPath: '.gemini/skills',
    configPathDisplay: '.gemini/settings.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: (p) => path.join(p, '.gemini', 'settings.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'stdio',
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'http',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Antigravity ─────────────────────────────────────────────
  {
    id: 'antigravity',
    name: 'Antigravity',
    skillsPath: '.agent/skills',
    configPathDisplay: '~/.gemini/config/mcp_config.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: () => path.join(home(), '.gemini', 'config', 'mcp_config.json'),
    // Antigravity uses a `serverUrl` key (not `url`) and a `disabled` flag.
    getStdioProps: (serverPath, port, auth, token) => ({
      disabled: false,
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, _token, _authRequired) => ({
      disabled: false,
      serverUrl: url,
    }),
    stdioRemoveKeys: ['url', 'serverUrl', 'type'],
    httpRemoveKeys: ['command', 'args', 'url', 'type'],
  },

  // ── Cline ───────────────────────────────────────────────────
  {
    id: 'cline',
    name: 'Cline',
    skillsPath: '.cline/skills',
    configPathDisplay: '~/Code/globalStorage/.../cline_mcp_settings.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: () => {
      if (isWindows()) {
        return path.join(
          appData(),
          'Code',
          'User',
          'globalStorage',
          'saoudrizwan.claude-dev',
          'settings',
          'cline_mcp_settings.json',
        );
      }
      const base = isMac()
        ? path.join(home(), 'Library', 'Application Support', 'Code', 'User', 'globalStorage')
        : path.join(home(), '.config', 'Code', 'User', 'globalStorage');
      return path.join(base, 'saoudrizwan.claude-dev', 'settings', 'cline_mcp_settings.json');
    },
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'stdio',
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'streamableHttp',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Open Code ───────────────────────────────────────────────
  {
    id: 'open-code',
    name: 'Open Code',
    skillsPath: '.opencode/skills',
    configPathDisplay: 'opencode.json',
    configFormat: 'json',
    bodyPath: 'mcp',
    getConfigPath: (p) => path.join(p, 'opencode.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'local',
      enabled: true,
      command: [serverPath, ...stdioArgs(port, auth, token)],
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'remote',
      enabled: true,
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url', 'args'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Codex (TOML config) ─────────────────────────────────────
  {
    id: 'codex',
    name: 'Codex',
    skillsPath: '.agents/skills',
    configPathDisplay: '.codex/config.toml',
    configFormat: 'toml',
    bodyPath: 'mcp_servers',
    getConfigPath: (p) => path.join(p, '.codex', 'config.toml'),
    // Codex's stdio arg vector omits the bearer token (it is not accepted on
    // Codex's command line); auth still flows via `authorization=`.
    getStdioProps: (serverPath, port, auth, _token) => ({
      enabled: true,
      command: serverPath,
      args: [`port=${port}`, `client-transport=stdio`, `authorization=${auth}`],
      tool_timeout_sec: 300,
    }),
    getHttpProps: (url, _token, _authRequired) => ({
      enabled: true,
      url,
      tool_timeout_sec: 300,
      startup_timeout_sec: 30,
    }),
    stdioRemoveKeys: ['url', 'type', 'startup_timeout_sec'],
    httpRemoveKeys: ['command', 'args', 'type'],
  },

  // ── Kilo Code ───────────────────────────────────────────────
  {
    id: 'kilo-code',
    name: 'Kilo Code',
    skillsPath: '.kilocode/skills',
    configPathDisplay: '.kilocode/mcp.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: (p) => path.join(p, '.kilocode', 'mcp.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'stdio',
      disabled: false,
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'streamable-http',
      disabled: false,
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url'],
    httpRemoveKeys: ['command', 'args'],
  },

  // ── Custom (generic mcpServers entry written to a caller path) ─
  {
    id: 'custom',
    name: 'Custom (generic MCP client)',
    skillsPath: null,
    configPathDisplay: 'mcp.json',
    configFormat: 'json',
    bodyPath: 'mcpServers',
    getConfigPath: (p) => path.join(p, 'mcp.json'),
    getStdioProps: (serverPath, port, auth, token) => ({
      type: 'stdio',
      command: serverPath,
      args: stdioArgs(port, auth, token),
    }),
    getHttpProps: (url, token, authRequired) => ({
      type: 'http',
      url,
      ...(authHeaders(token, authRequired) ? { headers: authHeaders(token, authRequired) } : {}),
    }),
    stdioRemoveKeys: ['url'],
    httpRemoveKeys: ['command', 'args'],
  },
] as const;

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------

export function getAgentById(id: string): AgentDefinition | undefined {
  return agentRegistry.find((a) => a.id === id);
}

export function getAgentIds(): string[] {
  return agentRegistry.map((a) => a.id);
}

export function listAgentTable(
  heading: string,
  locationLabel: string,
  locationFn: (agent: AgentDefinition) => string,
): void {
  const sorted = [...agentRegistry].sort((a, b) => a.id.localeCompare(b.id));

  const colId = 'ID';
  const colLoc = locationLabel;

  const wId = Math.max(colId.length, ...sorted.map((a) => a.id.length));
  const wLoc = Math.max(colLoc.length, ...sorted.map((a) => locationFn(a).length));

  const sep = chalk.dim;
  const hBar = (w: number) => '─'.repeat(w);

  console.log(`\n${chalk.bold.cyan(heading)}\n`);

  // Header
  console.log(sep('  ┌─') + sep(hBar(wId)) + sep('─┬─') + sep(hBar(wLoc)) + sep('─┐'));
  console.log(
    sep('  │ ') + chalk.bold.white(colId.padEnd(wId)) + sep(' │ ') + chalk.bold.white(colLoc.padEnd(wLoc)) + sep(' │'),
  );
  console.log(sep('  ├─') + sep(hBar(wId)) + sep('─┼─') + sep(hBar(wLoc)) + sep('─┤'));

  // Rows
  for (const agent of sorted) {
    const loc = locationFn(agent);
    console.log(
      sep('  │ ') + chalk.yellow(agent.id.padEnd(wId)) + sep(' │ ') + chalk.green(loc.padEnd(wLoc)) + sep(' │'),
    );
  }

  // Footer
  console.log(sep('  └─') + sep(hBar(wId)) + sep('─┴─') + sep(hBar(wLoc)) + sep('─┘'));
  console.log('');
}

// ---------------------------------------------------------------------------
// Config file writing — JSON
// ---------------------------------------------------------------------------

/**
 * Merge `props` into the `[bodyPath][serverName]` entry of the JSON config at
 * `configPath`, preserving every other server entry, and return the serialized
 * content. When `dryRun` is true the merged content is computed and returned
 * but NOTHING is written (side-effect-free — no dir creation, no file write).
 */
export function writeJsonAgentConfig(
  configPath: string,
  bodyPath: string,
  serverName: string,
  props: Record<string, unknown>,
  removeKeys: string[],
  dryRun = false,
): string {
  let root: Record<string, unknown> = {};
  if (fs.existsSync(configPath)) {
    try {
      root = JSON.parse(fs.readFileSync(configPath, 'utf-8')) as Record<string, unknown>;
    } catch {
      // If the file is malformed, start fresh
      root = {};
    }
    if (!root || typeof root !== 'object' || Array.isArray(root)) {
      root = {};
    }
  }

  // Navigate/create bodyPath
  let body = root[bodyPath] as Record<string, unknown> | undefined;
  if (!body || typeof body !== 'object' || Array.isArray(body)) {
    body = {};
    root[bodyPath] = body;
  }

  // Get or create the server entry
  let entry = body[serverName] as Record<string, unknown> | undefined;
  if (!entry || typeof entry !== 'object' || Array.isArray(entry)) {
    entry = {};
  }

  // Remove stale keys (drops the other transport's leftovers on a re-run)
  for (const key of removeKeys) {
    delete entry[key];
  }

  // Merge new properties
  for (const [key, value] of Object.entries(props)) {
    entry[key] = value;
  }

  body[serverName] = entry;
  root[bodyPath] = body;

  const content = JSON.stringify(root, null, 2) + '\n';
  if (!dryRun) {
    const dir = path.dirname(configPath);
    if (!fs.existsSync(dir)) {
      fs.mkdirSync(dir, { recursive: true });
    }
    fs.writeFileSync(configPath, content);
  }
  return content;
}

// ---------------------------------------------------------------------------
// Config file writing — TOML (Codex only)
// ---------------------------------------------------------------------------

/**
 * Write/merge a single `[bodyPath.serverName]` TOML section into `configPath`,
 * mirroring the Unity/Godot CLI's Codex TOML branch, and return the serialized
 * content. Existing sections under other headers are preserved; the target
 * section is replaced wholesale (idempotent re-runs; stale keys dropped). Keys
 * in `removeKeys` are never written. `dryRun` computes + returns the content
 * without touching disk. A deliberately minimal TOML emitter — Codex's schema
 * here is flat (string/number/bool/array scalars only).
 */
export function writeTomlAgentConfig(
  configPath: string,
  bodyPath: string,
  serverName: string,
  props: Record<string, unknown>,
  removeKeys: string[],
  dryRun = false,
): string {
  // Read existing content or start fresh
  let lines: string[] = [];
  if (fs.existsSync(configPath)) {
    lines = fs.readFileSync(configPath, 'utf-8').split('\n');
  }

  const sectionHeader = `[${bodyPath}.${serverName}]`;

  // Find existing section boundaries
  const sectionIdx = lines.findIndex((l) => l.trim() === sectionHeader);

  // Build TOML key-value pairs for the section
  const tomlLines = [sectionHeader];
  for (const [key, value] of Object.entries(props)) {
    if (removeKeys.includes(key)) continue;
    tomlLines.push(`${key} = ${tomlValue(value)}`);
  }

  if (sectionIdx >= 0) {
    // Find end of section (next [...] header or EOF). The leading-`[` test is
    // safe for the Codex schema, which emits only flat scalars (no inline-array
    // value lines that would also start with `[`).
    let endIdx = sectionIdx + 1;
    while (endIdx < lines.length && !lines[endIdx].trim().startsWith('[')) {
      endIdx++;
    }
    lines.splice(sectionIdx, endIdx - sectionIdx, ...tomlLines);
  } else {
    if (lines.length > 0 && lines[lines.length - 1].trim() !== '') {
      lines.push('');
    }
    lines.push(...tomlLines);
  }

  const content = lines.join('\n') + '\n';
  if (!dryRun) {
    const dir = path.dirname(configPath);
    if (!fs.existsSync(dir)) {
      fs.mkdirSync(dir, { recursive: true });
    }
    fs.writeFileSync(configPath, content);
  }
  return content;
}

function tomlValue(v: unknown): string {
  if (typeof v === 'string') return `"${v.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
  if (typeof v === 'boolean') return String(v);
  if (typeof v === 'number') {
    // TOML spells non-finite floats `nan` / `inf` / `-inf`, not JS's NaN/Infinity.
    if (Number.isNaN(v)) return 'nan';
    if (v === Infinity) return 'inf';
    if (v === -Infinity) return '-inf';
    return String(v);
  }
  if (Array.isArray(v)) {
    return `[${v.map(tomlValue).join(', ')}]`;
  }
  // null/undefined/object have no valid TOML scalar form here; emit a quoted
  // string so we never produce an invalid bare token (defensive fallback).
  return `"${String(v).replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
}

export { MCP_SERVER_NAME };
