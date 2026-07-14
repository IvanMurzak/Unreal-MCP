// The shared machine credential store at `~/.ai-game-dev/credentials.json` — the
// once-per-machine home for the ai-game.dev account credential (mcp-authorize
// design 06 / 09, D12). Engines, CLIs, and the local server all read the SAME
// store, so sign-in happens once per machine, never per project.
//
// This is the TypeScript peer of the shared C# `MachineCredentialStore`
// (McpPlugin/src/AgentConfig/MachineCredentialStore.cs). The on-disk contract
// MUST stay byte-compatible with it so the .NET sidecar can read what the CLI
// writes and vice versa:
//   • path        — `<home>/.ai-game-dev/credentials.json`.
//   • JSON schema — camelCase `{ version, accessToken, refreshToken, expiresAt,
//                   serverTarget, subject }`; null fields omitted; unknown fields
//                   ignored on read (forwards-compatible).
//   • at rest     — POSIX: plaintext with 0600 perms inside a 0700 directory.
//                   Windows: DPAPI-encrypted with the current user's key
//                   (`CryptProtectData`, CurrentUser scope, no entropy) so the
//                   on-disk bytes are never plaintext.
//
// Credentials NEVER go into a project file / VCS (that is the whole point — the
// `login --path` project-`.env` override is a separate, gitignored path).
//
// Library-safe: the store touches the filesystem (and, on Windows, spawns
// PowerShell as a stateless DPAPI oracle) but callers own error shaping.

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { execFile } from 'child_process';

/** Directory name under the user home that holds the store. */
export const STORE_DIR_NAME = '.ai-game-dev';

/** File name of the secret credential document. */
export const CREDENTIALS_FILE_NAME = 'credentials.json';

/** Current schema version of the persisted credential document. */
export const CREDENTIALS_VERSION = 1;

/**
 * The secret credential material persisted per machine. Mirrors the C#
 * `MachineCredentials` DTO field-for-field (camelCase JSON on the wire).
 */
export interface MachineCredentials {
  /** Schema version of the persisted document (currently {@link CREDENTIALS_VERSION}). */
  version: number;
  /** The current short-lived JWT access token (MCP audience). */
  accessToken?: string;
  /** The rotating refresh token used to mint a new access token before {@link expiresAt}. */
  refreshToken?: string;
  /** Absolute expiry of {@link accessToken} as an ISO-8601 string (C# `DateTimeOffset`). */
  expiresAt?: string;
  /** The server target the credential was issued for (hosted `https://ai-game.dev` or a local URL). */
  serverTarget?: string;
  /** The account id (`sub`) the credential resolves to. Audit/diagnostic only. */
  subject?: string;
}

function isWindows(): boolean {
  return process.platform === 'win32';
}

/**
 * The shared machine credential store. `baseDir` defaults to `~/.ai-game-dev`;
 * the override exists for tests and for the `--path` per-project store.
 */
export class MachineCredentialStore {
  private readonly baseDir: string;

  constructor(baseDir?: string) {
    this.baseDir = baseDir ?? path.join(os.homedir(), STORE_DIR_NAME);
  }

  /** Absolute path of the store directory. */
  get baseDirectory(): string {
    return this.baseDir;
  }

  /** Absolute path of the secret credential file. */
  get credentialsPath(): string {
    return path.join(this.baseDir, CREDENTIALS_FILE_NAME);
  }

  /** True when a credential file exists in the store. */
  exists(): boolean {
    return fs.existsSync(this.credentialsPath);
  }

  /**
   * Encrypt (Windows) / restrict (POSIX) and write `credentials` to the store,
   * creating the store directory with owner-only permissions if needed.
   */
  async write(credentials: MachineCredentials): Promise<void> {
    const json = serializeCredentials(credentials);
    const plaintext = Buffer.from(json, 'utf-8');

    fs.mkdirSync(this.baseDir, { recursive: true });
    setOwnerOnly(this.baseDir, 0o700);

    const bytes = isWindows() ? await dpapiProtect(plaintext) : plaintext;
    fs.writeFileSync(this.credentialsPath, bytes);
    setOwnerOnly(this.credentialsPath, 0o600);
  }

  /** Read and decrypt the stored credentials, or `null` when none are present. */
  async read(): Promise<MachineCredentials | null> {
    if (!fs.existsSync(this.credentialsPath)) return null;

    const raw = fs.readFileSync(this.credentialsPath);
    if (raw.length === 0) return null;

    const plaintext = isWindows() ? await dpapiUnprotect(raw) : raw;
    const json = plaintext.toString('utf-8');
    if (json.trim().length === 0) return null;

    return JSON.parse(json) as MachineCredentials;
  }

