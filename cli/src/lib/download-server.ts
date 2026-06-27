// `download-server` — fetch the shared `gamedev-mcp-server` binary (the
// engine-agnostic MCP server released from
// https://github.com/IvanMurzak/GameDev-MCP-Server) into the project's §6
// install layout: `<project>/Intermediate/UnrealMCP/server/<rid>/` plus a
// `version` marker file. Library-safe.
//
// The consumed server version is the pinned SERVER_VERSION constant
// (`server-version.ts`) — NOT the plugin/cli version. A cached binary whose
// `version` marker matches the pin is reused without touching the network.
//
// Dev override: the `UNREAL_MCP_SERVER_PATH` env var (mirroring
// `UNREAL_MCP_BRIDGE_PATH`, §6) points at a locally built server binary and
// skips the download + version check entirely.
//
// Zip-layout note (ground truth from the v8.0.0 release): the win zips are
// FLAT (gamedev-mcp-server.exe + ~7 sidecar files — appsettings.json,
// NLog.config, server.json, web.config, … — at the zip root) while the
// osx/linux zips wrap everything in a `<rid>/` folder. Extraction therefore
// goes through a throwaway staging dir, FINDS the binary wherever it landed
// (shallowest match wins), and moves the binary plus every sidecar file
// beside it into the install dir — so BOTH layouts (and any future
// re-arrangement) resolve correctly. Same pattern as Godot-MCP's
// `GodotMcpServerManager` (Godot-MCP PR #101).

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { createHash } from 'node:crypto';
import { unzipSync } from 'fflate';
import { ridForPlatform } from './bootstrap-local.js';
import { SERVER_VERSION } from './server-version.js';
import { asError } from '../utils/error.js';
import { emitProgress } from './progress.js';
import {
  serverChecksumsUrl,
  serverZipAssetName,
  verifyZip,
  checksumFailureReason,
  SHA256SUMS_ASSET_NAME,
} from './server-checksum.js';
import type { DownloadServerOptions, DownloadServerResult } from './types.js';

/** Attempts (1 initial + retries) for the SHA256SUMS manifest fetch before fail-closed. */
const SHA256SUMS_FETCH_ATTEMPTS = 3;

/**
 * Fetch the `SHA256SUMS` manifest text with a bounded transient-retry, reusing
 * the injectable `fetchImpl` so a test can inject the manifest. Returns the
 * manifest body, or `null` when every attempt failed (the fail-closed signal —
 * an unverified binary must NEVER be executed). Never throws.
 */
async function fetchSha256SumsText(
  url: string,
  fetchImpl: typeof fetch,
  warnings: string[],
): Promise<string | null> {
  for (let attempt = 1; attempt <= SHA256SUMS_FETCH_ATTEMPTS; attempt++) {
    try {
      const response = await fetchImpl(url);
      if (response.ok) return await response.text();
      // An HTTP error is treated as transient and retried (a release just being
      // published can briefly 404 the manifest); a persistent failure fails closed.
      warnings.push(`${SHA256SUMS_ASSET_NAME} fetch attempt ${attempt}/${SHA256SUMS_FETCH_ATTEMPTS} failed: HTTP ${response.status}.`);
    } catch (err: unknown) {
      warnings.push(
        `${SHA256SUMS_ASSET_NAME} fetch attempt ${attempt}/${SHA256SUMS_FETCH_ATTEMPTS} failed: ${asError(err).message}.`,
      );
    }
  }
  return null;
}

/** Base name of the shared server binary (extension added per-OS). */
export const SERVER_BINARY_BASENAME = 'gamedev-mcp-server';

/** Env var that overrides the server binary path (skips download + version check). */
export const SERVER_PATH_ENV_VAR = 'UNREAL_MCP_SERVER_PATH';

/** GitHub repo the server binaries (and the SHA256SUMS manifest) are released from. */
export const SERVER_RELEASE_REPO = 'IvanMurzak/GameDev-MCP-Server';

/** Executable file name for an OS: `gamedev-mcp-server(.exe)`. Pure. */
export function serverExecutableName(osPlatform: NodeJS.Platform): string {
  return `${SERVER_BINARY_BASENAME}${osPlatform === 'win32' ? '.exe' : ''}`;
}

/** Release-asset download URL for a RID + version (tags are v-prefixed). Pure. */
export function serverDownloadUrl(rid: string, version: string = SERVER_VERSION): string {
  return `https://github.com/${SERVER_RELEASE_REPO}/releases/download/v${version}/${SERVER_BINARY_BASENAME}-${rid}.zip`;
}

