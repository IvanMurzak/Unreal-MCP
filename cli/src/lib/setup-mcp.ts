// `setup-mcp` — write an MCP client config snippet for a supported AI agent so
// it can reach the project's local/cloud Unreal MCP server. Library-safe.
//
// The per-agent knowledge (config path, body key, config format, and the
// stdio/http server-entry shapes) lives in the shared `utils/agents.ts`
// registry — the same design the Unity and Godot CLIs use, so all three stay
// consistent. This module is the orchestrator: it resolves the connection,
// acquires the local `gamedev-mcp-server` binary for stdio transport (via the
// §6 `download-server.ts` resolver), and merges the chosen agent's entry into
// its config file (JSON or TOML), preserving any existing servers.

import * as path from 'path';
import { platform } from 'os';
// cli-core owns the T4 pinned-routing policy: `pinUrl` appends the `/p/<pin>`
// routing segment and `derivePinV2` is the shared v2 ProjectIdentity pin
// (`\`→`/`-normalized), so the CLI writes the SAME pinned URL the C# Editor
// Configure and the sibling CLIs write for a given project.
import {
  pinUrl,
  derivePinV2,
  isCloudUrl,
  toAuthServerRoot,
  createProjectKeyResolver,
  unrealAdapter,
  type ProjectKeyResult,
  type SetupMcpCredential,
} from '@baizor/gamedev-cli-core';
import { resolveConnection, appendMcp } from '../utils/config.js';
import { asError } from '../utils/error.js';
import { generatePortFromDirectory } from '../utils/port.js';
import {
  getAgentById,
  getAgentIds,
  writeJsonAgentConfig,
  writeTomlAgentConfig,
  MCP_SERVER_NAME,
} from '../utils/agents.js';
import {
  downloadServer,
  resolveServerBinaryPath,
  resolveServerOverride,
  SERVER_PATH_ENV_VAR,
} from './download-server.js';
import { emitProgress } from './progress.js';
import type { McpTransport, SetupMcpOptions, SetupMcpResult } from './types.js';

// Re-exported for compatibility: the §6 install-path resolver lives in
// `download-server.ts` (single owner of the server-binary layout).
export { resolveServerBinaryPath } from './download-server.js';

/** Valid agent ids accepted by `setupMcp`. Pure. */
export function listAgentIds(): string[] {
  return getAgentIds();
}

/**
 * Decide whether `setup-mcp` should write a static `Authorization: Bearer` header
 * into an http client config. Pure — exported for unit tests.
 *
 * Design decision D11 / Flow A: an OAuth-capable interactive client (Claude Code,
 * Cursor, Codex, Copilot, …) must receive a CREDENTIAL-FREE, URL-only config so it
 * performs its own native RFC 9728 OAuth against the endpoint. A static Bearer
 * header (a) is rejected by the hosted OAuth server (401) and (b) SUPPRESSES the
 * client's own OAuth handshake ("OAuth fallback is disabled when headers.
 * Authorization is set"). So a header is written ONLY when a credential is present
 * AND either the client is NOT OAuth-capable (`supportsOAuth:false`) OR the caller
 * EXPLICITLY opted into a PAT (passed `--token`) — Flow C, the recommended path for
 * headless CI / self-hosted required-auth against a non-OAuth endpoint. An ambient
 * token (resolved from the project `.env` / process env) never forces a header onto
 * an OAuth-capable client. Mirrors the shared b6 C# configurators / `configure --agent`.
 */
export function shouldWriteAuthHeader(args: {
  /** The resolved credential (`''` when none). */
  token: string;
  /** The agent's OAuth capability (`AgentDefinition.supportsOAuth`). */
  supportsOAuth: boolean;
  /** `true` when the user EXPLICITLY supplied a token (PAT opt-in), not an ambient one. */
  explicitPatOptIn: boolean;
}): boolean {
  if (args.token.length === 0) return false;
  return !args.supportsOAuth || args.explicitPatOptIn;
}

