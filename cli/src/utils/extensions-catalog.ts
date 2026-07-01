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
  {
    extensionId: 'com.ivanmurzak.unreal-ai-enhanced-input',
    name: 'EnhancedInput Tools',
    description:
      "AI tools for Unreal's Enhanced Input system (list/inspect Input Actions & Mapping Contexts, create actions/contexts, add key mappings).",
    pluginName: 'UnrealAIEnhancedInput',
    repo: 'IvanMurzak/Unreal-AI-EnhancedInput',
    version: '0.1.0',
    minCoreVersion: '0.5.0',
    enginePlugins: ['EnhancedInput'],
    tools: [
      {
        name: 'enhanced-input-list',
        description:
          'Lists every Enhanced Input asset in the project via the Asset Registry, without loading any of them: Input Actions (UInputAction) and Mapping Contexts (UInputMappingContext). Optionally filter by a content-path prefix.',
      },
      {
        name: 'enhanced-input-get',
        description:
          'Inspects a single Enhanced Input asset (read-only): for a UInputAction reports its value type; for a UInputMappingContext reports its key -> action mappings.',
      },
      {
        name: 'enhanced-input-action-create',
        description:
          'Creates a UInputAction asset (an Enhanced Input action) at a content path, with an optional value type (Boolean | Axis1D | Axis2D | Axis3D). Optionally saves it to disk.',
      },
      {
        name: 'enhanced-input-context-create',
        description:
          'Creates a UInputMappingContext asset (an Enhanced Input mapping context) at a content path. Optionally saves it to disk.',
      },
      {
        name: 'enhanced-input-add-mapping',
        description:
          'Adds a key -> action mapping to a UInputMappingContext, binding an FKey to a UInputAction. Optionally saves it to disk.',
      },
    ],
  },
  {
    extensionId: 'com.ivanmurzak.unreal-ai-pcg',
    name: 'PCG Tools',
    description:
      "AI tools for Unreal's Procedural Content Generation (PCG) framework (list/inspect graphs, add & inspect PCG components, trigger generation).",
    pluginName: 'UnrealAIPCG',
    repo: 'IvanMurzak/Unreal-AI-PCG',
    version: '0.1.0',
    minCoreVersion: '0.5.0',
    enginePlugins: ['PCG'],
    tools: [
      {
        name: 'pcg-list-graphs',
        description:
          'Lists every PCG graph (UPCGGraph) asset in the project via the Asset Registry, without loading any of them. Optionally filter by a content-path prefix.',
      },
      {
        name: 'pcg-get-graph',
        description: 'Inspects a single PCG graph asset (read-only) and reports its nodes.',
      },
      {
        name: 'pcg-add-component',
        description:
          'Adds a UPCGComponent to a named actor in the active editor world, optionally bound to a PCG graph asset.',
      },
      {
        name: 'pcg-get-component',
        description:
          'Inspects the UPCGComponent on a named actor in the active editor world (read-only): its bound graph and generation state.',
      },
      {
        name: 'pcg-generate',
        description:
          'Triggers PCG generation (refresh) on the UPCGComponent of a named actor in the active editor world.',
      },
    ],
  },
  {
    extensionId: 'com.ivanmurzak.unreal-ai-gas',
    name: 'GAS Tools',
    description:
      "AI tools for Unreal's Gameplay Ability System (list abilities/effects/attribute sets, inspect an ability, grant an ability to an actor).",
    pluginName: 'UnrealAIGAS',
    repo: 'IvanMurzak/Unreal-AI-GAS',
    version: '0.1.0',
    minCoreVersion: '0.5.0',
    enginePlugins: ['GameplayAbilities'],
    tools: [
      {
        name: 'gas-list-abilities',
        description:
          'Lists every Gameplay Ability class (UGameplayAbility subclass — native C++ and Blueprint) known to the project via the Asset Registry, without loading any of them. Optionally filter by a class-path prefix.',
      },
      {
        name: 'gas-list-effects',
        description:
          'Lists every Gameplay Effect class (UGameplayEffect subclass — native C++ and Blueprint) known to the project via the Asset Registry, without loading any of them. Optionally filter by a class-path prefix.',
      },
      {
        name: 'gas-list-attribute-sets',
        description:
          'Lists every Attribute Set class (UAttributeSet subclass — native C++ and Blueprint) known to the project via the Asset Registry, without loading any of them. Optionally filter by a class-path prefix.',
      },
      {
        name: 'gas-inspect-ability',
        description:
          'Inspects a single Gameplay Ability class (read-only) by loading its class default object and reporting its configuration: gameplay tags, cost/cooldown Gameplay Effect classes, and instancing policy.',
      },
      {
        name: 'gas-grant-ability',
        description:
          "Grants a Gameplay Ability to a named actor's AbilitySystemComponent in the active editor world (the actor must already own an AbilitySystemComponent).",
      },
    ],
  },
  {
    extensionId: 'com.ivanmurzak.unreal-ai-landscape',
    name: 'Landscape Tools',
    description: "AI tools for Unreal's Landscape + Water systems (inspect landscapes, water bodies & zones).",
    pluginName: 'UnrealAILandscape',
    repo: 'IvanMurzak/Unreal-AI-Landscape',
    version: '0.1.0',
    minCoreVersion: '0.5.0',
    enginePlugins: ['Water'],
    tools: [
      {
        name: 'landscape-list-actors',
        description:
          'Lists every Landscape actor / proxy in the active editor world (read-only), reporting class, whether it is the primary ALandscape (vs a streaming proxy), and component count.',
      },
      {
        name: 'landscape-get-actor',
        description:
          'Inspects a single Landscape actor by name (read-only): section layout (componentSizeQuads / subsectionSizeQuads / numSubsections), component count, landscape material, and paint layers.',
      },
      {
        name: 'landscape-list-water-bodies',
        description:
          'Lists every Water body actor in the active editor world (read-only), reporting its type (River / Lake / Ocean / Custom) and spline-point count.',
      },
      {
        name: 'landscape-get-water-body',
        description:
          'Inspects a single Water body by name (read-only): type, spline-point count, water material, and owning water zone.',
      },
      {
        name: 'landscape-list-water-zones',
        description:
          'Lists every Water zone actor in the active editor world (read-only), reporting world location and 2D zone extent.',
      },
    ],
  },
  {
    extensionId: 'com.ivanmurzak.unreal-ai-control-rig',
    name: 'ControlRig Tools',
    description:
      "AI tools for Unreal's Control Rig (list/inspect rig blueprints, controls, bones, and actor components).",
    pluginName: 'UnrealAIControlRig',
    repo: 'IvanMurzak/Unreal-AI-ControlRig',
    version: '0.1.1',
    minCoreVersion: '0.5.0',
    enginePlugins: ['ControlRig'],
    tools: [
      {
        name: 'control-rig-list',
        description:
          'Lists every Control Rig blueprint (UControlRigBlueprint) asset in the project via the Asset Registry, without loading any of them. Optionally filter by a content-path prefix. Returns { count, controlRigs:[{ name, path }] }.',
      },
      {
        name: 'control-rig-get',
        description:
          'Inspects a single Control Rig blueprint asset (read-only) and reports its rig element hierarchy. Returns { path, name, boneCount, nullCount, controlCount, curveCount, elementCount, elements:[{ name, type }] }.',
      },
      {
        name: 'control-rig-list-controls',
        description:
          'Lists the control elements of a Control Rig blueprint (read-only), each with its control type (e.g. Float, Transform, Bool). Returns { path, controlCount, controls:[{ name, controlType }] }.',
      },
      {
        name: 'control-rig-list-bones',
        description:
          'Lists the bone elements of a Control Rig blueprint (read-only), each with its immediate parent bone (empty for a root). Returns { path, boneCount, bones:[{ name, parent }] }.',
      },
      {
        name: 'control-rig-get-component',
        description:
          'Inspects the Control Rig component of an actor in the active editor world (read-only). Looks the actor up by its label. Returns { actorName, componentName, controlRigClass, hasControlRig } (controlRigClass is the bound rig instance\'s class, or "None" when no rig is bound) or a defensive error if the actor or component is not found.',
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