/** §6 install dir: `<project>/Intermediate/UnrealMCP/server/<rid>`. Pure. */
export function serverInstallDir(
  projectDir: string,
  osPlatform: NodeJS.Platform,
  arch: string = process.arch,
): string {
  return path.join(
    path.resolve(projectDir),
    'Intermediate',
    'UnrealMCP',
    'server',
    ridForPlatform(osPlatform, arch),
  );
}

/** Absolute path to the installed server binary under the §6 layout. Pure. */
export function resolveServerBinaryPath(
  projectDir: string,
  osPlatform: NodeJS.Platform,
  arch: string = process.arch,
): string {
  return path.join(serverInstallDir(projectDir, osPlatform, arch), serverExecutableName(osPlatform));
}

/**
 * Resolve the `UNREAL_MCP_SERVER_PATH` override: the value when it is set AND
 * points at an existing file (mirrors the C++ `UNREAL_MCP_BRIDGE_PATH`
 * semantics — a set-but-missing override falls through), else `null`.
 */
export function resolveServerOverride(env: NodeJS.ProcessEnv = process.env): string | null {
  const override = env[SERVER_PATH_ENV_VAR];
  return override && fs.existsSync(override) && fs.statSync(override).isFile() ? override : null;
}

/** The version recorded in the install dir's `version` marker, or `null`. */
export function readVersionMarker(installDir: string): string | null {
  const marker = path.join(installDir, 'version');
  try {
    return fs.existsSync(marker) ? fs.readFileSync(marker, 'utf-8').trim() : null;
  } catch {
    return null;
  }
}

/**
 * Locate the extracted server binary under the staging dir, wherever the zip
 * layout put it — at the root (the FLAT win zips) or nested in a `<rid>/`
 * folder (the osx/linux zips). Prefers the SHALLOWEST match so a hypothetical
 * nested duplicate cannot shadow the real binary. Returns `null` when no file
 * with the expected name exists. Exported for tests.
 */
export function findExtractedBinary(stagingDir: string, executableName: string): string | null {
  let best: string | null = null;
  let bestDepth = Number.MAX_SAFE_INTEGER;
  const walk = (dir: string, depth: number): void => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        walk(full, depth + 1);
      } else if (entry.isFile() && entry.name === executableName && depth < bestDepth) {
        best = full;
        bestDepth = depth;
      }
    }
  };
  walk(stagingDir, 1);
  return best;
}

/**
 * Download + install the pinned `gamedev-mcp-server` binary when it is
 * missing or version-mismatched. Resolution order:
 *
 * 1. `UNREAL_MCP_SERVER_PATH` override → used as-is (no download, no check).
 * 2. Cached binary whose `version` marker equals the pin → reused.
 * 3. Otherwise: download the release zip, staging-extract, move the binary +
 *    its sidecar files into the install dir, chmod on Unix, write `version`.
 *
 * Never throws past the boundary — failures return `{ kind: 'failure' }`.
 */
