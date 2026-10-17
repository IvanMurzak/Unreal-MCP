// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

// Agent-facing texts of the rql-* tools and the unreal-rql skill (UTF-8, ASCII only so every compiler reads them alike).
// The language section is RQL's evaluated agent prompt adapted to Unreal, as Unity-MCP's RqlGuide does for Unity: the guide
// travels in the tool descriptions, which agents load before first use; the blurbs are the always-visible catalog lines.

namespace UnrealMcpRqlGuide
{
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
Sources: actors (every actor of the editor world's loaded levels), levels, world, selection, console (editor log entries), editor (editor state and operations), plus the sources of installed extensions (rql-schema lists them). Assets: unreal.assets("Material") or unreal.assets("StaticMesh", "/Game/Props") (at most 200, loaded lazily), unreal.asset("/Game/Materials/M_Red"). ref("PointLight#1234") resolves a $ref; unreal.find("Lamp") an actor by label, name or path; unreal.classes("ActorComponent", "Light") lists classes.
Stages:
  where <predicate>                  filter
  select a b.c alias=(expr)          project columns (wrap computed expressions in parentheses)
  sort <expr> [desc], ...            stable sort
  first N | skip N | count
  get <path>                         replace each item by a member (lists are flattened)
  recurse Children [max-depth=N] [include-self=false]   walk a hierarchy (input items included by default)
  distinct [expr] | group-by expr | sum expr | min expr | max expr | avg expr
Expressions: == != < <= > >= and or not + - * / %; contains, starts-with, ends-with (case-insensitive); matches "regex"; x in [a, b]; x is null; x is not null; $ is PointLight (includes subclasses); has(member). Literals: numbers, "strings", true/false/null, lists [a, b], objects {X: 1, Y: 2}.
Paths: a.b.c, list[0], map["key"]; .member on a list maps over its elements. List methods: .any(pred) .all(pred) .count([pred]) .sum(e) .min(e) .max(e) .first([pred]); inside them bare names refer to the element, falling back to the outer item.
Meta: $ (current item), $ref (stable id such as PointLight#1234), $type, $path, $index, $depth. Enums compare to strings (Mobility == "Movable"); == on strings is case-sensitive.
Unreal: every object has Id, Name, Path. Actor has Label, Location, Rotation, Scale (world transform), Components (list), Children (attached actors), Parent, Folder, Level, HiddenInEditor. Other members are the reflected properties (UPROPERTY EditAnywhere / VisibleAnywhere / BlueprintVisible) under their C++ names (bHidden, Intensity, RelativeLocation, StaticMesh); find them via actors | get Components | where $ is PointLightComponent. Vectors, rotators and colors print as (x, y, z) (rotators (pitch, yaw, roll)); write "(1, 2, 3)", [1, 2, 3] or {X: 1, Y: 2, Z: 3}; colors also "#ff8800". Object references accept ref("..."), unreal.asset(path) or an asset path "/Game/Meshes/SM_Cube". Material parameters: Scalars["Roughness"], Vectors["BaseColor"], Textures["Albedo"]. Destroyed objects read as null (where StaticMesh == null finds empty references). Navigate with paths or get; select only projects output columns.
Results come back as compact rows (header line, then col|col). Long results are truncated - refine with where/first or use count. Unknown members produce warnings with suggestions; rql-schema(type) lists a type's members.)RQL";
	}

	inline const char* QueryExamples()
	{
		return R"RQL(Examples:
  actors | where Label contains "Enemy" | select $ref Label Location
  actors | get Components | where $ is PointLightComponent and Intensity > 2 | select Owner.Label Intensity LightColor
  actors | where Parent == null | recurse Children | select $depth Label
  unreal.assets("MaterialInstanceConstant") | select Name Scalars Vectors
  console | where Verbosity == "Error" | sort Sequence desc | first 5 | select Message
  levels | select Map IsPersistent IsCurrent IsVisible
  editor | select IsPlaying IsPaused Selection)RQL";
	}

