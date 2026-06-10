// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpLog.h"

/**
 * Trivial always-passing spec proving the Automation test plumbing works:
 * the UnrealMcpEditorTests module compiles, loads, and its specs are
 * discoverable under the `UnrealMcp.` filter prefix (design §9.3).
 * Real specs (schema generator golden files, registry dedup/ordering,
 * dispatcher timeout+cancel, config precedence) land with their tasks.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpScaffoldSpec, "UnrealMcp.Scaffold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpScaffoldSpec)

void FUnrealMcpScaffoldSpec::Define()
{
	Describe("Repo scaffold", [this]()
	{
		It("compiles, loads, and runs an always-passing spec", [this]()
		{
			TestTrue(TEXT("scaffold placeholder expectation"), true);
		});

		It("registers the dedicated LogUnrealMcp category", [this]()
		{
			TestEqual(TEXT("log category name"),
				LogUnrealMcp.GetCategoryName(), FName(TEXT("LogUnrealMcp")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
