// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * The registration bus for GENERATED skills — the C++ tool files `unreal-skill-create` writes into
 * `UnrealMCP/Source/UnrealMcpEditor/Private/Tools/Skills/` (docs/ARCHITECTURE.md §2.4).
 *
 * WHY a bus instead of patching the coordinator: every hand-written family is wired by an explicit
 * `UnrealMcpXTools::Register(*Registry)` line in UnrealMcpEditorCoordinator.cpp. Having a TOOL rewrite
 * that file on every generated skill would mean machine-patching a hand-maintained source file — not
 * idempotent, merge-hostile, and one bad edit away from breaking the whole plugin's boot path. Instead a
 * generated skill self-registers: each generated .cpp declares a file-scope
 * FUnrealMcpGeneratedSkillRegistrar whose constructor appends its Register function here at static-init
 * time, and the coordinator calls UnrealMcpGeneratedSkills::Register(*Registry) exactly ONCE (added in
 * the same change that introduced this header, never touched again). Adding or deleting a generated
 * skill is then purely "drop / remove a .cpp and rebuild" — UBT picks up new .cpp files in a module
 * automatically.
 *
 * Static-init-order safety: the backing array is a function-local static (constructed on first use), so a
 * registrar in ANY translation unit can run before or after any other and still find a live array. The
 * modules are unity-built, so a generated file's namespace/symbol names must be unique — the generator
 * derives them from the tool id (see UnrealMcpSkillTools::SkillSymbolFromToolId).
 */
namespace UnrealMcpGeneratedSkills
{
	/** Signature of a generated skill's registration entry point (matches a hand-written family's Register). */
	using FRegisterFn = void (*)(FUnrealMcpToolRegistry&);

	/** Append @p Fn to the pending registration list. Called from static init; never null-checked away. */
	UNREALMCPEDITOR_API void Add(FRegisterFn Fn);

	/** How many generated skills self-registered into this build (0 in a clean checkout). */
	UNREALMCPEDITOR_API int32 Num();

	/**
	 * Run every self-registered generated skill against @p Registry, in link order, inside an EXTENSION
	 * SCOPE (ExtensionId `generated-skill`) — never the core path. Generated skills are machine-authored
	 * and hand-editable, so they are untrusted: the extension scope validates each entry and rejects a
	 * collision first-wins ("extensions may not shadow core"), which is what actually stops a generated
	 * `actor-create` from replacing the built-in. ORDER IS NOT THE GUARD — the core path replaces any
	 * same-key entry, so committing there would let a generated id win outright (and running LAST would
	 * make that certain). Called once by the editor coordinator after the hand-written core families.
	 */
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}

/**
 * File-scope self-registrar a generated skill declares once:
 *
 *     static const FUnrealMcpGeneratedSkillRegistrar Registrar(&Register);
 *
 * Constructing it enrolls the function; there is nothing to unregister (generated skills live for the
 * module's lifetime, exactly like the hand-written families).
 */
struct UNREALMCPEDITOR_API FUnrealMcpGeneratedSkillRegistrar
{
	explicit FUnrealMcpGeneratedSkillRegistrar(UnrealMcpGeneratedSkills::FRegisterFn Fn)
	{
		UnrealMcpGeneratedSkills::Add(Fn);
	}
};