	inline const char* MutateStages()
	{
		return R"RQL(  set path = expr, ...   assign members (paths may go through structs: set Location.Z = Location.Z + 100)
  call Method(args)      invoke a method the tools expose
)RQL";
	}

	inline const char* MutateExamples()
	{
		return R"RQL(Examples:
  actors | where Label == "Player" | set Location = "(0, 0, 100)"
  actors | get Components | where $ is PointLightComponent | set Intensity = Intensity * 2, LightColor = "#ffcc88"
  actors | where Label starts-with "Enemy" | set HiddenInEditor = true
  unreal.asset("/Game/Materials/MI_Red") | set Scalars["Roughness"] = 0.2, Vectors["BaseColor"] = "(1, 0, 0, 1)"
Create / add / remove (call; the receipt's result column holds the new object's $ref for follow-up queries):
  editor | call SpawnActor("PointLight", "Lamp", "(0, 0, 300)")          class, label, location, rotation, parent
  actors | where Label == "Crate" | call AddComponent("StaticMeshComponent", "Mesh")
  actors | where Label == "Old" | call Destroy()                         components: get Components | where $ is X | call Destroy()
  actors | where Label == "Lamp" | set Parent = ref("StaticMeshActor#1234")   attach, keeping the world transform; null detaches
  actors | where Label == "Crate" | call Duplicate("(200, 0, 0)")        copy with an offset (returns the copy)
Levels, assets, editor state:
  editor | call OpenLevel("/Game/Maps/Arena")        editor | call NewLevel("/Game/Maps/New")
  editor | call SaveLevel()        editor | call SaveAll()        levels | where Map == "Sub" | set IsCurrent = true
  editor | call CreateMaterialInstance("/Game/Materials/MI_Glow", "/Game/Materials/M_Base")
  editor | call CopyAsset("/Game/A", "/Game/B")    MoveAsset / DeleteAsset / CreateFolder("/Game/Props") / ImportAsset("C:/art/crate.fbx", "/Game/Props")
  editor | set Selection = [ref("StaticMeshActor#1234")]        editor | set IsPlaying = true
)RQL";
	}

	inline const char* QuerySyntaxShort()
	{
		return R"RQL(Query part: the RQL of rql-query (full guide and the sources in its description); unreal.assets("Material"), unreal.asset(path), unreal.find("Label"), ref("PointLight#1234"). Stages: where, select, sort, first N, get <path>, recurse Children. Operators: == != < > and or not, contains, starts-with, $ is PointLight. Paths a.b.c, list[0], map["key"]. Vectors/colors as "(1, 2, 3)" or "#ff8800"; enums as strings.)RQL";
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
			"are not saved to disk: call SaveLevel / SaveAll / SaveAsset when the user wants them saved. dry_run=true returns the "
			"receipt ($ref, member, old, new) without changing anything. Do exactly the change requested; a read-only member fails "
			"with readonly-member instead of being approximated.";
	}

	inline const char* MutateOutro()
	{
		return "Object literals build values: set Settings = {Speed: 2}. At most 20 targets per call.";
	}

	inline const char* SchemaDescription()
	{
		return "Without a type: RQL sources, functions, and the actor and component classes in the editor world (with counts), "
			"plus the Actor members. With a type name (e.g. \"PointLight\", \"StaticMeshComponent\", \"Vector\"): that type's "
			"members (name type; ro = read-only) and methods. Use it before querying an unfamiliar class.";
	}

	inline const char* SkillDescription()
	{
		return "How to read and change anything in the Unreal Editor with RQL (rql-query / rql-mutate / rql-schema): the query "
			"language, operators, sources (actors, components, assets, levels, console logs, editor state), editor operations "
			"(spawn actors, add components, levels, assets, selection, Play In Editor) and what installed extensions add.";
	}
}
