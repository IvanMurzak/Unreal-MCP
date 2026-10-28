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

- **Sidecar** (`bridge/src/Tools/RqlBridge.cs`, NuGet `com.IvanMurzak.Rql` 0.3.0-dev.7dd3b29 — a temporary pre-release
  packed from RQL main 7dd3b29 and committed under `bridge/prerelease-packages/` until the cascade release): for `rql-query` / `rql-mutate` it
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
  or BlueprintReadOnly-only; a Blueprint actor's variable that is not Instance Editable is read-only on placed actors,
  since `AActor::ResetPropertiesForConstruction` resets it to the class default on every construction script run), every struct property, `TArray` / `TSet` / `TMap` as lists and maps, object references
  (instanced sub-objects are owning, others references), soft references as paths. Writes go through the editor's
  `PreEditChange` / `PostEditChangeProperty`, as an edit in the Details panel does (components refresh, Blueprint
  actors rerun their construction script).
- **Blueprint actors' components**: an edit of a placed Blueprint actor's component reruns its construction script,
  which rebuilds the construction-script components; the next writes of the same mutation and the receipt follow the
  rebuilt component. A relative-transform write also updates the component's world transform (some components, e.g.
  Niagara, do not re-register on edit).
- **Computed members** (core module): every object `Id`, `Name`, `Path`; Actor `Label`, `Location`, `Rotation`,
  `Scale`, `Components`, `Children`, `Parent` (writable: attach / detach; a refused attach names the reason, e.g. a Static
  actor under a Movable parent), `Folder`, `Layers` (writable through the Layers subsystem, as the Layers panel
  does: missing layers are created, the actor ends up in exactly the listed ones; refused in World Partition levels, which use data layers),
  `Level`, `HiddenInEditor`, `Selected` (writable: adds to / removes from the
  editor selection; neither it nor `HiddenInEditor` marks the level unsaved); Volume `BrushExtent` (local half-size of its
  brush, null without one; writable: rebuilds it as a box, undoable) and `BrushBounds` (world box); component `Owner` (and `Name`: a Blueprint's
  component templates read under their variable name, without the `_GEN_VARIABLE` suffix); Level `Map`, `Package`, `IsPersistent`, `IsCurrent` (writable), `IsVisible` (writable: the Levels panel's eye), `Actors`; World
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
  the same class resolver as `blueprint-*`). An asset reference that resolves to nothing suggests up to three asset paths
  of that class with a matching name (`no StaticMesh 'Cube' (did you mean /Engine/BasicShapes/Cube, ...?)`).
- **Math structs** (`Vector`, `Vector2D`, `Vector4`, `Rotator`, `Quat`, `LinearColor`, `Color`, `IntPoint`,
  `IntVector`) have RQL type adapters: they print as `(x, y, z)` and accept `"(1, 2, 3)"`, `[1, 2, 3]`, `{X: 1, …}`
  (an object literal builds a new value: members it does not name are zero); colors also `"#ff8800"`. They compare
  with those forms too (`where Location == "(0, 0, 100)"`, components within 1e-5).
