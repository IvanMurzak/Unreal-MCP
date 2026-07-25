// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * The SKILL-authoring tool family (docs/ARCHITECTURE.md §2.4) — Unreal's answer to Unity's
 * `unity-skill-create`. Registers ONE SYSTEM tool:
 *
 *   - `unreal-skill-create` — generate a new MCP tool as a C++ source file inside the plugin's editor
 *     module and report, honestly and explicitly, that the editor must be REBUILT before the new tool
 *     can be called.
 *
 * (Its sibling `unreal-skill-generate` — regenerate every `SKILL.md` from the registered tool set — is
 * NOT here: SKILL.md generation was deliberately moved sidecar-side in #101, and a C++ tool handler may
 * not synchronously wait on bridge state (§4 / §11 risk 2), so a C++-homed generate tool could only
 * fire-and-forget and misreport completion. It is a bridge-native system tool instead — see
 * `bridge/src/Tools/SkillGenerateTool.cs`.)
 *
 * WHY C++ AND NOT C# (owner ruling 2026-07-25, option (b)). Unity's `skill-create` writes a `.cs` that
 * Unity hot-compiles on domain reload; Unreal has no such path. Emitting C# into the .NET sidecar was
 * evaluated and rejected: `bridge/src` has no Roslyn and no AssemblyLoadContext, and the sidecar ships
 * as a PREBUILT single-file binary an end user cannot recompile. So the generator emits C++ and says so.
 * Auto-triggering Live Coding is deliberately NOT attempted here (a later layer, once the generator is
 * proven) — the tool tells the caller what to do rather than pretending the tool is live.
 *
 * WHY THE PLUGIN'S EDITOR MODULE, not the host project. A generated skill must see the tool registry
 * (`UnrealMcpToolRegistry.h`) and its `UNREALMCPRUNTIME_API` symbols. Putting it in a GAME module means
 * that module takes a hard dependency on `UnrealMcpRuntime`, which — per the plugin's own README/
 * CLAUDE.md — OVERRIDES a consumer's `TargetDenyList` and drags Unreal-MCP into packaged game builds.
 * The editor module already depends on the runtime module, is stripped from Game targets by Type, and is
 * rebuilt by the very same UBT invocation the user runs anyway.
 *
 * The generator + validator are pure and exported here so the Automation specs assert the exact emitted
 * text without a live editor or a real compile.
 */
namespace UnrealMcpSkillTools
{
	/** The generated skill's tool id — kept as a named constant so the specs and the docs cannot drift. */
	UNREALMCPEDITOR_API extern const TCHAR* const SkillCreateToolId;

	/** One declared parameter of a generated skill (mirrors FUnrealMcpParamSpec's declarable subset). */
	struct UNREALMCPEDITOR_API FSkillParamSpec
	{
		FString Name;                 // camelCase parameter name
		FString Type = TEXT("string");// "string" | "integer" | "number" | "boolean"
		FString Description;
		bool bRequired = false;
	};

	/** Everything the generator needs to emit one skill file. Plain data — no engine state. */
	struct UNREALMCPEDITOR_API FSkillSpec
	{
		FString ToolId;               // kebab-case, e.g. "my-skill"
		FString Title;                // human label; defaults to a Title-Cased ToolId when empty
		FString Description;          // the full tool description (becomes the SKILL.md body)
		FString SkillDescription;     // optional SHORT description for the SKILL.md front-matter
		TArray<FSkillParamSpec> Params;
		FString Body;                 // the C++ statements of the handler body (must return a FUnrealMcpToolResult)
		bool bReadOnlyHint = false;
		bool bDestructiveHint = false;
		bool bIdempotentHint = false;
	};

	/** True iff @p Type is one of the four scalar JSON types the generator can declare. */
	UNREALMCPEDITOR_API bool IsSupportedSkillParamType(const FString& Type);

	/**
	 * Validate @p Spec for generation. Checks the kebab-case tool id (the registry's own pattern), a
	 * non-empty description + body, unique camelCase-ish parameter names, and supported param types.
	 * Returns false with a caller-facing reason in @p OutError on the FIRST problem.
	 */
	UNREALMCPEDITOR_API bool ValidateSkillSpec(const FSkillSpec& Spec, FString& OutError);

	/** "my-skill" -> "MySkill". The unity-build-safe unique symbol stem for a generated file. */
	UNREALMCPEDITOR_API FString SkillSymbolFromToolId(const FString& ToolId);

	/** "my-skill" -> "My Skill". The default Title when the caller supplied none. */
	UNREALMCPEDITOR_API FString DefaultTitleFromToolId(const FString& ToolId);

	/** The generated file's name for @p ToolId, e.g. "UnrealMcpSkill_MySkill.cpp". */
	UNREALMCPEDITOR_API FString SkillFileNameFromToolId(const FString& ToolId);

	/**
	 * Emit the complete, compilable C++ translation unit for @p Spec. PURE + deterministic (no disk, no
	 * engine state) so a spec can assert the exact text. The caller must have validated the spec first —
	 * every embedded string is escaped, but an invalid tool id would still produce a file the registry
	 * rejects at runtime.
	 */
	UNREALMCPEDITOR_API FString BuildSkillSourceCode(const FSkillSpec& Spec);

	/** Escape @p In for embedding inside a C++ TEXT("...") literal (backslash, quote, CR/LF, tab). */
	UNREALMCPEDITOR_API FString EscapeCppStringLiteral(const FString& In);

	/**
	 * Absolute directory generated skills are written into:
	 * `<UnrealMCP plugin>/Source/UnrealMcpEditor/Private/Tools/Skills`. Returns an EMPTY string when the
	 * plugin's C++ source is not on disk (a precompiled / Fab install), which is the honest "cannot
	 * generate here" signal the tool surfaces instead of writing into a folder nothing will compile.
	 */
	UNREALMCPEDITOR_API FString GetGeneratedSkillsDir();

	/** Register the skill family into @p Registry (docs/ARCHITECTURE.md §3.3). */
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}
