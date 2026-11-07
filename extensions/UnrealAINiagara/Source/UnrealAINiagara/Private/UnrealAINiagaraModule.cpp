// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAINiagaraRqlApi.h"
#include "UnrealAINiagaraStack.h"
#include "UnrealAINiagaraTemplates.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace UnrealAINiagaraModuleLocal
{
	/** The system and emitter handle a renderer belongs to (its outer chain: emitter, system). */
	FString RendererEmitter(const UNiagaraRendererProperties& Renderer)
	{
		const UNiagaraEmitter* Emitter = Renderer.GetTypedOuter<UNiagaraEmitter>();
		const UNiagaraSystem* System = Emitter ? Emitter->GetTypedOuter<UNiagaraSystem>() : nullptr;
		if (System)
		{
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (Handle.GetInstance().Emitter == Emitter)
					return Handle.GetName().ToString();
			}
		}
		return Emitter ? Emitter->GetName() : FString();
	}

	TArray<UObject*> Renderers(UObject* Object)
	{
		TArray<UObject*> Out;
		if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Object))
		{
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
				{
					for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
					{
						if (Renderer)
							Out.Add(Renderer);
					}
				}
			}
		}
		return Out;
	}

	TSharedPtr<FJsonValue> Emitters(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Object))
		{
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("Name"), Handle.GetName().ToString());
				Row->SetBoolField(TEXT("Enabled"), Handle.GetIsEnabled());
				if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
				{
					Row->SetStringField(TEXT("Sim"), Data->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU"));
					TArray<TSharedPtr<FJsonValue>> Renderers;
					for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
					{
						if (Renderer)
							Renderers.Add(MakeShared<FJsonValueString>(Renderer->GetClass()->GetName().Replace(TEXT("Niagara"), TEXT("")).Replace(TEXT("RendererProperties"), TEXT(""))));
					}
					Row->SetArrayField(TEXT("Renderers"), Renderers);
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		return MakeShared<FJsonValueArray>(Rows);
	}

	TSharedPtr<FJsonValue> StackJson(UObject* Object, bool bInputs)
	{
		UNiagaraSystem* System = Cast<UNiagaraSystem>(Object);
		if (!System)
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
		UnrealAINiagara::FSystemStack Stack(*System);
		return Stack.ToJson(bInputs);
	}

	TSharedPtr<FJsonValue> UserParameters(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Object))
		{
			const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables())
			{
				const FNiagaraTypeDefinition& Type = Variable.GetType();
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("Name"), Variable.GetName().ToString());
				Row->SetStringField(TEXT("Type"), UnrealAINiagara::TypeName(Type));
				FString Value;
				if (Type.IsDataInterface())
				{
					const UNiagaraDataInterface* Data = Store.GetDataInterface(Variable);
					Value = Data ? Data->GetClass()->GetName() : FString(TEXT("None"));
				}
				else if (Type.IsUObject())
				{
					const UObject* Asset = Store.GetUObject(Variable);
					Value = Asset ? Asset->GetPathName() : FString(TEXT("None"));
				}
				else if (const uint8* Data = Store.GetParameterData(Variable))
				{
					Value = UnrealAINiagara::FormatValue(Type, Data);
				}
				Row->SetStringField(TEXT("Value"), Value);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		return MakeShared<FJsonValueArray>(Rows);
	}
}

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): Niagara is queried and authored with rql-query /
 * rql-mutate instead of dedicated tools. Reflection already exposes every Niagara type (UNiagaraSystem, renderer
 * properties, UNiagaraComponent, ...); the module adds the asset and template sources, the `niagara` operations (built
 * on the Niagara editor's stack view models, see UnrealAINiagaraStack.h), the members reflection does not show (emitters,
 * modules, module inputs, user parameters, renderers) and the agent guide rendered into the unreal-rql skill.
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
		using namespace UnrealAINiagaraModuleLocal;
		Context.AddAssetSource(TEXT("niagaraSystems"), TEXT("Niagara system assets (NiagaraSystem) of the project, at most 200"),
			UNiagaraSystem::StaticClass());
		Context.AddAssetSource(TEXT("niagaraEmitters"), TEXT("Niagara emitter assets (NiagaraEmitter) of the project, at most 200"),
			UNiagaraEmitter::StaticClass());
		Context.AddObjectListSource(TEXT("niagaraTemplates"),
			TEXT("the Niagara system and emitter templates of the engine and enabled plugins (Name, Path, Kind, Category, Plugin, Description, Available); start an effect from one with CreateSystem"),
			UUnrealAINiagaraTemplate::StaticClass(), []() { return UnrealAINiagaraTemplates::List(); });
		Context.AddApiSource(TEXT("niagara"),
			TEXT("Niagara authoring: call CreateSystem / AddEmitter / RemoveEmitter / RenameEmitter / SetEmitterEnabled / SetEmitterProperty / AddModule / RemoveModule / SetModuleEnabled / SetModuleInput / SetUserParameter / EnableCollision / SetMaterial / Compile / SpawnSystem / Activate / Deactivate / EnableFluids"),
			UUnrealAINiagaraRqlApi::StaticClass(), TEXT("NiagaraOperations"));

		Context.AddJsonMember(UNiagaraSystem::StaticClass(), TEXT("Emitters"), TEXT("the system's emitters: Name, Enabled, Sim (CPU / GPU), Renderers"),
			[](UObject* Object) { return Emitters(Object); });
		Context.AddJsonMember(UNiagaraSystem::StaticClass(), TEXT("Modules"),
			TEXT("the modules of the system stack and of each emitter, in stack order: Emitter (System for the system stack), Stage, Module, Enabled"),
			[](UObject* Object) { return StackJson(Object, false); });
		Context.AddJsonMember(UNiagaraSystem::StaticClass(), TEXT("Inputs"),
			TEXT("every module input SetModuleInput can set: Emitter, Module (Stage/Module), Input (a dynamic input's own inputs as Input.Child), Type, Value (Active false: off until set)"),
			[](UObject* Object) { return StackJson(Object, true); });
		Context.AddJsonMember(UNiagaraSystem::StaticClass(), TEXT("UserParameters"),
			TEXT("the system's user (exposed) parameters: Name (User.X), Type, Value"),
			[](UObject* Object) { return UserParameters(Object); });
		Context.AddObjectListMember(UNiagaraSystem::StaticClass(), TEXT("Renderers"),
			TEXT("the renderers of every emitter (sprite, mesh, ribbon, light, volume, ...): their properties are writable members"),
			UNiagaraRendererProperties::StaticClass(), [](UObject* Object) { return Renderers(Object); });
		Context.AddJsonMember(UNiagaraRendererProperties::StaticClass(), TEXT("Emitter"), TEXT("the name of the emitter this renderer belongs to"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UNiagaraRendererProperties* Renderer = Cast<UNiagaraRendererProperties>(Object);
				return MakeShared<FJsonValueString>(Renderer ? RendererEmitter(*Renderer) : FString());
			});

		Context.AddGuide(TEXT("Niagara"), TEXT(R"MD(Author Niagara effects with the `niagara` operations, never through Niagara's Python API. A NiagaraSystem asset holds
emitters; each emitter is a stack of modules in stages (Emitter Spawn / Update, Particle Spawn / Update, simulation
stages) whose inputs you set, plus renderers. Start from a template, tune, collide, compile, place, preview, save:

```
niagaraTemplates | where Kind != "Emitter" | select Name Path Kind Description     whole-effect templates; a Kind "Plugin" row is a disabled plugin
niagaraTemplates | where Kind == "Emitter" and Plugin == "Niagara" | select Name Path Description
niagara | call CreateSystem("/Game/VFX/NS_Campfire", "/Niagara/DefaultAssets/Templates/Emitters/Fountain")      emitter template: a system with that emitter
niagara | call AddEmitter("/Game/VFX/NS_Campfire", "/Niagara/DefaultAssets/Templates/Emitters/HangingParticulates", "Smoke")
niagara | call RenameEmitter("/Game/VFX/NS_Campfire", "Fountain", "Fire")
unreal.asset("/Game/VFX/NS_Campfire") | select Emitters Modules UserParameters
unreal.asset("/Game/VFX/NS_Campfire").Inputs | where Emitter == "Fire" and Module contains "Initialize" | select Module Input Type Value
niagara | call SetModuleInput("/Game/VFX/NS_Campfire", "Fire", "Spawn Rate", "SpawnRate", "120")
niagara | call SetModuleInput("/Game/VFX/NS_Campfire", "Fire", "Initialize Particle", "Lifetime Max", "1.2")
niagara | call SetModuleInput("/Game/VFX/NS_Campfire", "Fire", "Initialize Particle", "Color", "#ff7a1a")
niagara | call SetModuleInput("/Game/VFX/NS_Campfire", "Fire", "Add Velocity", "Velocity Speed", "random(150, 300)")
niagara | call SetModuleInput("/Game/VFX/NS_Campfire", "Fire", "Scale Color", "Scale Alpha", "curve(0: 1, 0.7: 0.8, 1: 0)")
niagara | call AddModule("/Game/VFX/NS_Campfire", "Smoke", "Particle Update", "CurlNoiseForce")         a module script by name or NiagaraScript path
niagara | call EnableCollision("/Game/VFX/NS_Campfire")                      every emitter collides with the scene (see Collision)
niagara | call SetMaterial("/Game/VFX/NS_Campfire", "Smoke", "/Game/VFX/M_Smoke")
niagara | call SetUserParameter("/Game/VFX/NS_Campfire", "Intensity", "1.5")   adds User.Intensity (float) or sets its default
niagara | call Compile("/Game/VFX/NS_Campfire")                              {Succeeded, Errors, Warnings}: fix errors before placing
niagara | call SpawnSystem("/Game/VFX/NS_Campfire", "(0, 0, 0)", "Campfire")
niagara | call Activate(unreal.find("Campfire"))                            restart the placed effect after edits
editor | call SaveAsset("/Game/VFX/NS_Campfire")
```

- **Values** are text: numbers; `"(x, y, z)"` (one number fills every component); colours `"#rrggbb"` (sRGB) or
  `"(r, g, b, a)"` (linear); enum entries by name (`"Random"`, `"Ray Traced"`); `true` / `false`; asset paths;
  `"random(min, max)"` (uniform random range, numbers or vectors); `"curve(t: v, ...)"` over the particle's normalized
  age (`"curve(0: #ffd27f, 0.4: #ff4a00, 1: #202020)"` colour ramps, size and alpha fades); `"@User.Intensity"` (link to
  a parameter); `"default"` (reset). A data interface input or user parameter takes `"Property=Value, ..."` or an
  asset path (`SetUserParameter(system, "StaticMesh", "/Engine/BasicShapes/Cylinder")` emits from that mesh).
- **Names**: `Module` and `Input` as the Modules / Inputs members print them (case, spaces, underscores ignored);
  `"Particle Update/Collision"` picks a module two stages share; `"Lifetime.Minimum"` is an input of the dynamic input
  set on Lifetime. Only the inputs the module's options select are listed: set `Lifetime Mode` (or another `... Mode`)
  first; an inline toggle is switched on when its input is set. `Emitter` `""` is the system stack. A wrong name fails
  listing what exists.
- **Collision** `EnableCollision(system, emitter = "" (all), mode = "Auto")`. Particles get the Collision module:
  `RayTrace` (CPU traces against collision geometry, off-screen too), `SceneDepth` (GPU, what is on screen; switches
  the emitter to GPU with fixed bounds, resize with `SetEmitterProperty(..., "FixedBounds",
  "(Min=(X=-500,Y=-500,Z=0),Max=(X=500,Y=500,Z=800))")`), `DistanceField` (GPU, mesh distance fields), `None`; `Auto` =
  RayTrace on CPU emitters, SceneDepth on GPU ones. Niagara Fluids grids get world collision instead: the physics
  shapes of every mesh in the grid, static props included (`Auto` / `RayTrace`), mesh distance fields (`Auto` when the
  project has them, `DistanceField`), `SceneDepth`, `GlobalDistanceField`. Distance fields need
  `unreal.settings("Rendering") | set bGenerateMeshDistanceFields = true` and an editor restart (else the grid does not
  compile). The result says what changed and what the project still needs.
- **Volumetric fire and smoke that flows around props**: the Niagara Fluids gas templates (`Grid3D_Gas_Fire`: fire from
  a mesh, `Grid3D_Gas_Smoke`, `Grid3D_Gas_SimpleParticleSource`, ...; GPU). Typical: CreateSystem from
  `/NiagaraFluids/Templates/Gas/3D/Systems/Grid3D_Gas_Fire`, `SetUserParameter(..., "StaticMesh", <burning mesh>)`,
  `SetUserParameter(..., "WorldSpaceSize", "(300, 300, 500)")` (the simulated box), smoke with
  `SetModuleInput(..., "Grid3D_Gas_Master_Emitter", "Grid 3D Gas Initialize Material Controls", "Render Density", "Linear")`
  and `"Grid 3D Gas Material Controls"` / `"Smoke Color"`, then `EnableCollision`. When `niagaraTemplates` has the
  `NiagaraFluids` row (Available false): `niagara | call EnableFluids()`, save, restart the editor.
- Renderers are objects: `unreal.asset("/Game/VFX/NS_Campfire").Renderers | where Emitter == "Fire" | set SubImageSize = "(4, 4)"`.
- Emitter Properties: `SetEmitterProperty(system, emitter, "SimTarget", "GPU")` (also `bLocalSpace`, `CalculateBoundsMode`).
- Placed effects are NiagaraComponents: `actors | get Components | where $ is NiagaraComponent | set Asset = "/Game/VFX/NS_Smoke"`.
  Each mutation is one undo step; created assets stay unsaved until `editor | call SaveAsset(path)`.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAINiagaraRqlModule>, UnrealAINiagara)
