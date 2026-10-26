// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpWorldProvider.h"

namespace FUnrealMcpWorldProvider
{
	// File-static so the runtime module owns no GEditor reference. Installed and invoked on the game thread
	// (§4), so a plain static needs no synchronization. Family-unique names per the unity-build ODR rule.
	struct FUnrealMcpWorldResolverEntry
	{
		const void* Owner;
		TFunction<UWorld*()> Resolver;
	};
	static TArray<FUnrealMcpWorldResolverEntry> GUnrealMcpWorldResolvers;

	UWorld* GetActiveWorld()
	{
		return GUnrealMcpWorldResolvers.Num() > 0 ? GUnrealMcpWorldResolvers.Last().Resolver() : nullptr;
	}

	void SetWorldResolver(const void* Owner, TFunction<UWorld*()> Resolver)
	{
		ClearWorldResolver(Owner);
		GUnrealMcpWorldResolvers.Add({ Owner, MoveTemp(Resolver) });
	}

	void ClearWorldResolver(const void* Owner)
	{
		GUnrealMcpWorldResolvers.RemoveAll([Owner](const FUnrealMcpWorldResolverEntry& Entry) { return Entry.Owner == Owner; });
	}
}