  /** Delete the stored credentials (sign-out). No-op when none exist. */
  delete(): void {
    if (fs.existsSync(this.credentialsPath)) {
      fs.rmSync(this.credentialsPath, { force: true });
    }
  }
}

/**
 * Serialize to the camelCase JSON shape the C# store reads. Null/undefined
 * fields are omitted (matches `DefaultIgnoreCondition.WhenWritingNull`); the
 * key order mirrors the C# DTO for a stable on-disk document.
 */
function serializeCredentials(c: MachineCredentials): string {
  const obj: Record<string, unknown> = { version: c.version ?? CREDENTIALS_VERSION };
  if (c.accessToken != null) obj.accessToken = c.accessToken;
  if (c.refreshToken != null) obj.refreshToken = c.refreshToken;
  if (c.expiresAt != null) obj.expiresAt = c.expiresAt;
  if (c.serverTarget != null) obj.serverTarget = c.serverTarget;
  if (c.subject != null) obj.subject = c.subject;
  return JSON.stringify(obj, null, 2);
}

/** chmod best-effort; a no-op on Windows (ACL-based, DPAPI already protects). */
function setOwnerOnly(target: string, mode: number): void {
  if (isWindows()) return;
  try {
    fs.chmodSync(target, mode);
  } catch {
    /* best-effort — a filesystem that rejects chmod must not fail the write */
  }
}

// ── Windows DPAPI oracle ───────────────────────────────────────────────────
// Node has no native DPAPI, so shell out to PowerShell's ProtectedData (the thin
// managed wrapper over CryptProtectData/CryptUnprotectData). `CurrentUser` scope
// + null entropy makes the blob byte-compatible with the C# store's
// `CryptProtectData(pOptionalEntropy = Zero)`. PowerShell is used as a stateless
// crypto oracle only — all file I/O and directory perms stay in Node above.
// Plaintext bytes go in base64 over stdin; the base64 result comes back on stdout.

const DPAPI_PROTECT_PS = [
  "$ErrorActionPreference='Stop'",
  'Add-Type -AssemblyName System.Security',
  '$b64=[Console]::In.ReadToEnd()',
  '$bytes=[System.Convert]::FromBase64String($b64)',
  '$out=[System.Security.Cryptography.ProtectedData]::Protect($bytes,$null,[System.Security.Cryptography.DataProtectionScope]::CurrentUser)',
  '[Console]::Out.Write([System.Convert]::ToBase64String($out))',
].join('\n');

const DPAPI_UNPROTECT_PS = [
  "$ErrorActionPreference='Stop'",
  'Add-Type -AssemblyName System.Security',
  '$b64=[Console]::In.ReadToEnd()',
  '$bytes=[System.Convert]::FromBase64String($b64)',
  '$out=[System.Security.Cryptography.ProtectedData]::Unprotect($bytes,$null,[System.Security.Cryptography.DataProtectionScope]::CurrentUser)',
  '[Console]::Out.Write([System.Convert]::ToBase64String($out))',
].join('\n');

function runDpapi(script: string, input: Buffer): Promise<Buffer> {
  return new Promise<Buffer>((resolve, reject) => {
    const child = execFile(
      'powershell',
      ['-NoProfile', '-NonInteractive', '-Command', script],
      { windowsHide: true, maxBuffer: 16 * 1024 * 1024 },
      (err, stdout) => {
        if (err) {
          reject(err);
          return;
        }
        try {
          resolve(Buffer.from(String(stdout).trim(), 'base64'));
        } catch (e) {
          reject(e as Error);
        }
      },
    );
    // A spawn failure (e.g. powershell not on PATH) surfaces via the execFile
    // callback's `err`; but writing to a broken/closed stdin ALSO emits an
    // `'error'` on the stdin stream, which throws (crashes the process) unless
    // handled. Route it to `reject` — harmless if the callback already settled
    // the promise. (Matches the `child.on('error', reject)` spawn convention in
    // unreal-build.ts / open.ts.)
    const stdin = child.stdin;
    if (stdin) {
      stdin.on('error', reject);
      stdin.end(input.toString('base64'));
    }
  });
}

function dpapiProtect(plaintext: Buffer): Promise<Buffer> {
  return runDpapi(DPAPI_PROTECT_PS, plaintext);
}

function dpapiUnprotect(protectedBytes: Buffer): Promise<Buffer> {
  return runDpapi(DPAPI_UNPROTECT_PS, protectedBytes);
}
