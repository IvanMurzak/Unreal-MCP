// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

/**
 * Shared external-process helpers for the sidecar / local-server managers (docs/ARCHITECTURE.md §6 / §7).
 * The .NET RID resolver and the +x helper were verbatim copies in both managers; this single
 * UNREALMCPRUNTIME_API surface owns one definition of each.
 */
namespace FUnrealMcpProcessUtil
{
	/**
	 * Resolve the .NET runtime-identifier directory name the bundled apphost lives under (§6.2 / §12.5):
	 * win-x64 / linux-x64 / osx-arm64 / osx-x64. On Apple Silicon the PHYSICAL host CPU is probed via
	 * `sysctlbyname("hw.optional.arm64")` (correct even under a Rosetta-translated editor); osx-arm64 is
	 * chosen only when @p bArm64SliceExists (so a missing arm64 build falls back to osx-x64 under Rosetta
	 * rather than failing). Returns empty on a non-desktop platform (console/mobile cannot spawn .NET).
	 */
	UNREALMCPRUNTIME_API FString ResolveDotNetRid(bool bArm64SliceExists);

	/**
	 * Best-effort `chmod 0755` (rwxr-xr-x) on @p Path so a bundled apphost is executable (§6.6). A no-op on
	 * Windows (no POSIX mode bits). NOT fatal on failure — the bit may already be set by the packager — so it
	 * logs a warning and returns false rather than aborting the spawn. Returns true when the mode was set (or
	 * on Windows, where nothing is needed).
	 */
	UNREALMCPRUNTIME_API bool MakeExecutable(const FString& Path);
}
