// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

// Prologue of the vendored RQL runtime (Source/ThirdParty/RQL, PolyForm Shield 1.0.0 — see its README.md). Each
// UnrealMcpRqlVendor_<file>.cpp compiles exactly one RQL source file between THIRD_PARTY_INCLUDES_START/END, so engine
// warning policy never applies to code we do not own and the files stay byte-identical to upstream.

#include "HAL/Platform.h"

// Shortest round-trip float formatting (std::to_chars / std::from_chars for floating point): MSVC's standard library has
// it; libc++ gained it only recently, so Mac/Linux keep RQL's portable %.17g fallback.
#if PLATFORM_WINDOWS
#define RQL_HAVE_FLOAT_CHARCONV 1
#endif
