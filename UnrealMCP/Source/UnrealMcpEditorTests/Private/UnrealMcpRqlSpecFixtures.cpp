// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSpecFixtures.h"

#include "IUnrealMcpRqlModule.h"

void UUnrealMcpRqlSpecApi::Boom(const FString& Message)
{
	UnrealMcpRql::Fail(Message);
}
