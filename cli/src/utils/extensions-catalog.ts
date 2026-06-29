// The SHARED, ENGINE-AGNOSTIC extension catalog — the typed list of installable
// Unreal-MCP extensions consumed by `install-extension` (this CLI) and, later, by
// the in-editor extensions panel and the desktop app. It is the Unreal analog of
// godot-cli's `extensions-catalog.ts`, deliberately given the SAME public shape
// (`ExtensionDescriptor` + `findExtension` + `hasVersion`) so unity-mcp-cli /
// godot-cli can converge on one catalog contract.
//
// What is an Unreal-MCP extension? A third-party UE **C++ Editor (or Runtime)
// plugin** that implements `IUnrealMcpToolProvider` (see `docs/EXTENSIONS.md`) and
// contributes MCP tools to the core Unreal-MCP plugin. Unlike a Godot extension
// (a NuGet `<PackageReference>`), it is installed as a **plugin folder** dropped
// into `<project>/Plugins/<pluginName>/`, enabled in the `.uproject`, and compiled
// by UBT — so the descriptor carries plugin-placement metadata (`pluginName`,
// `repo`, `enginePlugins`) rather than a package id.
//
// CATALOG FORMAT (the shape of the shared source-of-truth
// `cli/extensions.catalog.json`, mirroring Godot's
// `addons/godot_mcp/extensions.catalog.json` `{ schemaVersion, extensions[] }`):
//
//   { schemaVersion: 1, extensions: ExtensionDescriptor[] }
//
// This module is the CLI's typed mirror so the published npm package stays
// self-contained (no runtime dependency on a sibling JSON). The JSON is the
// single source of truth; this constant MUST stay value-equivalent to it. The
// parity test `cli/tests/extensions-catalog-parity.test.ts` reads the JSON and
// FAILS the build if this mirror drifts — so adding/changing an extension means
// editing BOTH the JSON and this array (the test enforces it), exactly the
// discipline godot-cli's `extensions-catalog-parity.test.ts` uses.
//
// No top-level side effects; pure data + pure lookups only.

/** Catalog schema version — bump on a breaking shape change. */
export const EXTENSION_CATALOG_SCHEMA_VERSION = 1;

/** One MCP tool an extension contributes — mirrors the JSON `tools[]` entry. */
export interface ExtensionTool {
  readonly name: string;
  readonly description: string;
}

/**
 * One installable extension — the engine-agnostic descriptor.
 *
 * `extensionId` is the INSTALL IDENTITY: the stable, reverse-DNS string an
 * extension returns from `IUnrealMcpToolProvider::GetExtensionId()` (e.g.
 * `"com.ivanmurzak.unreal.niagara"`). It is the primary resolve key.
 */
export interface ExtensionDescriptor {
  /** Stable, unique reverse-DNS id (the provider's `GetExtensionId()`). Primary resolve key. */
  readonly extensionId: string;
  /** Human-readable display name (shown in the extensions UI). */
  readonly name: string;
  readonly description: string;
  /**
   * The UE plugin folder + descriptor basename: the extension installs into
   * `<project>/Plugins/<pluginName>/` and its descriptor is `<pluginName>.uplugin`.
   * Also the name enabled in the `.uproject` `Plugins[]` array.
   */
  readonly pluginName: string;
  /**
   * `owner/repo` the extension's plugin zip is released from on GitHub Releases
   * (the default download channel). `null` for a source-only / Fab-only entry —
   * such an extension can still be installed via `--source <dir>`.
   */
  readonly repo: string | null;
  /** Catalog-pinned version, or `null` for an unpinned (floating) entry. */
  readonly version: string | null;
  /**
   * Minimum core Unreal-MCP plugin version this extension requires
   * (`min-core-version`). `null` = no declared floor. Surfaced as a warning when
   * the installed core plugin is older.
   */
  readonly minCoreVersion: string | null;
  /**
   * Gating engine plugins this extension needs (e.g. `["Niagara"]`) — enabled in
   * the `.uproject` alongside the extension. This is the catalog-declared hint
   * (so the UI can show requirements BEFORE download); the authoritative gating
   * set is unioned at install time with the extension's own materialized
   * `.uplugin` `Plugins[]`.
   */
  readonly enginePlugins: readonly string[];
  readonly tools: readonly ExtensionTool[];
}

/**
 * The extension catalog, the typed mirror of the shared
 * `cli/extensions.catalog.json`. `install-extension <id>` resolves a user-typed
 * id against this list (by `extensionId`, `name`, or `pluginName`); an id absent
 * from it reports "unknown extension" but is still installable via the
 * `--source <dir>` channel. The parity test
 * `cli/tests/extensions-catalog-parity.test.ts` keeps this in lockstep with the
 * JSON source of truth.
 */
export const EXTENSIONS_CATALOG: readonly ExtensionDescriptor[] = [
  {
    extensionId: 'com.ivanmurzak.unreal-ai-niagara',
    name: 'Niagara Tools',
    description: "AI tools for Unreal's Niagara VFX system (list/inspect systems, spawn components).",
    pluginName: 'UnrealAINiagara',
    repo: 'IvanMurzak/Unreal-AI-Niagara',
    version: '0.1.0',
    minCoreVersion: '0.5.0',
    enginePlugins: ['Niagara'],
    tools: [
      {
        name: 'niagara-list-systems',
        description:
          'List every Niagara system (UNiagaraSystem) asset in the project (read-only), optionally filtered by a content-path prefix.',
      },
      {
        name: 'niagara-get-system',
        description: 'Inspect a single Niagara system asset (read-only) and report its emitters.',
      },
      {
        name: 'niagara-spawn-component',
        description: 'Spawn a Niagara component for a system into the active editor world at a location.',
      },
    ],
  },
] as const;

/** True when a descriptor carries a concrete version pin (drives the up-to-date / update decision). */
export function hasVersion(descriptor: ExtensionDescriptor): boolean {
  return descriptor.version !== null && descriptor.version.trim() !== '';
}

/**
 * Resolve a user-supplied `<id>` to a catalog descriptor. Matches (all
 * case-insensitive) by `extensionId` first (the install identity), then by
 * `name`, then by `pluginName`, for convenience. Returns `null` when absent or
 * `id` is empty.
 */
export function findExtension(
  id: string | undefined | null,
  catalog: readonly ExtensionDescriptor[] = EXTENSIONS_CATALOG,
): ExtensionDescriptor | null {
  if (id === undefined || id === null) return null;
  const needle = id.trim().toLowerCase();
  if (needle === '') return null;

  return (
    catalog.find((d) => d.extensionId.toLowerCase() === needle) ??
    catalog.find((d) => d.name.toLowerCase() === needle) ??
    catalog.find((d) => d.pluginName.toLowerCase() === needle) ??
    null
  );
}

/** A clear "unknown extension" message listing what IS installable (or that the catalog is empty). */
export function unknownExtensionMessage(
  id: string,
  catalog: readonly ExtensionDescriptor[] = EXTENSIONS_CATALOG,
): string {
  if (catalog.length === 0) {
    return (
      `Unknown extension "${id}". The Unreal-MCP extension catalog is currently empty ` +
      '(no extension plugins have been published yet), so there is nothing to install by id. ' +
      'Pass --source <dir> to install an extension plugin from a local directory.'
    );
  }
  const available = catalog.map((d) => d.extensionId).join(', ');
  return `Unknown extension "${id}". Available extensions: ${available}. (Or pass --source <dir> for a local install.)`;
}
