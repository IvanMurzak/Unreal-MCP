// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Tools/UnrealMcpGeneratedSkills.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcpGeneratedSkills
{
	namespace
	{
		/**
		 * Function-local static (constructed on first use) so a generated skill's file-scope registrar can run
		 * at ANY point in static-init order and still see a live array — a namespace-scope TArray could still be
		 * uninitialized when another translation unit's registrar ran first (static-init-order fiasco).
		 */
		TArray<FRegisterFn>& PendingRegistrations()
		{
			static TArray<FRegisterFn> Registrations;
			return Registrations;
		}

		/**
		 * The ExtensionId every generated skill is stamped with. NOT "core" — that is the trusted path that
		 * replaces same-key entries without validating. It is also the §7 Tools-window family label, and it is
		 * never swept by RemoveToolsForExtension (the extension manager only removes ids it discovered itself).
		 */
		const TCHAR* const GeneratedSkillExtensionId = TEXT("generated-skill");
	}

	void Add(FRegisterFn Fn)
	{
		if (Fn != nullptr)
			PendingRegistrations().Add(Fn);
	}

	int32 Num()
	{
		return PendingRegistrations().Num();
	}

	void Register(FUnrealMcpToolRegistry& Registry)
	{
		const TArray<FRegisterFn>& Registrations = PendingRegistrations();
		if (Registrations.Num() == 0)
			return;

		// Commit through an EXTENSION SCOPE, never the core path. A generated skill is machine-authored (and
		// the emitted banner invites hand-editing), so it is NOT trusted core code. The core path
		// deliberately "replaces any same-key entry" without validating (UnrealMcpRegistry.h Commit), so
		// registering here on the core path — at ANY point in the order — would let a generated
		// `actor-create.cpp` silently REPLACE the built-in handler for every AI agent, and would skip the
		// kebab-id validation entirely. Registering LAST makes that worse, not safer: last writer wins.
		// The extension scope is the machinery that already implements the intended rule — it stamps the
		// owning id, validates each entry, and rejects a collision first-wins with "extensions may not
		// shadow core" — so a core id always survives and the offending skill is reported, not silently applied.
		const FUnrealMcpExtensionRegistrationResult Result =
			Registry.RegisterExtension(GeneratedSkillExtensionId, [&Registrations](FUnrealMcpToolRegistry& ScopedRegistry)
			{
				for (FRegisterFn Fn : Registrations)
					Fn(ScopedRegistry);
			});

		for (const FString& Error : Result.Errors)
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] generated skill rejected: %s"), *Error);

		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] registered %d of %d generated skill file(s)."),
			Result.ToolsRegistered, Registrations.Num());
	}
}
