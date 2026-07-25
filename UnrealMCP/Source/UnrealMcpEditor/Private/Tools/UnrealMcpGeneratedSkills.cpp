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

		for (FRegisterFn Fn : Registrations)
			Fn(Registry);

		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] registered %d generated skill file(s)."), Registrations.Num());
	}
}
