// The sign-in rule for the local-server fallback (shared with unity-mcp-cli / godot-cli): a Cloud-mode project on a
// machine that has signed in to ai-game.dev is never switched to a local server. Only a machine that NEVER signed in
// (cli-core MachineCredentialStore state `missing`) may serve the project from a locally started editor; a sign-in
// that cannot be used is reported (NOT_AUTHENTICATED), and an explicit --url / --token is used as is. Library-safe.

import { MachineCredentialStore } from '@baizor/gamedev-cli-core';
import { connectsInCloudMode } from '../utils/config.js';
import { liveOwnedEditorPid } from './session.js';
import type { RunToolFailure } from './types.js';

export type CredentialState = 'ok' | 'missing' | 'unreadable';

export const SIGNED_IN_BUT_UNUSABLE_MESSAGE = 'This machine is signed in to ai-game.dev, but the sign-in cannot be used right now '
  + '(expired, revoked, unreadable, or the credential store is busy). Run `unreal-mcp-cli login` to sign in again, or retry. '
  + 'The project stays in Cloud mode: the CLI does not switch it to a local server.';

/** The note shown when a never-signed-in machine uses the project's local server instead of the cloud. */
export const LOCAL_FALLBACK_NOTE = 'Not signed in to ai-game.dev, so the project\'s local MCP server is used: an editor started now serves it. '
  + 'Run `unreal-mcp-cli login` to use Cloud mode.';

/** `missing` = this machine never signed in to ai-game.dev; anything else = a sign-in exists (usable or not). */
export function machineCredentialState(): CredentialState {
  try {
    return new MachineCredentialStore().readState().status;
  } catch {
    return 'unreadable'; // fail safe: a store we cannot read is never "never signed in"
  }
}

/** The cloud rejected the sign-in. 401 only: a 403 can mean a plan, quota or forbidden tool. */
export function isAuthFailure(failure: RunToolFailure): boolean {
  return failure.reason === 'http-error' && failure.httpStatus === 401;
}

/** A call that uses the project's own Cloud-mode connection: no explicit --url / --token, plugin config in Cloud mode. */
export function usesProjectCloudMode(call: { projectDir?: string; url?: string; token?: string }): boolean {
  return call.url === undefined && call.token === undefined && call.projectDir !== undefined && connectsInCloudMode(call.projectDir);
}

/**
 * Cloud mode on a machine with a sign-in (usable or not): it must stay in Cloud mode — unless the CLI already runs its
 * own editor for the project (`open --headless`, an earlier `exec`), which serves the project's local server.
 */
export function mustStayInCloud(
  call: { projectDir?: string; url?: string; token?: string },
  state: () => CredentialState = machineCredentialState,
  cliEditorPid: (projectDir: string) => number | undefined = liveOwnedEditorPid,
): boolean {
  return usesProjectCloudMode(call) && cliEditorPid(call.projectDir!) === undefined && state() !== 'missing';
}
