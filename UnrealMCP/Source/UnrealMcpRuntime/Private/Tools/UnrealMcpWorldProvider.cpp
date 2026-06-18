// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpWorldProvider.h"

namespace FUnrealMcpWorldProvider
{
	// File-static so the runtime module owns no GEditor reference. The resolver is installed once on the
	// game thread during plugin startup and invoked on the game thread by tool bodies (§4), so a plain
	// static needs no synchronization. A family-unique name (GWorldResolver) per the unity-build ODR rule.
	static TFunction<UWorld*()> GUnrealMcpWorldResolver;

	UWorld* GetActiveWorld()
	{
		return GUnrealMcpWorldResolver ? GUnrealMcpWorldResolver() : nullptr;
	}

	void SetWorldResolver(TFunction<UWorld*()> Resolver)
	{
		GUnrealMcpWorldResolver = MoveTemp(Resolver);
	}

	void ClearWorldResolver()
	{
		GUnrealMcpWorldResolver = nullptr;
	}
}
