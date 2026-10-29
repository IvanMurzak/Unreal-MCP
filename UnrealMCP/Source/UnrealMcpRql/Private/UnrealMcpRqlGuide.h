// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

// Agent-facing texts of the rql-* tools and the unreal-rql skill (UTF-8, ASCII only so every compiler reads them alike).
// The language section is RQL's evaluated agent prompt adapted to Unreal, as Unity-MCP's RqlGuide does for Unity: the guide
// travels in the tool descriptions, which agents load before first use; the blurbs are the always-visible catalog lines.
// RQL's shared language rules (LanguageRules) follow the language section, so the rules every host states alike are not
// copied here.

#include "UnrealMcpRqlInclude.h"

namespace UnrealMcpRqlGuide
{
	/**
	 * RQL's own language rules text (conditions, ?:, list equality, group items, receipts, call, bulk create, vectors, map keys,
	 * typed literals) with SpawnActor(ClassName, Label, Location, ...) calls as its creation examples.
	 */
	inline const char* LanguageRules()
	{
		static const std::string Rules = []()
		{
			rql::RuleExamples Examples;
			Examples.create_call = "editor.SpawnActor";
			Examples.grid = "editor.SpawnActor(\"/Engine/BasicShapes/Cube\", \"Tile_\" + $, [($ % 15) * 200, floor($ / 15) * 200, 0])";
			Examples.per_entry = "[[\"Lamp_A\", [0, 0, 300]], [\"Lamp_B\", [400, 0, 300]]] | call editor.SpawnActor(\"PointLight\", $[0], $[1])";
			Examples.named = "editor.SpawnActor({ClassName: \"/Engine/BasicShapes/Cube\", Label: \"Tile_\" + $, Location: [$ * 200, 0, 0]})";
			return rql::language_rules_with(Examples);
		}();
		return Rules.c_str();
	}

	inline const char* QueryBlurb()
	{
		return "Query the live Unreal Editor world (actors, components, assets, levels) with RQL in ONE call: filter, project, "
			"sort, count, walk hierarchies. Far fewer calls/tokens than reading objects one by one.";
	}

	inline const char* MutateBlurb()
	{
		return "Change the live Unreal Editor with RQL set/call: bulk edits in one all-or-nothing, undoable call; dry_run previews.";
	}

	inline const char* SchemaBlurb()
	{
		return "List RQL sources and the actor / component classes in the editor world; pass a type name for its members.";
	}

