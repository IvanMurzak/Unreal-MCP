// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntimeSettings.h"

UUnrealMcpRuntimeSettings::UUnrealMcpRuntimeSettings()
{
	// Default-off kill switch (§12.8 #4). Explicit here as well as in the header default so a config that
	// predates the property reads false rather than an uninitialized value.
	bRuntimeMcpEnabled = false;
}

FName UUnrealMcpRuntimeSettings::GetCategoryName() const
{
	// Group under "Plugins" in Project Settings (UDeveloperSettings default is "Game"). Matches where a
	// consumer expects to find the Unreal-MCP runtime toggle.
	return FName(TEXT("Plugins"));
}