export async function setupMcp(opts: SetupMcpOptions): Promise<SetupMcpResult> {
  const warnings: string[] = [];
  const nextSteps: string[] = [];
  try {
    const agent = getAgentById(opts.agentId);
    if (!agent) {
      throw new Error(
        `Unknown agent "${opts.agentId}". Valid agents: ${getAgentIds().join(', ')}.`,
      );
    }
    const transport: McpTransport = opts.transport ?? 'http';
    const projectDir = path.resolve(opts.projectDir ?? process.cwd());

    const conn = await resolveConnection({
      projectDir,
      url: opts.url,
      token: opts.token,
      machineAuth: opts.machineAuth,
    });

    // Build the agent's server-entry props for the chosen transport.
    let props: Record<string, unknown>;
    let removeKeys: string[];
    let credential: SetupMcpCredential = 'none';
    let key: Extract<ProjectKeyResult, { kind: 'ok' }> | undefined;

    // Validated before the transport branch so a stdio run refuses `--regenerate-key` instead of
    // silently succeeding without regenerating anything.
    const explicitPatOptIn = (opts.token ?? '').trim().length > 0;
    const cloud = transport === 'http' && isCloudUrl(appendMcp(conn.url));
    if (opts.regenerateKey && (opts.oauth || explicitPatOptIn || !cloud || opts.dryRun)) {
      throw new Error(
        '--regenerate-key applies only to a Cloud http config without --oauth / --token / --dry-run ' +
          '(project keys are not used for stdio or a local server).',
      );
    }

    if (transport === 'stdio') {
      // stdio needs a LOCAL server binary. Resolution: the UNREAL_MCP_SERVER_PATH
      // override wins (no download, no version check — §6 dev-override semantics,
      // mirroring UNREAL_MCP_BRIDGE_PATH); otherwise download/refresh the pinned
      // gamedev-mcp-server release into the §6 install path. A failed download
      // degrades to a warning (the config is still written and works once the
      // binary is provided), never a hard failure.
      const env = opts.env ?? process.env;
      const osPlatform = platform() as NodeJS.Platform;
      let serverPath: string | null = resolveServerOverride(env);
      if (!serverPath && opts.dryRun) {
        // Dry run must stay side-effect-free: report the would-be path.
        serverPath = resolveServerBinaryPath(projectDir, osPlatform);
      } else if (!serverPath) {
        const download = opts.downloadServerImpl ?? downloadServer;
        const dl = await download({ projectDir, env, onProgress: opts.onProgress });
        if (dl.kind === 'success') {
          warnings.push(...dl.warnings);
          serverPath = dl.serverPath;
        } else {
          serverPath = resolveServerBinaryPath(projectDir, osPlatform);
          warnings.push(`Could not download the gamedev-mcp-server binary: ${dl.error.message}`);
          nextSteps.push(
            `Provide the server binary manually: set ${SERVER_PATH_ENV_VAR} to a local gamedev-mcp-server build, or re-run this command once the download can succeed.`,
          );
        }
      }
      const port = generatePortFromDirectory(projectDir);
      const auth = conn.token ? 'required' : 'none';
      props = agent.getStdioProps(serverPath, port, auth, conn.token ?? '');
      // A stdio entry never carries a static http header: drop one a previous Cloud http run wrote,
      // or a live project key would linger in the file.
      removeKeys = [...agent.stdioRemoveKeys, 'headers'];
    } else {
      // http — point the agent at the resolved `<host>/mcp` client URL. The
      // `/mcp` segment is appended ONCE here (idempotently, tolerating a URL
      // that already ends in `/mcp`) so the agent closures use `url` verbatim;
      // this avoids the historical `/mcp` double-append.
      //
      // T4 (defect B4/B8): the URL is PINNED by default —
      // `<base>/mcp/p/<pin-v2>` — so the config routes strictly to THIS
      // project's engine instance even when the account has several, exactly as
      // the Editor Configure does. `--no-pin` is the escape hatch (unpinned).
      // M8: the pin is a routing path segment, not part of the OAuth resource;
      // the canonical resource stays `<base>/mcp`.
      const canonicalUrl = appendMcp(conn.url);
      const pin = derivePinV2(projectDir);
      const httpUrl = opts.noPin ? canonicalUrl : pinUrl(canonicalUrl, pin);
      const token = conn.token ?? '';

      // Project keys (contract §7): a Cloud http config carries `Authorization: Bearer agd_pk_…` for
      // EVERY client — a non-expiring key bound to this project's pin, reused from the machine cache or
      // minted with the machine login. `--oauth` opts out; an explicit `--token` wins; no login / a
      // refused mint (e.g. 404 while the server feature is off) falls back to the URL-only config.
      if (cloud && !explicitPatOptIn && !opts.oauth) {
        if (opts.dryRun) {
          warnings.push('Dry run: the project key is not resolved, so the snippet is URL-only; a real run writes the project key when this machine is signed in.');
        } else {
          const resolver = opts.projectKeyResolver ?? createProjectKeyResolver(unrealAdapter);
          const outcome = await resolver({
            issuer: toAuthServerRoot(canonicalUrl),
            pin,
            engine: 'unreal',
            label: projectDir,
            machineName: opts.machineName,
            regenerate: opts.regenerateKey === true,
          });
          if (outcome.kind === 'ok') {
            key = outcome;
            warnings.push(...outcome.warnings);
          } else if (opts.regenerateKey) {
            throw new Error(`Could not regenerate the project key: ${outcome.reason}`);
          } else {
            warnings.push(
              `No project key written (${outcome.reason}) — the config is URL-only and the agent signs in with its own OAuth. ` +
                'Sign in on this machine (`unreal-mcp-cli login`) and run setup-mcp again to write a project key.',
            );
          }
        }
      }

      // D11 / Flow A: without a project key, OAuth-capable clients get a CREDENTIAL-FREE,
      // URL-only config so they run their own native OAuth; a static Bearer header is emitted
      // only for a non-OAuth client (`supportsOAuth:false`) or an EXPLICIT PAT opt-in (the
      // caller passed `--token`) — Flow C. An ambient token (from the project `.env` / process
      // env) never forces a header. See `shouldWriteAuthHeader`.
      // A project key is only resolved without an explicit `--token`, so it never competes with one.
      const writeAuthHeader =
        !!key ||
        (agent.patInHeader !== false && shouldWriteAuthHeader({ token, supportsOAuth: agent.supportsOAuth, explicitPatOptIn }));
      credential = key ? 'project-key' : writeAuthHeader ? 'token' : 'none';
      props = agent.getHttpProps(httpUrl, key?.key ?? token, writeAuthHeader);
      removeKeys = agent.httpRemoveKeys;
    }

    const configPath = agent.getConfigPath(projectDir);
    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Configuring ${agent.name} MCP for ${transport} transport`,
    });

    const snippet =
      agent.configFormat === 'toml'
        ? writeTomlAgentConfig(configPath, agent.bodyPath, MCP_SERVER_NAME, props, removeKeys, opts.dryRun)
        : writeJsonAgentConfig(configPath, agent.bodyPath, MCP_SERVER_NAME, props, removeKeys, opts.dryRun);

    // Regenerate (§7): the new key is cached and the config rewritten — only now revoke the old
    // key. A revoke failure is reported, never fatal.
    if (key?.revokePrevious && !opts.dryRun) {
      try {
        const revokeWarning = await key.revokePrevious();
        if (revokeWarning) warnings.push(revokeWarning);
      } catch (err) {
        warnings.push(`Revoking the previous project key failed (${asError(err).message}).`);
      }
    }

    if (opts.dryRun) {
      emitProgress(opts.onProgress, { phase: 'done', message: 'Dry run — config not written.' });
    } else {
      emitProgress(opts.onProgress, {
        phase: 'file-written',
        message: `Wrote ${configPath}`,
        filePath: configPath,
      });
    }

    nextSteps.push(`Restart ${agent.name} to pick up the new MCP server.`);
    if (transport === 'http') {
      nextSteps.push('Ensure the Unreal Editor (and its MCP server) is running — see `unreal-mcp-cli status`.');
    }

    return {
      kind: 'success',
      success: true,
      agentId: agent.id,
      configPath,
      transport,
      snippet,
      credential,
      projectKeyId: key?.keyId,
      projectKeySource: key?.source,
      warnings,
      nextSteps,
    };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      warnings,
      nextSteps,
      error: asError(err),
    };
  }
}
