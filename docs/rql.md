# RQL in Unreal-MCP

The editor's data layer is RQL (Reflection Query Language, [IvanMurzak/RQL](https://github.com/IvanMurzak/RQL)): one
query language instead of one tool per operation, as in Unity-MCP 1.0. Agents find actors, components and assets, read
members, filter, aggregate, and change them in one call:

```
actors | where Label contains "Enemy" | select $ref Label Location
actors | get Components | where $ is PointLightComponent | set Intensity = Intensity * 2
editor | call SpawnActor("PointLight", "Lamp", "(0, 0, 300)")
```

## Design

RQL's contract is the QueryPlan, not the text (RQL ADR-0002), and the C++ runtime has no text parser by design
(ADR-0003: "the sidecar parses text with the .NET parser and ships the plan over IPC; the C++ executor validates and
executes plans"). Unreal-MCP follows that split exactly:

```
agent ──query text──▶ unreal-mcp-bridge (sidecar, .NET)          UnrealMCP plugin (C++, game thread)
                      Rql.Parser: text → canonical QueryPlan ──IPC tool-call {query, plan}──▶ rql::Engine.execute(plan)
                      AgentFormat: QueryResult → compact rows ◀──IPC tool-response: QueryResult JSON── over live UObjects
```

- **Sidecar** (`bridge/src/Tools/RqlBridge.cs`, NuGet `com.IvanMurzak.Rql` 0.1.1): for `rql-query` / `rql-mutate` it
  parses `query` with `RqlParser` and adds `PlanCodec.Serialize(plan)` as the `plan` argument (a syntax error is
  answered without a round trip). The plugin answers with the QueryResult JSON; the sidecar renders it with
  `AgentFormat.Render(…, Table)` — byte-for-byte the agent output of Unity-MCP's rql tools. `rql-schema` answers with
  schema JSON the sidecar renders in RQL's compact schema lines.
- **Plugin** (module `UnrealMcpRql`, `UnrealMCP/Source/UnrealMcpRql/`): the vendored RQL C++ runtime
  (`Source/ThirdParty/RQL/`, unmodified) plus the **Unreal provider** the RQL roadmap describes ("`UClass`/`FProperty`,
  `TArray`/`TMap` adapters, `IsValid` liveness"), built on the runtime's provider seam (`TypeDescriptor` /
  `IListAdapter` / `IMapAdapter`) without changing RQL.
- **Why a separate module**: the runtime reports failures with C++ exceptions internally (its public API is
  `noexcept`), so the module sets `bEnableExceptions`; it is RTTI-free, so UE's `/GR-` / `-fno-rtti` is fine. Exceptions
  never cross a UE frame: UFUNCTIONs report failures through `UnrealMcpRql::Fail`, and RQL types never appear in a
  public header. The vendored sources keep file-local helpers, so the module builds without unity and without PCH;
  each `Private/Vendor/UnrealMcpRqlVendor_<file>.cpp` compiles one RQL source between `THIRD_PARTY_INCLUDES_START/END`.
- **License**: RQL is PolyForm Shield 1.0.0 (same owner; RQL ADR-0007). Its `LICENSE.md` ships next to the vendored
  sources and in the sidecar package; `UnrealMCP/ThirdPartyNotices.txt` lists both.

### The Unreal provider (`UnrealMcpRqlTypes`)

- **Types** are built at runtime from reflection, keyed by the `UClass` / `UScriptStruct` / `UEnum` pointer, and filled
  on first use (a query touches a few of the thousands of classes). Class names are the reflected names (`PointLight`,
  `StaticMeshComponent`, `BP_Enemy_C`, structs `Vector`, enums `EComponentMobility`); `rql-schema` accepts the C++
  spellings too (`APointLight`, `FVector`).
- **Members**: class properties that are EditAnywhere / VisibleAnywhere / BlueprintVisible (writable unless `EditConst`
  or BlueprintReadOnly-only), every struct property, `TArray` / `TSet` / `TMap` as lists and maps, object references
  (instanced sub-objects are owning, others references), soft references as paths. Writes go through the editor's
  `PreEditChange` / `PostEditChangeProperty`, as an edit in the Details panel does (components refresh, Blueprint
  actors rerun their construction script).
- **Computed members** (core module): every object `Id`, `Name`, `Path`; Actor `Label`, `Location`, `Rotation`,
  `Scale`, `Components`, `Children`, `Parent` (writable: attach / detach), `Folder`, `Level`, `HiddenInEditor`;
  component `Owner`; Level `Map`, `Package`, `IsPersistent`, `IsCurrent` (writable), `IsVisible`, `Actors`; World
  `Map`, `Levels`, `PersistentLevel`, `CurrentLevel`; MaterialInterface `Scalars` / `Vectors` / `Textures` (parameter
  maps, writable on material instances).
- **Methods** (`call`): Actor `Destroy()`, `Duplicate(Offset)`, `AddComponent(ClassName, Name)`; component
  `Destroy()`; every parameterless `CallInEditor` function (the Details panel's buttons); every UFUNCTION of an API class (the `editor` operations, extension
  operations). Parameters and results convert like members; UHT's recorded C++ defaults become RQL defaults.
- **Identity**: `$ref` is `Type#<Id>`, the `Id` of the object's slot in the object array plus that slot's serial number
  (editor-session ids, like Unity's instance ids); `ref(…)` resolves them directly, and a ref to a destroyed object never
  resolves to a new object that reuses the slot. Every object value carries a weak pointer, so destroyed (garbage)
  objects read as null and are never dereferenced.
- **References from text**: an object parameter or member accepts what an edit field accepts — an asset path, a class
  name or path for class references, an actor label / name / path for actors (`FUnrealMcpObjectRef::ResolveReference`,
  the same class resolver as `blueprint-*`).
- **Math structs** (`Vector`, `Vector2D`, `Vector4`, `Rotator`, `Quat`, `LinearColor`, `Color`, `IntPoint`,
  `IntVector`) have RQL type adapters: they print as `(x, y, z)` and accept `"(1, 2, 3)"`, `[1, 2, 3]`, `{X: 1, …}`;
  colors also `"#ff8800"`.
- **Per-call session**: each tool call builds a fresh engine and type cache (cheap: descriptors fill lazily). A
  Blueprint compile, hot reload or user-defined struct edit recreates `FProperty` / `UFunction` objects, so no
  descriptor may outlive the call that built it.
- **Mutations** run in one editor transaction (one Ctrl+Z step); owners are `Modify()`d before the first write and
  their packages marked dirty after. Unlike Unity-MCP, `rql-mutate` does **not** save to disk: Unreal edits stay
  unsaved until `editor | call SaveAll()` / `SaveLevel()` / `SaveAsset(…)` (saving in Unreal can raise source-control
  checkouts and Save-As dialogs, which an unattended tool call must not).
- **Function sources**: RQL text lets a host function start a query (`unreal.assets("Material") | …`); the C++ runtime
  starts only at sources and `ref(…)`, so the session evaluates such a call (literal arguments) once and runs the query
  on a source holding its value.

### Sources and functions (core module)

| Source / function | What |
|---|---|
| `actors` | every actor of the editor world's loaded levels |
| `levels`, `world` | the loaded levels, the editor world |
| `selection` | the selected actors (`editor | set Selection = [...]` writes it) |
| `editor` | editor state (`IsPlaying`, `IsPaused` — writable, `Selection`, `World`, `CurrentLevel`) and operations: `SpawnActor`, `OpenLevel`, `NewLevel`, `SaveLevel`, `SaveAll`, `UnloadSublevel`, `CreateAsset`, `CopyAsset`, `MoveAsset`, `DeleteAsset`, `CreateFolder`, `RefreshAssets`, `CreateMaterialInstance`, `ImportAsset`, `SaveAsset`, `ClearConsole` |
| `console` | editor log entries (`Sequence`, `Time`, `Verbosity`, `Category`, `Message`) |
| `unreal.find(label)` | an actor by label, name or path |
| `unreal.asset(path)`, `unreal.assets(class, path?)` | one asset; assets of a class (at most 200, loaded as read) |
| `unreal.classes(base?, filter?)` | classes deriving from a base |

## Tools

| Before | Now |
|---|---|
| `actor-find`, `actor-component-get`, `object-get-data`, `level-get-data` | `rql-query` on `actors`, `… | get Components`, `ref("Type#id")`, `unreal.asset(path)`, `levels | get Actors` |
| `actor-modify`, `actor-component-modify`, `object-modify` | `rql-mutate` `set` (transform members, any reflected property, struct paths `Location.Z`) |
| `actor-create`, `actor-destroy`, `actor-duplicate`, `actor-set-parent` | `editor | call SpawnActor(...)`, `call Destroy()`, `call Duplicate(offset)`, `set Parent = ref(...)` |
| `actor-component-add`, `actor-component-destroy`, `actor-component-list-all` | `call AddComponent(class, name)`, `… | get Components | where $ is X | call Destroy()`, `unreal.classes("ActorComponent")` |
| `asset-find`, `asset-get-data` | `unreal.assets("Material", "/Game/...")`, `unreal.asset(path)` |
| `asset-copy` / `-move` / `-delete` / `-create-folder` / `-refresh` / `-import` | `editor | call CopyAsset / MoveAsset / DeleteAsset / CreateFolder / RefreshAssets / ImportAsset` (and `CreateAsset(path, class)` for any asset class) |
| `asset-material-create`, `-modify`, `-get-data` | `editor | call CreateMaterialInstance(path, parent)`, `Scalars["…"]`, `Vectors["…"]`, `Textures["…"]` |
| `level-create` / `-open` / `-save` / `-set-current` / `-unload-sublevel` / `-list-loaded` | `editor | call NewLevel / OpenLevel / SaveLevel / UnloadSublevel`, `levels` (`set IsCurrent = true`) |
| `editor-application-get-state` / `-set-state` | `editor` `IsPlaying`, `IsPaused` (writable) |
| `editor-selection-get` / `-set` | `selection`, `editor | set Selection = [...]` |
| `console-get-logs` / `console-clear-logs` | `console` source (`where Verbosity == "Error"`, `Sequence > N`), `editor | call ClearConsole()` |
| `reflection-method-find` / `-call` | `rql-schema(type)` (members and methods), `call` on `CallInEditor` functions and the exposed operations |

Kept as tools: `blueprint-*` (Blueprint authoring is Unreal's script surface, like Unity-MCP's `script-*`),
`source-*`, `screenshot-*`, `console-run-command`, `python-execute` (code, not data access), and the system tools `ping`, `unreal-skill-create`,
`unreal-skill-generate`. The runtime (packaged game) surface is unchanged: `ping` only.

`reflection-method-call` could invoke any BlueprintCallable function; RQL `call` is an allowlist (as in Unity-MCP):
`CallInEditor` functions, the operations above and those extensions add.

**Tool data layer.** Unity-MCP also routes every other tool's arguments and results through RQL (McpPlugin
`IToolBinding`). That binding hooks C# `[AiTool]` methods; Unreal's tools are C++ tools proxied from the plugin's
manifest (`ProxyTool`, plain JSON-schema parameters), so there is nothing for it to bind and no `ToolFormat` setting —
the kept tools take simple arguments already.

**Descriptor.** The plugin marks its tools in the manifest: `"rql": "query" | "mutate" | "schema"` tells the sidecar
to parse `query` into `plan` (query / mutate) and to render the answer, and rql-query ships the `unreal-rql` skill (the
guide, the live sources and functions, the guides of installed RQL modules) as `"skills": [{name, description,
markdown}]`. `unreal-skill-generate` writes every shipped skill as `<name>/SKILL.md`; skills never reach `tools/list`.
The skill's name, description and body live in the plugin (`UnrealMcpRqlGuide.h`, the session's skill markdown).

