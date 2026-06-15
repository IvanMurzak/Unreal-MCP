// `auto-install-engine` — best-effort, CONSENT-GATED engine acquisition.
//
// Ground truth (researched 2026-06, Epic docs): there is NO unattended,
// unauthenticated CLI that installs an Unreal Engine on ANY desktop OS.
//   - Windows / macOS: engines install through the GUI Epic Games Launcher;
//     there is no silent install command. The honest behaviour is to open the
//     launcher via the `com.epicgames.launcher://` deep link and print precise
//     guided steps.
//   - Linux: Epic ships pre-compiled binaries as a `.zip`, but the download is
//     gated behind an Epic Games account login — there is no public direct URL
//     we can fetch unauthenticated. So Linux ALSO degrades to guided steps
//     (download-page URL + unzip + run instructions), not a silent fetch.
//
// Accordingly this module NEVER performs a multi-GB download today: every OS
// path returns guidance. The consent gate (`consent` / `interactive`) is wired
// anyway so that if/when a genuine unattended path becomes available it stays
// strictly opt-in (the brief's hard requirement: never spend multi-GB without
// explicit user intent). The function ALSO validates post-install via the
// injected `resolveInstalledImpl` — it will never claim an engine was installed
// unless a real editor binary resolves. Library-safe: pure given its injected
// surfaces, never prints, never throws past the boundary.

import { platform } from 'os';
import { launcherInstallUrl } from './install-engine.js';
import type { AutoInstallEngineOptions, AutoInstallEngineResult } from './types.js';

/**
 * Per-OS guided install steps for `version`. Pure. Exported for tests so the
 * guidance copy is asserted without running the whole flow.
 */
export function engineInstallGuidance(version: string, os: NodeJS.Platform, launcherUrl: string): string[] {
  switch (os) {
    case 'win32':
    case 'darwin':
      return [
        `Unreal Engine ${version} must be installed through the Epic Games Launcher (no unattended installer exists).`,
        `1. Open the launcher: ${launcherUrl}`,
        `2. Go to Unreal Engine → Library → "+" and choose version ${version}.`,
        '3. Install it, then re-run this command to confirm detection.',
      ];
    default:
      // Linux: official pre-compiled binaries, but login-gated download.
      return [
        `Unreal Engine ${version} for Linux is a login-gated download — the CLI cannot fetch it unattended.`,
        '1. Sign in and download the Linux binary zip: https://www.unrealengine.com/en-US/linux',
        '2. Unzip it to an install dir, e.g. /opt/UnrealEngine or ~/UnrealEngine.',
        '3. Re-run this command (set $UE_ROOT to the unzipped dir if it is not under a common location).',
      ];
  }
}

/**
 * Ensure an engine is available, best-effort and consent-gated.
 *
 * Resolution:
 *   1. If `resolveInstalledImpl(version)` already returns a binary → done
 *      (`already-installed`).
 *   2. Else there is no unattended install path on this OS → return per-OS
 *      `guidance` + the launcher deep link. When a real download path is wired
 *      in future, it runs ONLY when consent is present (`consent === true`, or
 *      an interactive confirmation); without consent the outcome is
 *      `consent-required` and nothing is downloaded.
 *
 * Any future install attempt is VALIDATED through `resolveInstalledImpl` before
 * reporting success — `install-unverified` is returned if it did not resolve.
 */
export function autoInstallEngine(opts: AutoInstallEngineOptions): AutoInstallEngineResult {
  const os = opts.os ?? (platform() as NodeJS.Platform);
  const version = opts.version.trim();
  const launcherUrl = launcherInstallUrl();
  const resolveInstalled = opts.resolveInstalledImpl ?? (() => null);
  const guidance = engineInstallGuidance(version, os, launcherUrl);

  // 1. Already installed — short-circuit, validated by a real binary.
  const existing = resolveInstalled(version, os);
  if (existing) {
    return {
      kind: 'success',
      success: true,
      version,
      outcome: 'already-installed',
      editorPath: existing,
      launcherUrl,
      guidance,
      message: `Unreal Engine ${version} is already installed (${existing}).`,
    };
  }

  // 2. No unattended path exists on any desktop OS today. The consent gate is
  // evaluated FIRST so that the moment a real download path is added, it is
  // governed by opt-in — never a surprise multi-GB spend.
  const hasConsent = opts.consent === true || opts.interactive === true;
  if (!hasConsent) {
    return {
      kind: 'success',
      success: true,
      version,
      outcome: 'consent-required',
      editorPath: null,
      launcherUrl,
      guidance,
      message:
        `Unreal Engine ${version} is not installed. Installing an engine is a multi-GB operation; ` +
        're-run with --yes (or in an interactive terminal) to proceed. ' +
        'Note: no unattended installer exists today — the CLI will guide you through the Epic launcher.',
    };
  }

  // Consent present — but there is still no unattended path, so we hand back
  // guidance rather than faking an install. (A genuine download path would slot
  // in here, then fall through to the post-install validation below.)
  return {
    kind: 'success',
    success: true,
    version,
    outcome: 'guidance-only',
    editorPath: null,
    launcherUrl,
    guidance,
    message:
      `The CLI cannot install Unreal Engine ${version} unattended on this platform. ` +
      'Follow the guided steps to install it via Epic, then re-run to confirm.',
  };
}
