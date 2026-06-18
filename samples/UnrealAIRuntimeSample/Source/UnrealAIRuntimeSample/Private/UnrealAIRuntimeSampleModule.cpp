// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

#include "Engine/Engine.h"          // GEngine->GetWorldContexts() — resolve the live game/PIE world
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h" // AWorldSettings::TimeDilation — the live gameplay state this tool drives

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIRuntimeSample, Log, All);

namespace
{
	/**
	 * Resolve the live game/PIE world this in-game tool operates on. An extension author does NOT have
	 * access to Unreal-MCP's private FUnrealMcpWorldProvider, so a real game extension resolves the world
	 * from the engine's world contexts (Engine-module only, runtime-safe), preferring a Game world and
	 * falling back to a PIE world so the sample also works when exercised inside the editor's PIE session.
	 * A sample-unique helper name per the unity-build ODR rule (docs/CLAUDE.md conventions).
	 */
	UWorld* RuntimeSampleResolveGameWorld()
	{
		if (!GEngine)
			return nullptr;

		UWorld* PieFallback = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World)
				continue;
			if (Context.WorldType == EWorldType::Game)
				return World; // a real packaged/standalone game world — the primary in-game target
			if (Context.WorldType == EWorldType::PIE && PieFallback == nullptr)
				PieFallback = World; // editor Play-In-Editor session — used when no standalone Game world exists
		}
		return PieFallback;
	}
}

/**
 * The sample's RUNTIME tool provider — the in-game analog of samples/UnrealAITemplate's editor provider
 * (docs/ARCHITECTURE.md §12.9, docs/EXTENSIONS.md "Runtime usage"). It contributes ONE gameplay tool,
 * `game-time-dilation`, that reads and (optionally) sets the live world's AWorldSettings::TimeDilation —
 * genuine gameplay state mutated on the live UWorld, proving a custom tool registered at runtime is
 * callable in a running game over a runtime MCP connection.
 */
class FUnrealAIRuntimeSampleProvider : public IUnrealMcpToolProvider
{
public:
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-runtime-sample"); }
	virtual FText GetDisplayName() const override { return NSLOCTEXT("UnrealAIRuntimeSample", "DisplayName", "Unreal AI Runtime Sample"); }
	virtual FString GetExtensionVersion() const override { return TEXT("1.0.0"); }

	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
	{
		Registry.Tool(TEXT("game-time-dilation"))
			.Title(TEXT("Get / Set Game Time Dilation"))
			.Description(TEXT("Reads the live game world's global time dilation (AWorldSettings::TimeDilation). "
			                  "Pass the optional 'value' to SET it first (slow-motion < 1, fast-forward > 1), then "
			                  "the current value is returned. A runtime gameplay-tool sample: it drives real "
			                  "gameplay state on the live UWorld, callable in a running game over a runtime MCP "
			                  "connection (docs/ARCHITECTURE.md §12.9)."))
			.ParamNumber(TEXT("value"), TEXT("Optional new global time dilation (> 0). Omit to read the current value without changing it."))
			.IdempotentHint(true) // setting to the same value twice yields the same state
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Runs ON the game thread (the dispatcher guarantees it, §4), so touching the live world is safe.
				UWorld* World = RuntimeSampleResolveGameWorld();
				if (!World)
				{
					return FUnrealMcpToolResult::Error(TEXT(
						"No live game world is available. Call this tool while a game is running (PIE, Standalone, "
						"or a packaged Development build) — the runtime MCP connection must be made from in-game."));
				}

				AWorldSettings* Settings = World->GetWorldSettings();
				if (!Settings)
					return FUnrealMcpToolResult::Error(TEXT("The live world has no AWorldSettings; cannot read or set time dilation."));

				bool bWasSet = false;
				if (Call.Has(TEXT("value")))
				{
					const double Requested = Call.GetNumber(TEXT("value"));
					if (Requested <= 0.0)
						return FUnrealMcpToolResult::Error(TEXT("'value' must be greater than 0 (time dilation cannot be zero or negative)."));

					// AWorldSettings clamps the assigned value to [MinGlobalTimeDilation, MaxGlobalTimeDilation],
					// so read the EFFECTIVE value back rather than echoing the request.
					Settings->SetTimeDilation(static_cast<float>(Requested));
					bWasSet = true;
				}

				const double Current = Settings->TimeDilation;

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("timeDilation"), Current);
				Structured->SetBoolField(TEXT("wasSet"), bWasSet);
				Structured->SetStringField(TEXT("world"), World->GetName());

				const FString Message = bWasSet
					? FString::Printf(TEXT("Set global time dilation to %.4f (effective, after clamping) on world '%s'."), Current, *World->GetName())
					: FString::Printf(TEXT("Global time dilation is %.4f on world '%s'."), Current, *World->GetName());
				return FUnrealMcpToolResult::Success(Message, Structured);
			});
	}
};

/**
 * Runtime (game) module that owns the provider and registers it as a modular feature. Mirrors
 * samples/UnrealAITemplate but Type=Runtime, so it loads in a packaged game (where the editor module is
 * absent). Registering on StartupModule lets Unreal-MCP discover the tool on boot OR live via the
 * OnModularFeatureRegistered event; unregistering on shutdown triggers a registry rebuild + manifest
 * re-push on the Unreal-MCP side (docs/ARCHITECTURE.md §12.9).
 *
 * Equivalent ergonomic alternative (docs/EXTENSIONS.md "Runtime usage"): once the runtime subsystem is up,
 * a game can call `UUnrealMcpRuntimeSubsystem::Get(this)->RegisterToolProvider(Provider)` instead of
 * touching IModularFeatures directly — both paths feed the same extension manager.
 */
class FUnrealAIRuntimeSampleModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeUnique<FUnrealAIRuntimeSampleProvider>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
		UE_LOG(LogUnrealAIRuntimeSample, Log, TEXT("[UnrealAIRuntimeSample] registered runtime MCP tool provider '%s'."), *Provider->GetExtensionId());
	}

	virtual void ShutdownModule() override
	{
		if (Provider.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
			Provider.Reset();
			UE_LOG(LogUnrealAIRuntimeSample, Log, TEXT("[UnrealAIRuntimeSample] unregistered runtime MCP tool provider."));
		}
	}

private:
	TUniquePtr<FUnrealAIRuntimeSampleProvider> Provider;
};

IMPLEMENT_MODULE(FUnrealAIRuntimeSampleModule, UnrealAIRuntimeSample)
