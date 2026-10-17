// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAINiagaraRqlApi.h"

#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAINiagara, Log, All);

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Niagara is queried and changed with rql-query /
 * rql-mutate instead of dedicated tools. Reflection already exposes every Niagara type (UNiagaraSystem,
 * UNiagaraComponent, ...); the module adds the `niagaraSystems` asset source, the `niagara` operations, the systems'
 * emitters (not a reflected property) and the agent guide rendered into the unreal-rql skill.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName (scripts/extension_release.py bump updates both).
 */
class FUnrealAINiagaraRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("Niagara"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-niagara"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.1"); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddAssetSource(TEXT("niagaraSystems"), TEXT("Niagara system assets (NiagaraSystem) of the project, at most 200"),
			UNiagaraSystem::StaticClass());
		Context.AddApiClass(UUnrealAINiagaraRqlApi::StaticClass(), TEXT("NiagaraOperations"));
		Context.AddObjectSource(TEXT("niagara"), TEXT("Niagara operations: call SpawnSystem(system, location, label, autoActivate)"),
			UUnrealAINiagaraRqlApi::StaticClass(), []() -> UObject* { return GetMutableDefault<UUnrealAINiagaraRqlApi>(); });
		Context.AddJsonMember(UNiagaraSystem::StaticClass(), TEXT("Emitters"), TEXT("the system's emitters: Name, Enabled"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Emitters;
				if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Object))
				{
					for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Name"), Handle.GetName().ToString());
						Entry->SetBoolField(TEXT("Enabled"), Handle.GetIsEnabled());
						Emitters.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				return MakeShared<FJsonValueArray>(Emitters);
			});
		Context.AddGuide(TEXT("Niagara"), TEXT(R"MD(Niagara systems are assets: `niagaraSystems` (or `unreal.assets("NiagaraSystem", "/Game/VFX")`). Placed effects
are NiagaraComponents (a Niagara actor's component, or any actor's): `actors | get Components | where $ is NiagaraComponent`.

```
niagaraSystems | where Name contains "Fire" | select $ref Name Path Emitters
niagara | call SpawnSystem("/Game/VFX/NS_Fire", "(0, 0, 100)", "Campfire")        a Niagara actor in the current level; returns it
actors | get Components | where $ is NiagaraComponent | select Owner.Label Asset bAutoActivate
actors | where Label == "Campfire" | get Components | where $ is NiagaraComponent | set Asset = "/Game/VFX/NS_Smoke"
```

`Emitters` lists each emitter's Name and Enabled flag. Spawning is undoable; the level is marked unsaved.)MD"));
	}
};

/**
 * Editor module that owns the RQL module and registers it as a modular feature, so Unreal-MCP discovers it — on boot, or
 * live when this plugin loads after Unreal-MCP (the rql-* tools are republished either way).
 */
class FUnrealAINiagaraModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RqlModule = MakeUnique<FUnrealAINiagaraRqlModule>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
		UE_LOG(LogUnrealAINiagara, Log, TEXT("[UnrealAINiagara] registered RQL module '%s'."), *RqlModule->GetRqlModuleName());
	}

	virtual void ShutdownModule() override
	{
		if (RqlModule.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
			RqlModule.Reset();
		}
	}

private:
	TUniquePtr<FUnrealAINiagaraRqlModule> RqlModule;
};

IMPLEMENT_MODULE(FUnrealAINiagaraModule, UnrealAINiagara)
