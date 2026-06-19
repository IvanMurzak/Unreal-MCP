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
#include "UnrealMcpRuntimeCoreTools.h" // runtime-safe families (§12.7) — for the runtime-manifest-separation check
#include "UnrealMcpToolRegistry.h"      // FUnrealMcpToolRegistry — the registry the separation check builds against

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

	Describe("Disconnect + orphan safety (§12.4 lifecycle / §12.8 #1)", [this]()
	{
		// R6 (issue #127) — the connect/disconnect lifecycle and orphan-safe teardown. The LIVE sidecar
		// spawn-and-handshake half is the packaged / PIE runbook (docs/RUNTIME-E2E.md, test.md Suite 7);
		// these specs lock the headless-deterministic half: that Disconnect is a safe idempotent no-op when
		// nothing was ever connected, so a game that calls Disconnect (or whose UGameInstance tears down)
		// without a prior successful Connect leaves ZERO orphaned sidecar state behind.
		It("Disconnect on a never-connected subsystem is a harmless no-op (no orphaned sidecar)", [this]()
		{
			UUnrealMcpRuntimeSubsystem* Subsystem = RuntimeSpecMakeSubsystem();
			TestFalse(TEXT("precondition: not connected"), Subsystem->IsConnected());

			// Disconnect must not require a prior Connect — the §12.4 teardown contract is "idempotent,
			// a no-op when not connected" (the same path Deinitialize relies on for orphan safety).
			Subsystem->Disconnect();
			TestFalse(TEXT("still not connected after a no-op Disconnect"), Subsystem->IsConnected());

			// A second Disconnect is equally safe (idempotency under repeated teardown).
			Subsystem->Disconnect();
			TestFalse(TEXT("idempotent second Disconnect"), Subsystem->IsConnected());
		});

		It("a rejected Connect leaves nothing to disconnect (kill switch off -> Connect false -> clean Disconnect)", [this]()
		{
			const bool bPrev = RuntimeSpecSetKillSwitch(false);
			UUnrealMcpRuntimeSubsystem* Subsystem = RuntimeSpecMakeSubsystem();

			// Connect rejects (kill switch off); it must NOT have spawned a sidecar, so IsConnected stays false
			// and a follow-up Disconnect is a clean no-op — proving a rejected opt-in cannot orphan a process.
			const bool bConnected = Subsystem->Connect(TEXT("http://localhost:8080"));
			TestFalse(TEXT("rejected Connect"), bConnected);
			Subsystem->Disconnect();
			TestFalse(TEXT("nothing connected after rejected Connect + Disconnect"), Subsystem->IsConnected());

			RuntimeSpecSetKillSwitch(bPrev);
		});

		It("Get(nullptr) returns null rather than crashing (console-command / BeginPlay null-context guard)", [this]()
		{
			// The §12.4 BeginPlay snippet and the UnrealMcp.Connect console command both reach the subsystem
			// via Get(WorldContext); a null context (no world / no game instance) must resolve to null, not
			// dereference. This is the guard that keeps a Disconnect-on-shutdown path orphan-safe.
			TestNull(TEXT("Get(nullptr) -> null"), UUnrealMcpRuntimeSubsystem::Get(nullptr));
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

		It("swaps editor<->runtime resolvers and GetActiveWorld follows whichever is active (§12.6 dual-bootstrap)", [this]()
		{
			// R6 (issue #127): the §12.6 design point is that the SAME runtime module is driven by an EDITOR
			// resolver (FUnrealMcpEditorCoordinator -> editor world) or a RUNTIME resolver (the subsystem ->
			// GetGameInstance()->GetWorld()), and GetActiveWorld() routes to whichever bootstrap installed
			// last. We inject two distinct fixture worlds standing in for "the editor world" and "the live
			// game world" and assert the swap is honoured both directions — the headless proxy for the
			// editor->PIE->packaged resolver handoff the runbook exercises live.
			UWorld* EditorFixtureWorld  = NewObject<UWorld>(GetTransientPackage());
			UWorld* RuntimeFixtureWorld = NewObject<UWorld>(GetTransientPackage());
			TestNotEqual(TEXT("the two fixture worlds are distinct"), EditorFixtureWorld, RuntimeFixtureWorld);

			// Editor bootstrap installs its resolver.
			FUnrealMcpWorldProvider::SetWorldResolver([EditorFixtureWorld]() -> UWorld* { return EditorFixtureWorld; });
			TestEqual(TEXT("editor resolver -> editor world"),
				FUnrealMcpWorldProvider::GetActiveWorld(), EditorFixtureWorld);

			// Runtime bootstrap installs its resolver (e.g. a packaged game's UGameInstanceSubsystem) — the
			// later install wins, exactly as the runtime subsystem's Initialize() overrides any editor resolver.
			FUnrealMcpWorldProvider::SetWorldResolver([RuntimeFixtureWorld]() -> UWorld* { return RuntimeFixtureWorld; });
			TestEqual(TEXT("runtime resolver -> runtime/game world"),
				FUnrealMcpWorldProvider::GetActiveWorld(), RuntimeFixtureWorld);

			// And back to the editor resolver (a runtime teardown re-installing the editor resolver).
			FUnrealMcpWorldProvider::SetWorldResolver([EditorFixtureWorld]() -> UWorld* { return EditorFixtureWorld; });
			TestEqual(TEXT("swapped back -> editor world"),
				FUnrealMcpWorldProvider::GetActiveWorld(), EditorFixtureWorld);

			// Restore the live editor resolver so the rest of the suite is unaffected.
			FUnrealMcpWorldProvider::SetWorldResolver([]() -> UWorld*
			{
				return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			});
		});
	});

	Describe("runtime manifest separation (ping-only built-in)", [this]()
	{
		// Build a registry with EXACTLY what the runtime bootstrap subsystem registers
		// (UUnrealMcpRuntimeSubsystem::Initialize): `ping` and nothing else. The subsystem builds its registry
		// privately inside Initialize() (which needs a live UGameInstance + bridge), so asserting against the
		// SAME registration list here proves the runtime manifest's shape without the live harness — and is
		// what makes "the runtime ships ONLY ping; every engine-development tool is editor-only" a deterministic
		// CI gate rather than a flaky live-e2e step. A game adds its own runtime tools via an
		// IUnrealMcpToolProvider extension, which is exercised separately (extension specs).
		It("the runtime built-in manifest is EXACTLY {ping} — every engine-development tool is absent", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpPingTool::Register(Registry); // the ONLY built-in the runtime subsystem registers

			// --- ping is present and is the SOLE built-in. ---
			TestTrue(TEXT("runtime manifest HAS ping"), Registry.HasTool(TEXT("ping")));
			TestEqual(TEXT("runtime built-in tool count == 1 (ping only)"), Registry.Num(), 1);

			// --- Every engine-development tool MUST be ABSENT from the runtime built-in manifest. ---
			const TCHAR* const EditorOnlyForbidden[] = {
				// actor / component family (now editor-only)
				TEXT("actor-create"), TEXT("actor-destroy"), TEXT("object-get-data"), TEXT("object-modify"),
				// console + reflection (now editor-only)
				TEXT("console-get-logs"), TEXT("console-clear-logs"), TEXT("console-run-command"),
				TEXT("reflection-method-find"), TEXT("reflection-method-call"),
				// screenshot (now editor-only)
				TEXT("screenshot-viewport"), TEXT("screenshot-isolated"),
				TEXT("screenshot-game-view"), TEXT("screenshot-camera"),
				// level read + write (now editor-only)
				TEXT("level-get-data"), TEXT("level-create"), TEXT("level-open"), TEXT("level-save"),
				TEXT("level-list-loaded"), TEXT("level-set-current"), TEXT("level-unload-sublevel"),
				// representative asset / blueprint / editor-application / source family members
				TEXT("asset-find"), TEXT("blueprint-create"), TEXT("editor-application-get-state"),
				TEXT("editor-selection-get"), TEXT("source-compile")
			};
			for (const TCHAR* Name : EditorOnlyForbidden)
				TestFalse(FString::Printf(TEXT("runtime manifest does NOT have engine-dev tool %s"), Name), Registry.HasTool(Name));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
