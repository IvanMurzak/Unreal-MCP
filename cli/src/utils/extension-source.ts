// Pure helpers that resolve WHERE an extension's plugin files come from when
// `install-extension` materializes them. This is the PLUGGABLE INSTALL-SOURCE
// RESOLVER seam the task requires: callers ask `resolveInstallSource(...)` for a
// discriminated `InstallSource` and never branch on the channel themselves, so a
// future Fab (Epic marketplace) channel slots in as a new `kind` here + a new
// materializer branch in `install-extension.ts` — WITHOUT touching the command or
// the library's public signature.
//
// Mirrors godot-cli's `addon-source.ts` trusted-source pattern (github.com only,
// `releases/download/v<version>/<asset>`). No side effects; unit-testable.

/** GitHub-release host extension zips are downloaded from. NOTHING else is trusted. */
export const TRUSTED_DOWNLOAD_HOST = 'github.com';

/**
 * Where an extension's plugin files are materialized from. A discriminated union
 * so the installer's `switch (source.kind)` stays exhaustive and a new channel is
 * a compile-checked addition.
 *
 * - `local` — a directory on disk (the `--source <dir>` offline / CI / dev path).
 * - `github-release` — the extension's plugin zip from its GitHub release.
 * - (reserved) `fab` — the Epic Fab marketplace channel; add it here + a
 *   materializer branch when it exists, no caller change needed.
 */
export type InstallSource =
  | { readonly kind: 'local'; readonly dir: string }
  | { readonly kind: 'github-release'; readonly url: string; readonly version: string; readonly repo: string };

/** The channel discriminant, exported for the public result type. */
export type InstallSourceKind = InstallSource['kind'];

/** Drop a single leading `v`/`V` from a version string. Pure. */
export function stripLeadingV(version: string): string {
  const v = (version ?? '').trim();
  return /^v/i.test(v) ? v.slice(1) : v;
}

/**
 * The git release TAG for a version: the version with a leading `v` (`1.0.0` →
 * `v1.0.0`). An already-`v`-prefixed input passes through unchanged so a caller
 * cannot double-prefix. Pure.
 */
export function releaseTag(version: string): string {
  const v = (version ?? '').trim();
  return /^v/i.test(v) ? v : `v${v}`;
}

/**
 * The release-asset name for an extension plugin: `<pluginName>-<version>.zip`
 * (bare version, no leading `v` — the `v` only appears in the tag). Mirrors the
 * Godot `godot-mcp-addon-<version>.zip` convention, parameterized by plugin name
 * so each extension repo names its own asset. Pure.
 */
export function extensionAssetName(pluginName: string, version: string): string {
  return `${pluginName}-${stripLeadingV(version)}.zip`;
}

/**
 * The download URL for an extension version's plugin zip:
 * `https://github.com/<repo>/releases/download/v<version>/<pluginName>-<version>.zip`.
 * The host is always `github.com` by construction; `assertTrustedDownloadUrl`
 * re-checks any caller-supplied URL before a network call. Pure string build.
 */
export function extensionDownloadUrl(repo: string, pluginName: string, version: string): string {
  const bare = stripLeadingV(version);
  return `https://${TRUSTED_DOWNLOAD_HOST}/${repo}/releases/download/${releaseTag(version)}/${extensionAssetName(pluginName, bare)}`;
}

/**
 * Fail-closed host-trust check: throw unless `url` is an HTTPS URL whose host is
 * EXACTLY `github.com` (no subdomains, no `github.com.evil.test`, no http). The
 * security boundary the task requires. Returns the parsed URL on success; the
 * caller turns the throw into a structured failure so nothing escapes the public
 * boundary.
 */
export function assertTrustedDownloadUrl(url: string): URL {
  let parsed: URL;
  try {
    parsed = new URL(url);
  } catch {
    throw new Error(`Refusing to download extension: malformed URL "${url}".`);
  }
  if (parsed.protocol !== 'https:') {
    throw new Error(
      `Refusing to download extension: only https is allowed, got "${parsed.protocol}" (${url}).`,
    );
  }
  if (parsed.hostname.toLowerCase() !== TRUSTED_DOWNLOAD_HOST) {
    throw new Error(
      `Refusing to download extension from untrusted host "${parsed.hostname}". Only ${TRUSTED_DOWNLOAD_HOST} is trusted.`,
    );
  }
  return parsed;
}

/** Inputs the resolver needs to pick + build an `InstallSource`. */
export interface ResolveInstallSourceInput {
  /** An explicit local `--source <dir>` (wins over every download channel when non-empty). */
  readonly source?: string;
  /** `owner/repo` for the GitHub-release channel (from the catalog descriptor). */
  readonly repo: string | null;
  /** The plugin name (asset basename). */
  readonly pluginName: string;
  /** The version to download (catalog pin or `--version` override). */
  readonly version: string | null;
}

/**
 * Resolve the install source for an extension. This is the single decision point
 * for the install channel:
 *
 *  1. An explicit `--source <dir>` always wins → `{ kind: 'local' }`.
 *  2. Otherwise, when a `repo` + `version` are known → `{ kind: 'github-release' }`
 *     with a github.com-only URL (re-checked by `assertTrustedDownloadUrl` before
 *     the fetch).
 *  3. Otherwise throw a clear error explaining what is missing.
 *
 * A future Fab channel becomes a third branch here; callers are unaffected. Pure
 * (throws on the unhappy path; the library converts it to a structured failure).
 */
export function resolveInstallSource(input: ResolveInstallSourceInput): InstallSource {
  const source = (input.source ?? '').trim();
  if (source !== '') {
    return { kind: 'local', dir: source };
  }

  const version = (input.version ?? '').trim();
  if (input.repo && version !== '') {
    const url = extensionDownloadUrl(input.repo, input.pluginName, version);
    assertTrustedDownloadUrl(url); // fail-closed: github.com + https only.
    return { kind: 'github-release', url, version, repo: input.repo };
  }

  // Neither a local dir nor a resolvable release.
  if (!input.repo) {
    throw new Error(
      `No download source for "${input.pluginName}": the catalog entry has no release repo. ` +
        'Pass --source <dir> to install from a local copy.',
    );
  }
  throw new Error(
    `No version to download for "${input.pluginName}". Pass --version <x.y.z> ` +
      '(or set a catalog pin), or --source <dir> for a local install.',
  );
}
