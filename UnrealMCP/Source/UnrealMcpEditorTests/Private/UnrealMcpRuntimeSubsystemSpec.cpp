// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h" // a transient UGameInstance as the subsystem's Outer (ClassWithin=UGameInstance)
#include "Editor.h" // GEditor — the spec restores the editor world resolver after exercising the runtime one

#include "UnrealMcpRuntimeSubsystem.h"
#include "UnrealMcpRuntimeSettings.h"
#include "Tools/UnrealMcpWorldProvider.h"

/**
 * Specs for the R3 runtime bootstrap (docs/ARCHITECTURE.md §12.4 / §12.6 / §12.8). These exercise the parts
 * that do NOT require a live UGameInstance + sidecar:
 *  - the §12.8 #5 loopback-host policy (IsLoopbackHost), the pure gate the host-rejection branch uses;
 *  - the §12.8 #4 kill-switch and §12.8 #5 host rejection through Connect() on a bare (un-Initialized)
 *    subsystem object — those two gates run BEFORE the listener-armed gate, so a NewObject suffices to
 *    prove they reject (and that the security invariant "no connect path without an explicit, gated call"
 *    holds even for a fully-defaulted project — kill switch off → Connect false);
 *  - the §12.6 world-provider switching: a runtime-style resolver installs and GetActiveWorld follows it,
 *    ClearWorldResolver returns to the null (no-resolver) state.
 *
 * The live sidecar connect → server → ping wire path is the live-e2e / PIE runbook (test.md Suite 7), not
 * unit-asserted here (no headless sidecar+server harness in a dev checkout).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRuntimeSubsystemSpec, "UnrealMcp.RuntimeSubsystem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Spec-unique helper (unity-build ODR rule): a fresh, un-Initialized subsystem object. Connect()'s
	// kill-switch + host gates run before the listener gate, so this is enough to assert their rejections.
	// UUnrealMcpRuntimeSubsystem (a UGameInstanceSubsystem) has ClassWithin=UGameInstance, so it must be
	// created with a UGameInstance Outer — a transient one suffices; NewObject into the transient PACKAGE
	// would trip the engine's "created in invalid Outer" ensure. The subsystem is NOT Initialize()'d here
	// (we never call Initialize), so the transient game instance never drives real bring-up.
	static UUnrealMcpRuntimeSubsystem* RuntimeSpecMakeSubsystem()
	{
		UGameInstance* OuterGI = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UUnrealMcpRuntimeSubsystem>(OuterGI);
	}

	// Spec-unique helper: set the kill switch and return the previous value so the test can restore it.
	static bool RuntimeSpecSetKillSwitch(bool bEnabled)
	{
		UUnrealMcpRuntimeSettings* Settings = GetMutableDefault<UUnrealMcpRuntimeSettings>();
		const bool bPrev = Settings->bRuntimeMcpEnabled;
		Settings->bRuntimeMcpEnabled = bEnabled;
		return bPrev;
	}

END_DEFINE_SPEC(FUnrealMcpRuntimeSubsystemSpec)

void FUnrealMcpRuntimeSubsystemSpec::Define()
{
	Describe("IsLoopbackHost (§12.8 #5 policy)", [this]()
	{
		It("accepts loopback hostnames / addresses (schemed and bare, with and without port)", [this]()
		{
			TestTrue(TEXT("localhost"),             UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("localhost")));
			TestTrue(TEXT("http://localhost:8080"), UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("http://localhost:8080")));
			TestTrue(TEXT("127.0.0.1"),             UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("127.0.0.1")));
			TestTrue(TEXT("http://127.0.0.1:9000/"),UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("http://127.0.0.1:9000/")));
			TestTrue(TEXT("127.5.5.5 (loopback /8)"),UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("127.5.5.5")));
			TestTrue(TEXT("[::1]:8080"),            UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("http://[::1]:8080")));
			TestTrue(TEXT("empty -> default localhost"), UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("")));
		});

		It("rejects non-loopback hosts", [this]()
		{
			TestFalse(TEXT("public IP"),       UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("203.0.113.7")));
			TestFalse(TEXT("LAN IP"),          UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("192.168.1.50")));
			TestFalse(TEXT("remote hostname"), UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("http://ai-game.dev")));
			TestFalse(TEXT("near-127 decoy"),  UUnrealMcpRuntimeSubsystem::IsLoopbackHost(TEXT("128.0.0.1")));
		});
	});

	Describe("Connect security gates (§12.8)", [this]()
	{
		It("returns false when the kill switch is OFF (§12.8 #4), even for a loopback host", [this]()
		{
			const bool bPrev = RuntimeSpecSetKillSwitch(false);
			UUnrealMcpRuntimeSubsystem* Subsystem = RuntimeSpecMakeSubsystem();

			// Connect logs a Warning (not Error) on rejection, so no AddExpectedError is needed — warnings do
			// not fail an Automation spec; only the boolean return is asserted.
			const bool bConnected = Subsystem->Connect(TEXT("http://localhost:8080"));
			TestFalse(TEXT("kill switch off -> Connect rejects"), bConnected);
			TestFalse(TEXT("not connected"), Subsystem->IsConnected());

			RuntimeSpecSetKillSwitch(bPrev);
		});

		It("returns false for a non-loopback host even when the kill switch is ON (§12.8 #5)", [this]()
		{
			const bool bPrev = RuntimeSpecSetKillSwitch(true);
			UUnrealMcpRuntimeSubsystem* Subsystem = RuntimeSpecMakeSubsystem();

			// Rejection is a Warning, not an Error (see above) — assert only the return value.
			const bool bConnected = Subsystem->Connect(TEXT("http://203.0.113.7:8080"), TEXT(""),
				EUnrealMcpRuntimeConnectionMode::Custom, /*bAllowRemoteHost*/ false);
			TestFalse(TEXT("remote host without bAllowRemoteHost -> Connect rejects"), bConnected);

			RuntimeSpecSetKillSwitch(bPrev);
		});

		It("a bare (un-Initialized) subsystem never reports a connection", [this]()
		{
			UUnrealMcpRuntimeSubsystem* Subsystem = RuntimeSpecMakeSubsystem();
			TestFalse(TEXT("fresh subsystem is not connected"), Subsystem->IsConnected());
			TestFalse(TEXT("fresh subsystem listener is not armed"), Subsystem->IsListenerArmed());
		});
	});

	Describe("World resolver switching (§12.6)", [this]()
	{
		// Save + restore the global resolver so these tests do not perturb the editor coordinator's installed
		// resolver (this spec runs inside a live editor where the coordinator already set the editor resolver).
		It("a runtime-style resolver is honoured by GetActiveWorld, and ClearWorldResolver returns to null", [this]()
		{
			UWorld* FakeWorld = NewObject<UWorld>(GetTransientPackage());

			// Install a runtime-style resolver (the subsystem installs `[this]{ GetGameInstance()->GetWorld() }`;
			// here we inject a fixed world to assert the provider plumbing follows whatever resolver is set).
			FUnrealMcpWorldProvider::SetWorldResolver([FakeWorld]() -> UWorld* { return FakeWorld; });
			TestEqual(TEXT("GetActiveWorld follows the installed resolver"),
				FUnrealMcpWorldProvider::GetActiveWorld(), FakeWorld);

			FUnrealMcpWorldProvider::ClearWorldResolver();
			TestNull(TEXT("after Clear, GetActiveWorld is null"), FUnrealMcpWorldProvider::GetActiveWorld());

			// Re-install the editor resolver so the rest of the suite (which may resolve worlds) is unaffected.
			FUnrealMcpWorldProvider::SetWorldResolver([]() -> UWorld*
			{
				return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			});
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
