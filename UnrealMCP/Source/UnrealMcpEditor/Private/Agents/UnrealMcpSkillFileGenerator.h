// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;
struct FUnrealMcpRegisteredTool;

/**
 * Per-agent SKILL.md generator (docs/ARCHITECTURE.md §7, issue #53 Phase C) — the Unreal/C++ analog of Unity's
 * SkillFileGenerator + UnitySkillFileGenerator. It writes one `<skillsRoot>/<tool-name>/SKILL.md` per registered
 * tool, sourced ENTIRELY from the plugin's own C++ tool registry (FUnrealMcpToolRegistry — the 8 core tool
 * families / ~62 tools), NOT from Unity's .NET tool reflection. This is the deliberate skill-content-source choice:
 *
 *   - Unity generates SKILL.md from its .NET tool set via [AiSkillDescription]/[AiSkillBody] reflected by the
 *     shared McpPlugin.GenerateSkillFiles. That generator lives in the bridge SIDECAR and knows ONLY about .NET
 *     tools; it has no visibility into Unreal's C++ tool registry, so reusing it verbatim would produce EMPTY or
 *     wrong skill docs for an Unreal project.
 *   - The faithful Unreal analog is to generate from FUnrealMcpToolRegistry, which carries rich per-tool metadata
 *     (Title, Description, declared Params with name/type/required/description, behavioural hints, the JSON input
 *     schema) headless-enumerable on the game thread WITHOUT a live bridge or editor connection. The resulting
 *     docs describe the ACTUAL Unreal tools an agent can call — accurate, not misleading.
 *
 * Idempotent: each SKILL.md is unconditionally overwritten on every run, so re-generating after a tool set change
 * cleanly refreshes the docs. Token discipline (§8): a skill file is PURE tool documentation — the generator never
 * reads or writes any token/secret/host/connection value. The "How to Call" block uses a `<token>` PLACEHOLDER
 * only (never the real bearer), matching Unity's secret-free SKILL.md output.
 *
 * The markdown builder is a static pure function (no disk access) so the Automation specs assert the exact output
 * shape for a hand-built tool; PopulateCoreRegistry() fills a bare registry with every core family so a spec (or
 * the panel) can generate the full set without standing up the runtime.
 */
class FUnrealMcpSkillFileGenerator
{
public:
	/** Outcome of a generation run (for the panel's status line + the specs). */
	struct FResult
	{
		bool bSuccess = false;
		int32 FilesWritten = 0;
		FString SkillsRootAbsolute; // the resolved folder the files were written under
		FString Error;              // non-empty on failure (e.g. empty root, write failure)
	};

	/**
	 * Generate one SKILL.md per tool in @p Registry under @p SkillsRootAbsolute (an absolute folder). Creates the
	 * folder tree as needed. Overwrites existing files (idempotent). Every tool in the registry is documented
	 * regardless of its enabled flag (the §7 Tools window's blocklist hides tools at runtime, but their docs stay
	 * useful). Returns a result with the count written. Never throws.
	 */
	static UNREALMCPEDITOR_API FResult Generate(const FUnrealMcpToolRegistry& Registry, const FString& SkillsRootAbsolute);

	/**
	 * Build the SKILL.md markdown for ONE tool. Pure (no disk access) — the spec-friendly heart of the generator.
	 * Shape: YAML front-matter (name + description) → `# <Title>` → description body → `## Input` (param table +
	 * JSON Schema) → `## How to Call` (the `unreal-mcp-cli run-tool` form with a `<token>` placeholder only). The
	 * YAML description is the tool's Description, single-lined + length-capped to the Agent-Skills limit so the
	 * front-matter stays valid. NO secret ever appears.
	 */
	static UNREALMCPEDITOR_API FString BuildSkillMarkdown(const FUnrealMcpRegisteredTool& Tool);

	/**
	 * Sanitize a tool name into a safe folder name: lowercase, alphanumeric runs joined by single hyphens, no
	 * leading/trailing/consecutive hyphens. A name that sanitizes to empty yields "tool". Tool ids are already
	 * kebab-case, so this is a defensive normalization (and the seam an extension tool with an odd id flows through).
	 */
	static UNREALMCPEDITOR_API FString SanitizeSkillFolderName(const FString& ToolName);

	/**
	 * Populate a bare registry with EVERY core tool family (ping + the 8 families). Headless-safe and
	 * bridge-independent — registration only declares descriptors, it never touches a world or the bridge. The
	 * panel and the specs use this to enumerate the full tool set for skill generation without the runtime.
	 */
	static UNREALMCPEDITOR_API void PopulateCoreRegistry(FUnrealMcpToolRegistry& Registry);

	/** The max length of the YAML `description:` value (the Codex/Anthropic Agent-Skills limit). */
	static constexpr int32 MaxSkillDescriptionLength = 1024;
};