	inline const char* Language()
	{
		return R"RQL(RQL: source | stage | stage ...
Sources: actors (every actor of the editor world's loaded levels), levels, world, selection, console (editor log entries), editor (editor state and operations), dataTables, gameplayTags, settings (Project Settings / Editor Preferences sections), blueprints, widgetBlueprints, levelSequences, plus the sources of installed extensions (rql-schema lists them). Assets: unreal.assets("Material") or unreal.assets("StaticMesh", "/Game/Props") (at most 200, loaded lazily), unreal.asset("/Game/Materials/M_Red"). ref("PointLight#1234") resolves a $ref; unreal.find("Lamp") an actor by label, name or path; unreal.classes("ActorComponent", "Light") lists classes; unreal.settings("Maps & Modes") a settings object. A member path may follow a function: unreal.asset("/Game/BP_Door").EventGraph.
Stages:
  where <predicate>                  filter
  select a b.c alias=(expr)          project columns (wrap computed expressions in parentheses)
  sort <expr> [desc], ...            stable sort
  first N | skip N | count
  get <path>                         replace each item by a member (lists are flattened)
  recurse Children [max-depth=N] [include-self=false]   walk a hierarchy (input items included by default)
  distinct [expr] | sum expr | min expr | max expr | avg expr
  group-by expr                      rows key|count; group-by a, b (or kind=$type) gives one column per key, then count; aggregate each group's items: group-by $type | select key, count, avg = items.avg(Intensity) (items.sum / .min / .max / .count(pred) / .first().Label)
Expressions: == != < <= > >= and or not + - * / %; contains, starts-with, ends-with, matches "regex" (all case-insensitive); x in [a, b]; x is null; x is not null; $ is PointLight (includes subclasses; a Blueprint's $type is BP_Door_C, $ is BP_Door matches it and its child Blueprints); has(member); functions abs round floor ceil sqrt pow min(a, b) max(a, b) len lower upper coalesce; conditional: cond ? a : b or if(cond, a, b); text: replace(s, from, to) (case-insensitive), replace-regex(s, pattern, repl) ($1 = group 1), substring(s, start[, length]), split(s, sep), trim(s), pad(v, width[, fill]) - also as methods: Label.replace("Old", "New"), Label.substring(0, 4), Index.pad(3). After select, sort / where use the column names (sort Owner.Label). Literals: numbers, "strings", true/false/null, lists [a, b], objects {X: 1, Y: 2}.
Paths: a.b.c, list[0], map["key"]; .member on a list maps over its elements. List methods: .any(pred) .all(pred) .count([pred]) .sum(e) .min(e) .max(e) .first([pred]); inside them bare names refer to the element, falling back to the outer item.
Meta: $ (current item), $ref (stable id such as PointLight#1234), $type, $path, $index, $depth. Enums compare to strings: the entry name, the editor's display name or the number (Mobility == "Movable"); == on strings is case-sensitive.
Unreal: every object has Id, Name, Path. Actor has Label, Location, Rotation, Scale (world transform), Components (list), Children (attached actors), Parent, Folder, Level, HiddenInEditor (editor viewport only), bHidden (Actor Hidden In Game: invisible when playing), Selected; a Volume also BrushExtent (writable: rebuilds its box brush) and BrushBounds. Locations, distances and sizes such as BrushExtent are Unreal units, centimetres, Z up (Scale is a plain multiplier, 1 = original size): a user's metres are x 100 ("start at (-10, -10, 0) m, 2 m apart" is [-1000, -1000, 0] stepping 200). Other members are the reflected properties (UPROPERTY EditAnywhere / VisibleAnywhere / BlueprintVisible) under their C++ names (bHidden, Intensity, RelativeLocation, StaticMesh); find them via actors | get Components | where $ is PointLightComponent (every light: $ is LightComponentBase; LightComponent leaves out sky lights). Vectors, rotators and colors print as (x, y, z) (rotators (pitch, yaw, roll)); write "(1, 2, 3)", [1, 2, 3] or {X: 1, Y: 2, Z: 3}; colors also "#ff8800". Vector, Vector2D, Vector4, LinearColor and Rotator compute component-wise: Scale * 1.5, Location + [0, 0, 100], Location - Other.Location (a rotator per axis, Rotation + [0, 90, 0] adds 90 to Yaw: an editor nudge, not a composed rotation); Quat and Transform do not. Object references accept ref("..."), unreal.asset(path) or an asset path "/Game/Meshes/SM_Cube". Material parameters: Scalars["Roughness"], Vectors["BaseColor"], Textures["Albedo"]. Gameplay tags are text: Tag == "Ability.Fire", set Tags = ["Ability.Fire", "State.Stunned"] (registered tags only). DataTable rows: Rows["Sword"].Damage (RowNames, RowStructName). Blueprints: Components (the components it adds; their defaults: StaticMesh, ...), Graphs / EventGraph -> Nodes -> Pins (Name Direction Type DefaultValue LinkedTo). User-defined structs: Fields. Widget Blueprints: RootWidget / Widgets, panels have Children, widgets Parent, Slot and IsVariable (set true to read the widget in graphs). Level Sequences: Bindings -> Tracks -> Sections -> Channels (frames at the display rate). Destroyed objects read as null (where StaticMesh == null finds empty references). Navigate with paths or get; select only projects output columns.
Results come back as compact rows (header line, then col|col). Long results are truncated - refine with where/first or use count. Unknown members produce warnings with suggestions; rql-schema(type) lists a type's members.)RQL";
	}

	inline const char* QueryExamples()
	{
		return R"RQL(Examples:
  actors | where Label contains "Enemy" | select $ref Label Location
  actors | get Components | where $ is PointLightComponent and Intensity > 2 | select Owner.Label Intensity LightColor
  actors | get Components | where $ is LightComponentBase and CastShadows | sort Intensity desc | select Owner.Label $type Intensity      every light kind (sky lights too)
  actors | where Parent == null | recurse Children | select $depth Label
  actors | where $ is PointLight and distance(Location, unreal.find("Player").Location) < 1000 | select Label      within 10 m (cm)
  unreal.assets("MaterialInstanceConstant") | select Name Scalars Vectors
  console | where Verbosity == "Error" | sort Sequence desc | first 5 | select Message
  actors | get Components | where $ is LightComponentBase | group-by $type | select key, count, avg = items.avg(Intensity)
  levels | select Map IsPersistent IsCurrent IsVisible
  editor | select IsPlaying IsPaused Selection
  editor | select NextUndo UndoHistory RedoHistory      what Ctrl+Z / Ctrl+Y would revert (RQL steps are "RQL: <query>")
  actors | where $ is Volume | select Label BrushExtent BrushBounds      volume shapes (BrushExtent null = no brush, no shape)
  unreal.asset("/Game/Data/DT_Weapons") | get Rows | where Value.Damage > 10 | select Key Value.Damage
  gameplayTags | where Name starts-with "Ability." | select Name Comment
  settings | where Container == "Project" | select Category Section DisplayName
  unreal.asset("/Game/BP_Door").EventGraph | get Nodes | select $ref Title Pins.Name)RQL";
	}

	inline const char* MutateStages()
	{
		return R"RQL(  set path = expr, ...   assign members (paths may go through structs: set Location.Z = Location.Z + 100)
                         values see each target: $ (the target), $index (its position in the stream entering set / call)
  call Method(args)      invoke a method the tools expose
                         installed extensions add sources with operations (e.g. landscapes, Control Rig, Niagara, PCG,
                         Enhanced Input): check rql-schema before writing C++ or Python for an editor feature
)RQL";
	}

	inline const char* MutateExamples()
	{
		return R"RQL(Examples:
  actors | where Label == "Player" | set Location = "(0, 0, 100)"
  actors | get Components | where $ is PointLightComponent | set Intensity = Intensity * 2, LightColor = "#ffcc88"
  actors | where Label starts-with "Coin" | set Scale = Scale * 1.5, Location = Location + [0, 0, 50]       vectors compute component-wise
  actors | where Label starts-with "Enemy" | set bHidden = true          invisible in game (Actor Hidden In Game); HiddenInEditor only hides it in the editor viewport
  actors | where Label starts-with "Pillar_" | set Label = Label.replace("Pillar_", "Column_")      bulk rename
  actors | where Label starts-with "Crate" | sort Label | set Location.X = $index * 200              spread along X
  actors | where Label starts-with "Guard" | set Location = Location + normalize([1, 1, 0]) * 300     3 m diagonally (vector functions: length, normalize, distance, dot, lerp; [X, 0, Z] * k)
  unreal.asset("/Game/Input/IA_Jump") | set Triggers = [{$type: "InputTriggerHold", HoldTimeThreshold: 0.5}], Modifiers = append(Modifiers, {$type: "InputModifierNegate"})
                         instanced subobjects (EditInlineNew: input triggers / modifiers ...) are built from a typed literal for the object that owns them
  actors | get Components | where $ is LightComponentBase | set Intensity = (Intensity > 5000 ? 5000 : Intensity)    clamp
  ref("Button#34") | set Slot.LayoutData.Anchors = "(Minimum=(X=0.5,Y=0),Maximum=(X=0.5,Y=0))"   structs also take UE's text form (the editor's Copy)
  unreal.asset("/Game/Materials/MI_Red") | set Scalars["Roughness"] = 0.2, Vectors["BaseColor"] = "(1, 0, 0, 1)"
Create / add / remove (call; the receipt's result column holds the new object's $ref for follow-up queries):
  editor | call SpawnActor("PointLight", "Lamp", "(0, 0, 300)")          class, label, location (cm: 1 m = 100), rotation, parent, scale
  editor | call SpawnActorFromAsset("/Game/Props/SM_Rock", "Rock", [0, 0, 0], [0, 0, 0], null, [2, 2, 2])     place an asset (mesh, Blueprint, sound) as drag and drop does
  range(0, 300) | call editor.SpawnActor("/Engine/BasicShapes/Cube", "Cube_" + $, [($ % 20) * 150, floor($ / 20) * 150, 50])   with max_targets=300: a 20 x 15 grid, one undo step
  [["Lamp_A", 0], ["Lamp_B", 400]] | call editor.SpawnActor("PointLight", $[0], [$[1], 0, 300])      one spawn per list entry
  range(0, 10) | call editor.SpawnActor({ClassName: "PointLight", Label: "Lamp_" + $, Location: [$ * 300, 0, 300]})   arguments by name (ClassName Label Location Rotation Parent Scale)
  actors | where Label == "Crate" | call AddComponent("StaticMeshComponent", "Mesh")
  actors | where Label == "Old" | call Destroy()                         components: get Components | where $ is X | call Destroy()
  actors | where Label == "Lamp" | set Parent = ref("StaticMeshActor#1234")   attach, keeping the world transform; null detaches
  actors | where Label == "Crate" | call Duplicate("(200, 0, 0)")        copy with an offset (returns the copy)
Levels, assets, editor state:
  editor | call OpenLevel("/Game/Maps/Arena")        editor | call NewLevel("/Game/Maps/New")
  editor | call SaveLevel()        editor | call SaveAll()        levels | where Map == "Sub" | set IsCurrent = true
  editor | call CreateMaterialInstance("/Game/Materials/MI_Glow", "/Game/Materials/M_Base")
  editor | call CreateAsset("/Game/Data/DA_Sword", "WeaponData")         any asset class (the editor's factory when it has one)
  editor | call CopyAsset("/Game/A", "/Game/B")    MoveAsset / DeleteAsset / CreateFolder("/Game/Props") / ImportAsset("C:/art/crate.fbx", "/Game/Props")   (returns the mesh and the materials / textures it created)
  actors | where Label == "NavBounds" | set BrushExtent = "(2000, 2000, 500)"     a volume's box brush (half-size, cm)
  editor | set Selection = [ref("StaticMeshActor#1234")]        actors | where $ is PointLight | set Selected = true        editor | set IsPlaying = true
Data, tags, settings (settings are saved to their ini at once, tag edits too; neither is undoable):
  unreal.asset("/Game/Data/DT_Weapons") | set Rows["Sword"].Damage = 12        call AddRow("Bow", {Damage: 7}) / RemoveRow / RenameRow / DuplicateRow
  unreal.asset("/Game/Data/DT_Weapons") | get Rows | where Value.Damage > 10 | set Value.Damage = Value.Damage * 2     rows (map entries) as targets
  editor | call CreateDataTable("/Game/Data/DT_Weapons", "WeaponRow")      editor | call AddGameplayTag("Ability.Fire", "fire ability")
  editor | call CreateAsset("/Game/Data/S_Enemy", "UserDefinedStruct")      unreal.asset("/Game/Data/S_Enemy") | call AddField("Health", "float", 100)   RemoveField / RenameField / SetFieldDefault
  unreal.settings("Maps & Modes") | set GameDefaultMap = "/Game/Maps/Arena"
Blueprints (component defaults reach placed actors that keep the old value; each graph Add* returns the node; connect by pin name; Compile returns errors with the node's $ref):
  unreal.asset("/Game/BP_Coin").Components | where Name == "Mesh" | set StaticMesh = "/Engine/BasicShapes/Sphere"
  unreal.asset("/Game/BP_Door").EventGraph | call AddEvent("BeginPlay")      AddCallFunction("PrintString") / AddVariableGet("Health") / AddVariableGet("BP_Player.Score") (Target pin) / AddInputKey("SpaceBar") / AddNode("Branch")
  ref("K2Node_Event#12") | call Connect("then", ref("K2Node_CallFunction#34"), "execute")     ref("K2Node_CallFunction#34") | call SetPinDefault("InString", "Hi")
  unreal.asset("/Game/BP_Door") | call Compile()
Widgets and sequences:
  unreal.asset("/Game/UI/WBP_Menu") | call AddWidget("Button", "Play", ref("VerticalBox#56"))      ref("TextBlock#78") | set Text = "Play"
  unreal.asset("/Game/Cine/LS_Intro") | call AddActor(unreal.find("Door"))      ... | get Bindings | where Name == "Door" | call AddTrack("Transform")
  unreal.asset("/Game/Cine/LS_Intro") | get Bindings | get Tracks | get Sections | call AddKey("Location.Z", 30, 200)    channels: Location.X, Rotation.Yaw, Scale.Z ...
  ... | get Bindings | where Name == "Lamp" | call AddTrack("Intensity")      any animatable property (the light component's too); key it: AddKey("Intensity", 60, 5000)
)RQL";
	}

	inline const char* QuerySyntaxShort()
	{
		return R"RQL(Query part: the RQL of rql-query (full guide and the sources in its description); unreal.assets("Material"), unreal.asset(path), unreal.find("Label"), ref("PointLight#1234"). Stages: where, select, sort, first N, get <path>, recurse Children. Operators: == != < > and or not, contains, starts-with, $ is PointLight, cond ? a : b; conditions must be booleans (StaticMesh != null, has(x)). Paths a.b.c, list[0], map["key"]. Vectors/colors as "(1, 2, 3)" or "#ff8800"; enums as strings.)RQL";
	}

	inline const char* QueryIntro()
	{
		return "Run a read-only RQL query against the live Unreal Editor world and return compact rows. One precise query beats many "
			"exploratory calls. Changes need rql-mutate.";
	}

	inline const char* MutateIntro()
	{
		return "Change the live Unreal Editor with an RQL query whose last stage is set or call. All-or-nothing: every target and "
			"value is checked before anything is written; a failure changes nothing. One undo step in the editor (Ctrl+Z). Changes "
			"are not saved to disk: when the user wants them saved, run editor | call SaveAsset(\"/Game/Path\") (one asset), SaveLevel() or SaveAll(). dry_run=true returns the "
			"receipt ($ref, member, old, new) without changing anything. The receipt lists the assignments that change a value; those that keep "
			"the old value are only counted (N unchanged). Do exactly the change requested; a read-only member fails "
			"with readonly-member instead of being approximated.";
	}

	inline const char* MutateOutro()
	{
		return "Object literals build values: set Settings = {Speed: 2}, and instanced subobjects (EditInlineNew classes such as "
			"input triggers and modifiers) from a typed literal: {$type: \"InputTriggerHold\", HoldTimeThreshold: 0.5}, owned by the "
			"object written to. Actors, components and assets are never built from literals: reference them (unreal.find(\"Label\"), "
			"ref(\"Type#id\"), an asset path) and create them with editor | call SpawnActor / AddComponent / CreateAsset. "
			"Destructive cleanup (Destroy, DeleteAsset, removing many items): first list what would go (rql-query, or dry_run=true) "
			"and show it to the user; delete only in a second call after that. At most 200 targets per call by default: pass max_targets "
			"(up to 1000) for a bigger edit, it stays one all-or-nothing undo step; beyond 1000 split it with first N / skip N. A misspelt "
			"member in where / get that leaves set / call nothing to change is an error (nothing changed); a real member no remaining "
			"item has is a no-match (0 changes, warning). Filter before setting a member only some classes have: where $ is "
			"LightComponentBase (or has(Intensity)) | set Intensity = ... When you use a different member than the user named (a "
			"misspelling, or the did-you-mean of a warning), or a different actor / asset than the name the user gave, tell the user which one you changed. Bulk create: range(0, N) | call "
			"editor.SpawnActor(... $ ...) spawns N actors in one call (each counts against max_targets; every spawn is checked "
			"before the first); the receipt has one row per actor with its $ref.";
	}

	inline const char* SchemaDescription()
	{
		return "Without a type: RQL sources, functions, and the actor and component classes in the editor world (with counts), "
			"plus the Actor members. With a type name (e.g. \"PointLight\", \"StaticMeshComponent\", \"Vector\"): that type's "
			"members (name type; ro = read-only) and methods. Use it before querying an unfamiliar class.";
	}

	/** The skill rql-query ships (FUnrealMcpToolBuilder::Skill): the one place its name and description live. */
	inline const char* SkillName()
	{
		return "unreal-rql";
	}

	inline const char* SkillDescription()
	{
		return "How to read and change anything in the Unreal Editor with RQL (rql-query / rql-mutate / rql-schema): the query "
			"language, operators, sources (actors, components, assets, levels, console logs, editor state, DataTable rows, gameplay "
			"tags, Project Settings, Blueprint graphs), editor operations (spawn actors, add components, levels, assets, "
			"selection, Play In Editor, Blueprint nodes and pins) and what installed extensions add.";
	}
}
