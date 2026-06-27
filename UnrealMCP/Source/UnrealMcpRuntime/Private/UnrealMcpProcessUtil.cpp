// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpProcessUtil.h"
#include "UnrealMcpLog.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <cerrno>
#include <sys/stat.h>
#endif
#if PLATFORM_MAC
#include <sys/sysctl.h>
#endif

namespace FUnrealMcpProcessUtil
{
	FString ResolveDotNetRid(bool bArm64SliceExists)
	{
#if PLATFORM_WINDOWS
		(void)bArm64SliceExists;
		return TEXT("win-x64");
#elif PLATFORM_MAC
		// §6.2: select from the PHYSICAL host CPU, not the (possibly Rosetta-translated) editor arch — the
		// child should match the hardware. `hw.optional.arm64 == 1` reports Apple Silicon even under a
		// translated process (design R5). Fall back to osx-x64 (runs under Rosetta 2) when the arm64 slice is
		// absent, so a missing build is never fatal.
		int32 IsArm64 = 0;
		size_t Size = sizeof(IsArm64);
		if (sysctlbyname("hw.optional.arm64", &IsArm64, &Size, nullptr, 0) != 0)
			IsArm64 = 0; // probe unavailable → treat as Intel
		if (IsArm64 == 1 && bArm64SliceExists)
			return TEXT("osx-arm64");
		return TEXT("osx-x64");
#elif PLATFORM_LINUX
		(void)bArm64SliceExists;
		return TEXT("linux-x64");
#else
		(void)bArm64SliceExists;
		return FString();
#endif
	}

	bool MakeExecutable(const FString& Path)
	{
		if (Path.IsEmpty())
			return false;

#if PLATFORM_MAC || PLATFORM_LINUX
		// UE has no portable chmod; call the syscall directly (no shell, no PATH dependency). 0755 = rwxr-xr-x.
		const auto Utf8Path = StringCast<ANSICHAR>(*Path);
		if (chmod(Utf8Path.Get(), 0755) != 0)
		{
			UE_LOG(LogUnrealMcp, Warning,
				TEXT("[Unreal-MCP] could not set +x on '%s' (errno=%d); spawn may fail."), *Path, errno);
			// Not fatal — the bit may already be set by the packager; let the spawn attempt surface a real failure.
			return false;
		}
		return true;
#else
		return true; // Windows: no POSIX mode bits to set
#endif
	}
}
