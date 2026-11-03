// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpRuntimeCoreTools.h"
#include "Tools/UnrealMcpSkillTools.h"

namespace UnrealMcpCoreTools
{
	UNREALMCPEDITOR_API void RegisterAll(FUnrealMcpToolRegistry& Registry)
	{
		UnrealMcpPingTool::Register(Registry); // runtime-safe liveness probe (UnrealMcpRuntimeCoreTools.h)
		// --- Editor-only families (UnrealMcpCoreTools.h) ---
		// docs/rql.md: the RQL data layer — rql-query / rql-mutate / rql-schema replace the actor / object / asset / level /
		// editor-state / selection / console-log / reflection tools (the UnrealMcpRql module).
		UnrealMcpRqlTools::Register(Registry);
		UnrealMcpBlueprintTools::Register(Registry); // §10 flagship Blueprint family (CORE)
		UnrealMcpEditorTools::Register(Registry); // §10 console family: console-run-command
		UnrealMcpToolListTools::Register(Registry); // unreal-tool-list: every callable tool, hidden ones included
		UnrealMcpPythonTools::Register(Registry); // python-execute (PythonScriptPlugin + vendored code_exec)
		UnrealMcpScreenshotTools::Register(Registry); // §10 screenshot / viewport-capture family (issue #17)
		UnrealMcpSourceTools::Register(Registry); // §10 C++ source / script family (issue #18)
		UnrealMcpSkillTools::Register(Registry); // §2.4 skill authoring — `unreal-skill-create` (SYSTEM surface)
	}
}