## Extensions

An extension contributes an `IUnrealMcpRqlModule` (`UnrealMCP/Source/UnrealMcpRql/Public/IUnrealMcpRqlModule.h`),
registered as a modular feature. The editor's extension manager runs a module pass in the same rebuild as tool
providers: the Extensions window lists the module's extension (`GetExtensionId`, `GetExtensionVersion`), disabling it
there leaves the module out, and a module coming or going re-registers the rql-* tools (their skill) before the one
manifest push.
Reflection already exposes every type the extension's engine plugin defines, so a module adds what reflection does not:

- sources: `AddObjectSource(name, description, UClass*, getter)`, `AddObjectListSource(name, description, UClass*,
  getter)`, `AddAssetSource(name, description, UClass*)`, `AddClassDefaultsSource(name, description, UClass* base)` —
  the class types the source in `rql-schema`;
- operations: `AddApiSource(name, description, UClass*, "TypeName")` (or `AddApiClass` alone) — every UFUNCTION of the
  class becomes callable (declare them `UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true"))` so
  UHT records default arguments; the doc comment is the description; failures via `UnrealMcpRql::Failed<T>`; object
  parameters are required unless their C++ default is `nullptr`);
- members: `ExposeProperties` (properties reflection hides), `AddJsonMember`, `AddObjectMember`, `AddObjectListMember`;
- `AddGuide(topic, markdown)`, rendered into the `unreal-rql` skill.

`IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FMyRqlModule>, MyModule)` registers the module for the editor module's
lifetime; `UnrealMcpRql::SpawnActor` / `AddComponent` / `CreateAsset` / `EditorWorld` / `PluginVersion` share the core
operations' rules (docs/EXTENSIONS.md).

## Tests

- `UnrealMCP/Source/UnrealMcpEditorTests/Private/UnrealMcpRqlSpec.cpp` (`UnrealMcp.Tools.Rql`): the tools, queries,
  mutations (set, dry run, read-only, permission), components, calls (spawn, add component, duplicate, destroy), refs,
  selection, levels, console, assets, schema. The C++ runtime has no parser, so each case carries the plan Rql.Parser
  produces for its query (in the comment above it).
- `bridge/tests/RqlBridgeTests.cs`: plans, result and schema rendering, the descriptor fields, the proxy wiring, the
  shipped skill file.
- `UnrealMcpExtensionsSpec.cpp` (`UnrealMcp.Extensions`): the module pass (records, disabled set, enabled ids).
- The connection smoke (`scripts/connection_smoke.py`) and the chain battery call `rql-schema` over MCP.

A local Mac run: build a host project with the plugin (`RunUBT.sh <Host>Editor Mac Development -project=…`), then
`UnrealEditor-Cmd <host>.uproject -nullrhi -unattended -ExecCmds="Automation RunTests UnrealMcp.Tools.Rql; Quit"`.
