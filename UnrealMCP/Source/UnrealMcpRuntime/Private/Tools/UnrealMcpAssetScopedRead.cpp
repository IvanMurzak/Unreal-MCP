// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Tools/UnrealMcpAssetScopedRead.h"
#include "Tools/UnrealMcpScopedRead.h"
#include "Dom/JsonObject.h"

namespace UnrealMcpAssetScopedRead
{
	TSharedPtr<FJsonObject> Apply(const TSharedPtr<FJsonObject>& Source, const TArray<FString>& Paths)
	{
		// The asset-family scoped read is now the shared FUnrealMcpScopedRead::Filter with the ASSET options:
		// exact-case segment match, DEEP-CLONE copied values (so callers can mutate the result freely), and NO
		// partial-branch residue for an unresolved path. The segment-split / descent / overlapping-path merge that
		// used to live here verbatim moved into the shared filter (the header's "consolidate post-merge" note).
		return FUnrealMcpScopedRead::Filter(Source, Paths, FScopedReadOptions::AssetDefaults());
	}
}
