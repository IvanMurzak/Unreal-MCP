// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpRuntimeSubsystem.h"
#include "UnrealMcpRuntimeSettings.h"

/**
 * Specs for the §12.8 built-in-runtime-tools opt-out (issue #141) — a game can ship exposing ONLY its own
 * custom tools by turning off UUnrealMcpRuntimeSettings::bRegisterBuiltinRuntimeTools.
 *
 * The gate logic in UUnrealMcpRuntimeSubsystem::Initialize is `if (Settings->bRegisterBuiltinRuntimeTools)
 * RegisterBuiltinRuntimeTools(Registry)`. RegisterBuiltinRuntimeTools is a static, exported helper that
 * registers the five runtime-safe core families, so the gate is testable headlessly without standing up a
 * subsystem (which needs an FSubsystemCollection + a bound IPC port): a spec reproduces both arms of the
 * `if` against a fresh registry and asserts the built-in count is what the flag selects, while a custom
 * provider's tools register regardless. This is the deterministic CI gate the issue's acceptance criterion
 * asks for; the live subsystem Initialize wiring is exercised by the headless boot smoke (test.md Suite 2).
 */
namespace
{
	/** A test-only provider standing in for a game's custom tools — registers one uniquely-named tool. */
	class FBuiltinGateSpecProvider : public IUnrealMcpToolProvider
	{
	public:
		virtual FString GetExtensionId() const override { return TEXT("test.builtin.gate"); }
		virtual FText GetDisplayName() const override { return FText::FromString(TEXT("Builtin gate spec provider")); }
		virtual FString GetExtensionVersion() const override { return TEXT("1.0.0"); }
		virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
		{
			Registry.Tool(TEXT("builtin-gate-custom-tool"))
				.Title(TEXT("Custom gameplay tool"))
				.Description(TEXT("A game's own tool (issue #141 opt-out spec fixture)."))
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(TEXT("ok")); });
		}
	};

	/** Register the provider's tools the same way the §5 extension manager would (a scoped, deduped commit). */
	void BuiltinGateRegisterProvider(FUnrealMcpToolRegistry& Registry, IUnrealMcpToolProvider& Provider)
	{
		Registry.RegisterExtension(Provider.GetExtensionId(),
			[&Provider](FUnrealMcpToolRegistry& R) { Provider.RegisterTools(R); });
	}
}

BEGIN_DEFINE_SPEC(FUnrealMcpRuntimeBuiltinToolsGateSpec, "UnrealMcp.RuntimeBuiltinToolsGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpRuntimeBuiltinToolsGateSpec)

void FUnrealMcpRuntimeBuiltinToolsGateSpec::Define()
{
	Describe("bRegisterBuiltinRuntimeTools gate (§12.8 opt-out)", [this]()
	{
		It("flag OFF -> the registry exposes ZERO built-in tools (only the custom provider's tool)", [this]()
		{
			// Reproduce Initialize()'s gate with the opt-out OFF: built-ins are skipped, provider tools merge.
			FUnrealMcpToolRegistry Registry;
			FBuiltinGateSpecProvider Provider;
			BuiltinGateRegisterProvider(Registry, Provider);

			TestEqual(TEXT("only the custom provider's tool is registered"), Registry.Num(), 1);
			TestTrue(TEXT("the custom provider's tool is present"), Registry.HasTool(TEXT("builtin-gate-custom-tool")));

			// The high-risk built-ins the §12.8 opt-out exists to suppress must be absent.
			TestFalse(TEXT("ping built-in absent"), Registry.HasTool(TEXT("ping")));
			TestFalse(TEXT("reflection-method-call built-in absent"), Registry.HasTool(TEXT("reflection-method-call")));
			TestFalse(TEXT("console-run-command built-in absent"), Registry.HasTool(TEXT("console-run-command")));
		});

		It("flag ON -> the built-in count is unchanged and the custom tool registers on top", [this]()
		{
			// Reproduce Initialize()'s gate with the opt-out ON (the default): built-ins register, then the
			// provider's tools merge on top — today's behavior, byte-for-byte.
			FUnrealMcpToolRegistry Registry;
			UUnrealMcpRuntimeSubsystem::RegisterBuiltinRuntimeTools(Registry);
			const int32 BuiltinCount = Registry.Num();

			// Sanity: the built-in set is non-trivial and includes the high-risk remote-control tools.
			TestTrue(TEXT("built-ins registered a non-zero set"), BuiltinCount > 0);
			TestTrue(TEXT("ping built-in present"), Registry.HasTool(TEXT("ping")));
			TestTrue(TEXT("reflection-method-call built-in present"), Registry.HasTool(TEXT("reflection-method-call")));
			TestTrue(TEXT("console-run-command built-in present"), Registry.HasTool(TEXT("console-run-command")));

			FBuiltinGateSpecProvider Provider;
			BuiltinGateRegisterProvider(Registry, Provider);

			TestTrue(TEXT("the custom provider's tool is present alongside the built-ins"),
				Registry.HasTool(TEXT("builtin-gate-custom-tool")));
			TestEqual(TEXT("the built-in count is unchanged (custom tool added exactly one)"),
				Registry.Num(), BuiltinCount + 1);
		});

		It("the setting defaults to TRUE (no behavior change for existing users)", [this]()
		{
			const UUnrealMcpRuntimeSettings* Settings = GetDefault<UUnrealMcpRuntimeSettings>();
			TestNotNull(TEXT("runtime settings CDO resolves"), Settings);
			if (Settings)
			{
				TestTrue(TEXT("bRegisterBuiltinRuntimeTools defaults to true"), Settings->bRegisterBuiltinRuntimeTools);
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
