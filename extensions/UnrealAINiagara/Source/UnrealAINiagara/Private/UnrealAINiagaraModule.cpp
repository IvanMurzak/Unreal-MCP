// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAINiagaraRqlApi.h"

#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Niagara is queried and changed with rql-query /
 * rql-mutate instead of dedicated tools. Reflection already exposes every Niagara type (UNiagaraSystem,
 * UNiagaraComponent, ...); the module adds the `niagaraSystems` asset source, the `niagara` operations, the systems'
 * emitters (not a reflected property) and the agent guide rendered into the unreal-rql skill.
 *
 * The version is the .uplugin VersionName.
 */
class FUnrealAINiagaraRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("Niagara"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-niagara"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAINiagara")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddAssetSource(TEXT("niagaraSystems"), TEXT("Niagara system assets (NiagaraSystem) of the project, at most 200"),
			UNiagaraSystem::StaticClass());
		Context.AddApiSource(TEXT("niagara"), TEXT("Niagara operations: call SpawnSystem(system, location, label, autoActivate)"),
			UUnrealAINiagaraRqlApi::StaticClass(), TEXT("NiagaraOperations"));
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

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAINiagaraRqlModule>, UnrealAINiagara)
