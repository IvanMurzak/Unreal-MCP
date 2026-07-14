import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import {
  pinnedUrl,
  originMatches,
  applyPinToConfigText,
  projectLocalAgentConfigPaths,
  upsertProjectPin,
} from '../src/utils/pin-upsert.js';

const PIN = 'deadbeef';

describe('pinnedUrl', () => {
  it('appends /p/<pin> after a hosted /mcp path', () => {
    expect(pinnedUrl('https://ai-game.dev/mcp', PIN)).toBe('https://ai-game.dev/mcp/p/deadbeef');
  });

  it('appends /p/<pin> to a bare local origin', () => {
    expect(pinnedUrl('http://localhost:24567', PIN)).toBe('http://localhost:24567/p/deadbeef');
  });

  it('is idempotent — replaces an existing stale pin segment, never nests', () => {
    const once = pinnedUrl('https://ai-game.dev/mcp', PIN);
    expect(pinnedUrl(once, PIN)).toBe(once);
    expect(pinnedUrl('https://ai-game.dev/mcp/p/00112233', PIN)).toBe('https://ai-game.dev/mcp/p/deadbeef');
  });

  it('leaves a non-URL string unchanged', () => {
    expect(pinnedUrl('not a url', PIN)).toBe('not a url');
  });
});

describe('originMatches', () => {
  it('matches by origin when a serverTarget is given', () => {
    expect(originMatches('https://ai-game.dev/mcp', 'https://ai-game.dev')).toBe(true);
    expect(originMatches('https://evil.example/mcp', 'https://ai-game.dev')).toBe(false);
    expect(originMatches('http://localhost:8080/mcp', 'http://localhost:8080')).toBe(true);
    expect(originMatches('http://localhost:9999/mcp', 'http://localhost:8080')).toBe(false);
  });

  it('falls back to ai-game.dev + loopback hosts when no target', () => {
    expect(originMatches('https://ai-game.dev/mcp')).toBe(true);
    expect(originMatches('http://localhost:1234')).toBe(true);
    expect(originMatches('https://example.com/mcp')).toBe(false);
  });
});

describe('applyPinToConfigText', () => {
  it('rewrites a JSON url value, preserving formatting', () => {
    const src = '{\n  "mcpServers": {\n    "ai-game-developer": {\n      "type": "http",\n      "url": "https://ai-game.dev/mcp"\n    }\n  }\n}';
    const { text, changed } = applyPinToConfigText(src, PIN, 'https://ai-game.dev');
    expect(changed).toBe(true);
    expect(text).toContain('"url": "https://ai-game.dev/mcp/p/deadbeef"');
    // Formatting untouched apart from the URL token.
    expect(text).toContain('"type": "http"');
  });

  it('rewrites a TOML url value', () => {
    const src = '[mcp_servers.unreal-mcp]\nenabled = true\nurl = "https://ai-game.dev/mcp"\n';
    const { text, changed } = applyPinToConfigText(src, PIN, 'https://ai-game.dev');
    expect(changed).toBe(true);
    expect(text).toContain('url = "https://ai-game.dev/mcp/p/deadbeef"');
  });

  it('rewrites the Antigravity serverUrl key', () => {
    const src = '{ "mcpServers": { "x": { "serverUrl": "https://ai-game.dev/mcp" } } }';
    const { text, changed } = applyPinToConfigText(src, PIN, 'https://ai-game.dev');
    expect(changed).toBe(true);
    expect(text).toContain('"serverUrl": "https://ai-game.dev/mcp/p/deadbeef"');
  });

  it('does NOT rewrite a non-matching origin', () => {
    const src = '{ "mcpServers": { "other": { "url": "https://other.example/mcp" } } }';
    const { text, changed } = applyPinToConfigText(src, PIN, 'https://ai-game.dev');
    expect(changed).toBe(false);
    expect(text).toBe(src);
  });

  it('is a no-op when already pinned', () => {
    const src = '{ "url": "https://ai-game.dev/mcp/p/deadbeef" }';
    const { changed } = applyPinToConfigText(src, PIN, 'https://ai-game.dev');
    expect(changed).toBe(false);
  });
});

describe('projectLocalAgentConfigPaths', () => {
  it('returns only paths under the project root (no global/home configs)', () => {
    const projectDir = path.resolve('/tmp/some-unreal-project');
    const paths = projectLocalAgentConfigPaths(projectDir);
    expect(paths.length).toBeGreaterThan(0);
    for (const p of paths) {
      expect(p.startsWith(projectDir)).toBe(true);
    }
    // A known project-local config (Claude Code .mcp.json) is included; a known
    // global one (Claude Desktop) is not.
    expect(paths).toContain(path.join(projectDir, '.mcp.json'));
    expect(paths.some((p) => p.includes('claude_desktop_config.json'))).toBe(false);
  });
});

describe('upsertProjectPin (filesystem)', () => {
  let dir: string;
  beforeEach(() => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'unreal-mcp-pin-'));
  });
  afterEach(() => {
    fs.rmSync(dir, { recursive: true, force: true });
  });

  it('pins a manually-added Claude Code .mcp.json server URL', () => {
    const mcpJson = path.join(dir, '.mcp.json');
    fs.writeFileSync(
      mcpJson,
      JSON.stringify({ mcpServers: { 'ai-game-developer': { type: 'http', url: 'https://ai-game.dev/mcp' } } }, null, 2),
    );
    const result = upsertProjectPin(dir, PIN, { serverTarget: 'https://ai-game.dev' });
    expect(result.updatedFiles).toEqual([mcpJson]);
    const parsed = JSON.parse(fs.readFileSync(mcpJson, 'utf-8'));
    expect(parsed.mcpServers['ai-game-developer'].url).toBe('https://ai-game.dev/mcp/p/deadbeef');
  });

  it('skips configs with no matching server and reports no updates', () => {
    const mcpJson = path.join(dir, '.mcp.json');
    fs.writeFileSync(mcpJson, JSON.stringify({ mcpServers: { other: { url: 'https://other.example/mcp' } } }));
    const result = upsertProjectPin(dir, PIN, { serverTarget: 'https://ai-game.dev' });
    expect(result.updatedFiles).toEqual([]);
  });

  it('is idempotent across re-runs', () => {
    const configPath = path.join(dir, 'mcp.json');
    fs.writeFileSync(configPath, JSON.stringify({ mcpServers: { x: { url: 'https://ai-game.dev/mcp' } } }));
    upsertProjectPin(dir, PIN, { serverTarget: 'https://ai-game.dev', configPaths: [configPath] });
    const second = upsertProjectPin(dir, PIN, { serverTarget: 'https://ai-game.dev', configPaths: [configPath] });
    expect(second.updatedFiles).toEqual([]);
  });
});