- **UE's text form for any other struct**: a string in the form the editor's Copy / `ExportText` writes is imported with
  `UScriptStruct::ImportText` (RQL's `MutationPolicy::coerce_text`), so `set Slot.LayoutData.Anchors =
  "(Minimum=(X=0.5,Y=0),Maximum=(X=0.5,Y=0))"` works on structs without an adapter; text that does not import (an
  unknown member, trailing text) fails with `coercion-failed` and UE's message.
- **Gameplay tags** read and write as text: `FGameplayTag` as `"Ability.Fire"` (`""` = none), `FGameplayTagContainer`
  as `["Ability.Fire", "State.Stunned"]` (or `"A.B, C.D"`), validated against the registered tags (an unknown tag fails
  and names similar ones); containers are rebuilt with `AddTag`, so their parent-tag cache stays right. Members:
  `Tag.Parent`, `Tags.Names`, `Tags.Num`; `==` compares with text / lists (containers as sets). `FGameplayTagQuery`
  shows its `Description` and is read-only.
- **Soft paths** (`FSoftObjectPath`, `FSoftClassPath`: `GameDefaultMap`, classes to spawn) read and write as text
  (`"/Game/Maps/Arena.Arena"`, `""` = none, or an object); they compare with the path or its package
  (`where GameDefaultMap == "/Game/Maps/Arena"`); members `Path`, `Package`.
- **Struct members are known before a value is read**: a struct type's members are filled when a member of that type is
  added (and when an object literal constructs it), so `where Row.Damage > 3` and `set Row = {Damage: 5}` work on a
  struct the session never read.
- **Per-call session**: each tool call builds a fresh engine and type cache (cheap: descriptors fill lazily). A
  Blueprint compile, hot reload or user-defined struct edit recreates `FProperty` / `UFunction` objects, so no
  descriptor may outlive the call that built it.
- **Mutations** run in one editor transaction (one Ctrl+Z step, named `RQL: <query>` in the Undo History, so `TRANSACTION UNDO`
  says which change it reverts; `python-execute` steps are named after the snippet's first statement); owners are `Modify()`d before the first write and
  their packages marked dirty after. The receipt's `new` column is the value read back after the write (what the
  setter and the class's `PostEditChangeProperty` left); `rql-mutate` refuses a query without `set` / `call`, and a
  throwing operation is `call-threw`. Unlike Unity-MCP, `rql-mutate` does **not** save to disk: Unreal edits stay
  unsaved until `editor | call SaveAll()` / `SaveLevel()` / `SaveAsset(…)` (saving in Unreal can raise source-control
  checkouts and Save-As dialogs, which an unattended tool call must not).
- **Batch checks** (RQL's `MutationPolicy::validate_batch`): every planned change of a `set` / `call` stage is checked at
  once before anything runs, dry runs included, and a refusal is `invalid-value` with nothing changed: class default
  objects are read-only (except settings objects and API objects), a settings write must hit a config property, and one
  object reached twice by the same `call` (`actors | get Parent | call Destroy()` over siblings) is refused (`| distinct`
  first) — a call cannot be rolled back, so running it twice would break all-or-nothing. Hints never advise
  `budgets.*` (the tools take no budgets), a `ref("Name")` hint names `unreal.find("…")`, and a hint that sends the
  caller to the type list names `rql-schema` (`Engine::schema_hint`).
- **Target limit**: `rql-mutate` changes at most 200 targets per call by default (a mutation is one undo step whatever its
  size, RQL spec §8); its `max_targets` argument raises the limit for one call up to the ceiling of 1000
  (`MutationPolicy::limit_argument`, mapped to `Budgets::max_mutation_targets`). Over the limit nothing changes and
  `mutation-limit` names the match count (or "more than 1000") and, while the limit is below 1000, `max_targets`;
  at the ceiling it suggests `first N` / `skip N` batches.
- **Typos in mutations**: when no item reaches `set` / `call` because a member read (`where` / `get`) missed on every
  item and no type declares the name, the `unknown-member` warning is an error (`ok=false`, nothing changed). Streams
  are polymorphic (`actors | get Components`), so the session's `Engine::is_known_member` accepts the exposed property
  names of every loaded class and struct (user-defined struct fields by authored name) and the members hooks add:
  `get Components | where Intensity > 0 | set Intensity = …` over actors without lights stays a no-match (ok, 0
  changes) while `get RootComponnt | set …` fails. A `set` that reaches targets lacking the member is still an
  `unknown-member` error: filter first (`where $ is LightComponentBase`).
- **Enums** read as entry names and accept the entry name, the editor's display name (`UMETA(DisplayName)`, RQL enum
  aliases; one that spells another entry's name is skipped), `Type::Name` or the number.
- **Function sources**: RQL text lets a host function start a query (`unreal.assets("Material") | …`, also with a member
  path after it: `unreal.asset("/Game/BP_Door").EventGraph | …`); the C++ runtime runs a bare function call but not
  the member-path form, so the session evaluates such a call (literal arguments) once and runs the query on a source
  holding its value.

### Sources and functions (core module)

| Source / function | What |
|---|---|
| `actors` | every actor of the editor world's loaded levels |
| `levels`, `world` | the loaded levels, the editor world |
| `selection` | the selected actors (`editor | set Selection = [...]` writes it) |
| `editor` | editor state (`IsPlaying`, `IsPaused` — writable, `Selection`, `World`, `CurrentLevel`; the transaction buffer: `NextUndo`, `NextRedo`, `UndoHistory`, `RedoHistory` — titles, next first — `UndoCount`, `RedoCount`) and operations: `SpawnActor` (a class, Blueprint name or path, or an asset to place: a StaticMesh path gives a StaticMeshActor; a volume class gets the editor's 200 cm box brush, sized with `Scale`), `OpenLevel`, `NewLevel`, `SaveLevel`, `SaveAll`, `AddSublevel` (a saved map as a streaming sublevel; not undoable, the current level stays; refused in World Partition worlds), `UnloadSublevel` (a level by its map name), `CreateAsset`, `CopyAsset`, `MoveAsset`, `DeleteAsset`, `CreateFolder`, `RefreshAssets`, `CreateMaterialInstance`, `ImportAsset` (returns every asset the import created: the mesh, then its materials, textures, skeleton), `SaveAsset`, `ClearConsole` |
| `console` | editor log entries (`Sequence`, `Time`, `Verbosity`, `Category`, `Message`) |
| `unreal.find(label)` | an actor by label, name or path |
| `unreal.asset(path)`, `unreal.assets(class, path?)` | one asset; assets of a class (at most 200, loaded as read; without a path: `/Game`, or in rql-query every content root (engine, plugins) when the project has none of the class) |
| `unreal.classes(base?, filter?)` | classes deriving from a base |
| `dataTables` | the project's DataTables (see [DataTables](#datatables)) |
| `gameplayTags` | every registered gameplay tag: `Name`, `Comment`, `Source` (ini), `Parent`, `Children`, `Explicit`, `Restricted` |
| `settings`, `unreal.settings(section)` | Project Settings / Editor Preferences sections and their objects (see [Settings](#settings)) |
| `blueprints` | the project's Blueprints (see [Blueprint graphs](#blueprint-graphs)) |
| `widgetBlueprints` | the project's Widget Blueprints (see [Widget Blueprints](#widget-blueprints)) |
| `levelSequences` | the project's Level Sequences (see [Level Sequences](#level-sequences)) |

The `editor` operations also include `CreateDataTable(path, rowStruct)` and, for gameplay tags,
`AddGameplayTag(name, comment?, source?)`, `RemoveGameplayTag(name)`, `RenameGameplayTag(name, newName)` (written to
the tag ini at once through the GameplayTagsEditor plugin, not undoable; a rename leaves a redirector).

### DataTables

`UDataTable::RowMap` is not a `UPROPERTY`, so reflection does not see a table's rows. The core module adds:

- `Rows`: a map from row name (case-insensitive) to the row struct. `set Rows["Sword"].Damage = 12` writes the row in
  place; `set Rows["Bow"] = {Damage: 7}` adds a row (a failing write removes it again); `get Rows | where Value.Damage > 10
  | select Key` streams the rows as entries, and `get Rows | where Value.Damage > 10 | set Value.Damage = Value.Damage * 2`
  writes through the table (each entry is the key-path write `Rows["<row>"].Damage`; the receipt names the table).
- `RowNames`, `RowCount`, `RowStructName` (pass it to `rql-schema` for the row members).
- `call AddRow(name, values?)`, `RemoveRow(name)`, `RenameRow(name, newName)`, `DuplicateRow(name, newName)`: the
  `FDataTableEditorUtils` functions the DataTable editor's buttons call (row names are validated; values that do not fit
  leave no half-made row).

Row writes run in the mutation's transaction (the table is `Modify()`d first, so Ctrl+Z restores the rows), call
`HandleDataTableChanged` and notify open DataTable editors. Composite tables and `UCurveTable` are not covered.

### Settings

`settings` lists the sections of the Project Settings ("Project") and Editor Preferences ("Editor") windows (`Container`,
`Category`, `Section`, `DisplayName`, `Description`) with `Object`, the object the section edits; `unreal.settings(name)`
returns that object by section name, display name or class name (the auto-discovered `UDeveloperSettings` sections
such as Physics, Rendering and Collision are registered first, as the settings window does; UE 5.6+) (`unreal.settings("Maps & Modes") | set GameDefaultMap =
"/Game/Maps/Arena"`). Settings objects are class defaults, read-only elsewhere in RQL; here their **config** properties
are writable (a non-config property is refused: it would be lost on restart) and each mutation is saved the way the
settings editor saves it: one property into `Default<Config>.ini` for project (default-config) classes, the whole section
otherwise; legacy input mappings rebuild their key maps. Config files are not part of the editor transaction: a settings
write is saved at once and Ctrl+Z does not revert it. Source control is not checked out (read-only ini files are made
writable).

### Blueprint graphs

- `UBlueprint`: `Components` (the component templates its Simple Construction Script adds, under their variable names:
  their reflected defaults are writable — `unreal.asset("/Game/BP_Coin").Components | where Name == "Mesh" | set
  StaticMesh = "/Engine/BasicShapes/Sphere"` — and placed actors whose component still holds the old value follow, as
  after an edit in the Components panel; components inherited from a native class or a parent Blueprint are not listed), `Graphs` (event graphs,
  functions, macros), `EventGraph`, `Status` (`UpToDate`, `Dirty`, `Error`, …);
  `call Compile()` returns `{Succeeded, Errors, Warnings, Messages: [{Severity, Message, Node}]}` (a failed compile is a
  result, not an error; `Node` is the node's `$ref`) and `call AddFunction(name)` adds a function graph.
- `UEdGraph`: `Nodes`, `Kind` (`EventGraph`, `Function`, `Macro`, `Delegate`), `Blueprint`; node calls (each returns the
  node; `X` / `Y` place it, default: the editor's next free spot): `AddCallFunction("PrintString" |
  "KismetMathLibrary.Add_IntInt" | a function of the Blueprint)`, `AddVariableGet(name)`, `AddVariableSet(name)`,
  `AddEvent("BeginPlay")` (overridable events of the parent class; enables the editor's placeholder node when there is
  one), `AddCustomEvent(name)`, `AddCast(class)`, `AddNode("Branch" | "Sequence" | "Self" | a standard macro such as
  "ForLoop", "ForEachLoop", "DoOnce", "Gate")`.
- `UEdGraphNode`: `Title`, `X`, `Y`, `Comment`, `Enabled` (writable), `Error` (the last compile's message), `Graph`,
  `Pins`; calls `Connect(pin, target, targetPin)`, `Disconnect(…)`, `BreakLinks(pin?)`, `SetPinDefault(pin, value)`,
  `Destroy()`. Pins are named by pin name or display name; `exec` / `out` name the execution pins.
- `EdGraphPin` (a host object, not a UObject: it holds its node and pin id): `Name`, `DisplayName`, `Direction`, `Type`
  (as the editor names it), `IsExec`, `DefaultValue` (writable: text, numbers, bools, `"(1, 2, 3)"`, an asset path for
  object pins), `LinkedTo`, `Node`; calls `Connect(otherPin)`, `Disconnect(otherPin)`, `BreakLinks()`.

Connections go through the K2 schema (`CanCreateConnection` / `TryCreateConnection`, the checks of a drag between pins),
defaults through `IsPinDefaultValid` / `TrySetDefaultValue`. Every change is undoable (one transaction per mutation) and
marks the Blueprint modified; nothing compiles until `call Compile()` (which runs outside the transaction). The
`blueprint-*` tools stay for variables, components, class defaults and spawning.

### Widget Blueprints

A Widget Blueprint's widgets live in its `WidgetTree`, which reflection does not expose; the widgets are UObjects whose
properties (and `Slot`, the widget's layout in its panel: `set Slot.LayoutData.Offsets.Left = 24`) are reflected as usual.

- `UBaseWidgetBlueprint`: `RootWidget`, `Widgets` (every widget of the tree), `call AddWidget(class, name?, parent?)` (into
  a panel, or as the root of an empty tree), and the Blueprint members (`Compile`, `Status`, `Graphs` …).
- `UWidget`: `Parent`, `WidgetBlueprint`, `call AddChild(class, name?)` (panels), `call Destroy()` (with its children);
  `UPanelWidget`: `Children` (so `RootWidget | recurse Children` walks the tree).

New widgets are registered as Blueprint variables and removed ones leave the tree, as the Designer does; structure
changes mark the Blueprint structurally modified and property edits mark it modified (compile to apply them). The
widgets of a Widget Blueprint are templates of its generated class but its design data: writable here, unlike other
class defaults.

### User-defined structs

A user-defined struct (the struct asset a DataTable row or a Blueprint variable uses when the project has no C++ struct)
keeps its fields in the Struct editor's data, which reflection does not expose. `UUserDefinedStruct` gets `Fields`
(`[{Name, Type, Default}]`) and `call AddField(name, type, default?)`, `RemoveField(name)` (a struct keeps at least one
field), `RenameField(name, newName)`, `SetFieldDefault(name, value)`, through `FStructureEditorUtils`: the struct recompiles
and the DataTables and Blueprints using it update, as after an edit in the Struct editor. Types are the tokens of
`blueprint-add-variable` (`UnrealMcpPinTypes`: `bool`, `int`, `float`, `string`, `vector`, a struct such as
`GameplayTag`, a class such as `StaticMesh`). `editor | call CreateAsset(path, "UserDefinedStruct")` makes one (with the
editor's initial `MemberVar_0` field), `CreateDataTable(path, structPath)` a table of it.

### Level Sequences

Frames are at the sequence's display rate (what the Sequencer shows), converted to and from its tick resolution.

- `ULevelSequence`: `DisplayRate`, `Start`, `End` (playback range, writable), `Bindings`, `Tracks` (tracks not bound to an
  object); `call AddActor(actor)` (a possessable binding, like dragging the actor into the Sequencer) and
  `call AddTrack(class)`.
- `SequenceBinding` (a host object: the sequence and the binding's GUID): `Name`, `Guid`, `Kind` (`Possessable` /
  `Spawnable`), `Class`, `Tracks`; `call AddTrack("Transform" | "Visibility" | a track class | a property)` — every new track gets a
  section over the playback range. A property (`"Intensity"`, `"LightColor"`, `"LightComponent0.Intensity"`: an animatable
  — `Interp` — property of the bound actor or of one of its components) gets the property track of its type (float, double,
  bool, color, vector) bound to it, on the component's binding when the property is a component's (made as the
  Sequencer makes it); its single channel is named after the property (`AddKey("Intensity", 60, 5000)`). A bare property
  track class (`MovieSceneFloatTrack`) is refused, since it would animate nothing; `Visibility` keys the actor's
  `bHidden` (a component's `bHiddenInGame`), as the Sequencer's Visibility track does; its channel is `Visible` (true =
  visible).
- `UMovieSceneTrack`: `Sections`, `DisplayName`, `call AddSection()`; `UMovieSceneSection`: `Start`, `End` (writable),
  `Channels` (`[{Name, Type, Keys: [{Frame, Value}]}]`, names as the Sequencer shows them: `Location.X`, `Rotation.Yaw`),
  `call AddKey(channel, frame, value)` (the channel by that name, or by its internal name: `Rotation.Z` is the yaw) (double / float / bool channels; a key outside the section grows it).

Every change is undoable (one transaction per mutation). Spawnables are listed but not created.

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
| `level-create` / `-open` / `-save` / `-set-current` / `-unload-sublevel` / `-list-loaded` | `editor | call NewLevel / OpenLevel / SaveLevel / AddSublevel / UnloadSublevel`, `levels` (`set IsCurrent = true`, `set IsVisible = false`) |
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

- `UnrealMcpRqlAdaptersSpec.cpp` (`UnrealMcp.Tools.Rql.Adapters`): every existing adapter on a fixture object with one
  property of each kind (`UnrealMcpRqlSpecFixtures.h`): the nine math structs (text, lists, literals, equality, errors),
  references, arrays / sets / maps, scalars and enums (ranges, rollback, undo), asset and class-defaults lists, the
  console, material parameter maps, the seven class hooks and every registration of the extension seam.
- `UnrealMcpRqlDataModelsSpec.cpp` (`UnrealMcp.Tools.Rql.DataModels`): DataTable rows (edit, add / rename / duplicate /
  remove, validation, undo, save and reload), user-defined struct fields, gameplay tags (tag ini edits, text conversion,
  parent tags and their comments, rows with tags), Blueprint component defaults and Instance Editable variables,
  settings (sections, ini writes, refusals, dry run), Blueprint graphs (nodes, pins, defaults, connections, compile
  errors, undo), Widget Blueprints (tree, properties, slots, removal, compile, undo) and Level Sequences (bindings,
  tracks, sections, keys, ranges, undo).
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
