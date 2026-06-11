// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

class FUnrealMcpToolRegistry;

/**
 * The C++ source / script tool family (docs/ARCHITECTURE.md §10 "source family", issue #18).
 *
 * Six kebab-case CORE tools that let an AI scaffold, read, edit, list and compile project C++ and
 * receive a machine-readable build report — the static-compile reinterpretation of Unity-MCP's
 * `Script.*` family. Every file operation is JAILED to the loaded project's `Source/` directory.
 *
 * The jail helper and the compiler-diagnostic parser are exported here (separate from the family's
 * Register entry point in UnrealMcpCoreTools.h) so the Automation specs in the sibling
 * UnrealMcpEditorTests module can exercise them directly, fast and deterministically, without a
 * live editor or a real UBT invoke.
 */
namespace UnrealMcpSourceTools
{
	/** One parsed compiler diagnostic — a single row of the §3 structured build report. */
	struct UNREALMCPEDITOR_API FSourceDiagnostic
	{
		FString File;        // absolute or build-relative source path the compiler reported
		int32 Line = 0;      // 1-based line, 0 when the format carried none
		FString Severity;    // "error" | "warning"
		FString Message;     // the compiler text (code + description), trimmed
	};

	/** Outcome of resolving a caller-supplied path against the project Source/ jail. */
	struct UNREALMCPEDITOR_API FJailedPath
	{
		bool bOk = false;
		FString FullPath;    // canonicalized absolute path (valid only when bOk)
		FString RelPath;     // path relative to the jail root, '/'-separated (valid only when bOk)
		FString Error;       // human-readable reason (valid only when !bOk)
	};

	/** Absolute, normalized jail root = <Project>/Source for the currently loaded project. */
	UNREALMCPEDITOR_API FString GetProjectSourceRoot();

	/**
	 * Canonicalize @p InPath (relative to the jail root, or absolute) and confirm it stays inside the
	 * jail. Rejects `..` traversal and absolute paths that escape, and — best effort — a junction /
	 * symlink whose on-disk target escapes the jail. @p JailRoot lets specs supply a temp root.
	 */
	UNREALMCPEDITOR_API FJailedPath ResolveJailedPath(const FString& JailRoot, const FString& InPath);

	/** Compute the MODULE_API export macro token for a module name (e.g. "MyGame" -> "MYGAME_API"). */
	UNREALMCPEDITOR_API FString ModuleApiMacro(const FString& ModuleName);

	/**
	 * Parse compiler diagnostics out of raw build output (MSVC `file(line): error Cxxxx: msg` and
	 * clang `file:line:col: error: msg` forms). Pure + deterministic: link-stage failures
	 * (`LINK : fatal error LNK....`, which carry no `file(line)`) are intentionally NOT reported as
	 * compiler diagnostics, so a "compiler-clean but the loaded editor DLL is locked" build still
	 * reports zero errors. Duplicate rows are collapsed.
	 */
	UNREALMCPEDITOR_API void ParseDiagnostics(const FString& BuildOutput, TArray<FSourceDiagnostic>& OutDiagnostics);

	/** Register the source family into @p Registry (docs/ARCHITECTURE.md §3.3). */
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry);
}