export async function downloadServer(opts: DownloadServerOptions): Promise<DownloadServerResult> {
  const warnings: string[] = [];
  let url: string | undefined;
  try {
    if (!opts?.projectDir) throw new Error('projectDir is required.');
    const osPlatform = opts.os ?? (os.platform() as NodeJS.Platform);
    const arch = opts.arch ?? process.arch;
    const version = opts.version ?? SERVER_VERSION;
    const env = opts.env ?? process.env;

    // 1. Dev override — skips download + version check entirely (§6).
    const override = resolveServerOverride(env);
    if (override) {
      emitProgress(opts.onProgress, {
        phase: 'info',
        message: `Using ${SERVER_PATH_ENV_VAR} override: ${override}`,
      });
      return { kind: 'success', success: true, serverPath: override, source: 'override', version: null, warnings };
    }

    const installDir = serverInstallDir(opts.projectDir, osPlatform, arch);
    const exeName = serverExecutableName(osPlatform);
    const exePath = path.join(installDir, exeName);

    // 2. Cache hit — binary present AND version marker matches the pin.
    if (!opts.force && fs.existsSync(exePath) && readVersionMarker(installDir) === version) {
      return { kind: 'success', success: true, serverPath: exePath, source: 'cache', version, warnings };
    }

    // 3. Download the release zip.
    const rid = ridForPlatform(osPlatform, arch);
    url = serverDownloadUrl(rid, version);
    emitProgress(opts.onProgress, {
      phase: 'start',
      message: `Downloading gamedev-mcp-server ${version} (${rid}) from ${url}`,
    });
    const fetchImpl = opts.fetchImpl ?? fetch;
    const response = await fetchImpl(url);
    if (!response.ok) {
      throw new Error(
        `Download failed: HTTP ${response.status} ${response.statusText} for ${url}. ` +
          `Does the GameDev-MCP-Server v${version} release exist?`,
      );
    }
    const zipBytes = new Uint8Array(await response.arrayBuffer());

    // FAIL-CLOSED INTEGRITY GATE (verify-before-execute, issue #155). The zip bytes
    // are in hand but UNTRUSTED. Before unzipSync/extract/execute, fetch the
    // release's SHA256SUMS manifest (sibling of the zip URL under the same
    // v<version> tag), compute the downloaded zip's SHA256 (node:crypto), and
    // compare against the manifest entry for THIS RID. On MISMATCH / MISSING entry /
    // unparsable-or-unfetchable manifest we return the failure path WITHOUT
    // extracting — an unverified binary must NEVER be executed (a compromised
    // release asset or a trusted-CA MITM would otherwise yield arbitrary code
    // execution on the developer's machine).
    const sumsUrl = serverChecksumsUrl(version);
    const sha256SumsText = await fetchSha256SumsText(sumsUrl, fetchImpl, warnings);
    if (sha256SumsText === null) {
      throw new Error(
        `Refusing to launch MCP server: could not download the ${SHA256SUMS_ASSET_NAME} integrity manifest ` +
          `from ${sumsUrl} after ${SHA256SUMS_FETCH_ATTEMPTS} attempt(s). The downloaded binary was NOT ` +
          `verified and will not be extracted (fail-closed).`,
      );
    }
    const actualHexDigest = createHash('sha256').update(zipBytes).digest('hex');
    const assetZipName = serverZipAssetName(rid);
    const verdict = verifyZip(sha256SumsText, assetZipName, actualHexDigest);
    if (verdict !== 'verified') {
      throw new Error(
        `Refusing to launch MCP server: ${checksumFailureReason(verdict, assetZipName)}. ` +
          `The binary will not be extracted or executed (fail-closed).`,
      );
    }
    emitProgress(opts.onProgress, {
      phase: 'info',
      message: `Verified ${assetZipName} against ${SHA256SUMS_ASSET_NAME} (SHA256 OK).`,
    });

    // Staging-dir extraction: unzip everything to a throwaway dir, find the
    // binary wherever the layout put it, then move its folder's files.
    const stagingDir = fs.mkdtempSync(path.join(os.tmpdir(), `${SERVER_BINARY_BASENAME}-extract-`));
    try {
      const entries = unzipSync(zipBytes);
      for (const [entryName, data] of Object.entries(entries)) {
        if (entryName.endsWith('/')) continue; // directory entry
        const target = path.join(stagingDir, entryName);
        // Zip-slip guard: never write outside the staging dir.
        if (!path.resolve(target).startsWith(path.resolve(stagingDir) + path.sep)) {
          warnings.push(`Skipped suspicious zip entry: ${entryName}`);
          continue;
        }
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.writeFileSync(target, data);
      }

      const extractedBinary = findExtractedBinary(stagingDir, exeName);
      if (!extractedBinary) {
        throw new Error(`Server binary '${exeName}' not found inside the downloaded zip (${url}).`);
      }

      // Replace any stale install dir so a partial/old extract can't linger,
      // then move the binary PLUS its sidecar files (appsettings.json,
      // NLog.config, server.json, … — they MUST land next to the binary).
      fs.rmSync(installDir, { recursive: true, force: true });
      fs.mkdirSync(installDir, { recursive: true });
      const sourceDir = path.dirname(extractedBinary);
      for (const entry of fs.readdirSync(sourceDir, { withFileTypes: true })) {
        if (!entry.isFile()) continue;
        fs.renameSync(path.join(sourceDir, entry.name), path.join(installDir, entry.name));
      }
    } finally {
      fs.rmSync(stagingDir, { recursive: true, force: true });
    }

    if (!fs.existsSync(exePath)) {
      throw new Error(`Server binary not found after unpack at: ${exePath}`);
    }
    if (osPlatform !== 'win32') {
      try {
        fs.chmodSync(exePath, 0o755);
      } catch (err: unknown) {
        warnings.push(`Could not mark the server binary executable: ${asError(err).message}`);
      }
    }
    fs.writeFileSync(path.join(installDir, 'version'), version, 'utf-8');

    emitProgress(opts.onProgress, {
      phase: 'file-written',
      message: `Server binary ready: ${exePath} (version ${version})`,
      filePath: exePath,
    });
    return { kind: 'success', success: true, serverPath: exePath, source: 'download', version, warnings };
  } catch (err: unknown) {
    return {
      kind: 'failure',
      success: false,
      url,
      warnings,
      error: asError(err),
    };
  }
}
