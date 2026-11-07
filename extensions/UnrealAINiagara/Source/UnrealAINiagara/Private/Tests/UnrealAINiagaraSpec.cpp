// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealAINiagaraStack.h"
#include "UnrealAINiagaraTemplates.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "NiagaraActor.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions").
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::RunPlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it. Authoring results are read back
//  from the system asset and through the same stack view models the operations use.
//
//  Run via:  Automation RunTests UnrealAINiagara
// ============================================================================================

namespace UnrealAINiagaraSpecLocal
{
	FString Json(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		if (Object.IsValid())
			FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		return Out;
	}

	/** The text value of one module input, as the Inputs member prints it. */
	FString InputValue(UNiagaraSystem& System, const FString& Emitter, const FString& Module, const FString& Input)
	{
		UnrealAINiagara::FSystemStack Stack(System);
		UnrealAINiagara::FModuleRef Found;
		if (!Stack.FindModule(Emitter, Module, Found))
			return TEXT("<no module>");
		for (const UnrealAINiagara::FInputRef& Ref : UnrealAINiagara::Inputs(Found))
		{
			if (Ref.Path == Input)
				return UnrealAINiagara::FormatInput(*Ref.Input);
		}
		return TEXT("<no input>");
	}

	bool HasModule(UNiagaraSystem& System, const FString& Emitter, const FString& Module)
	{
		UnrealAINiagara::FSystemStack Stack(System);
		for (const UnrealAINiagara::FModuleRef& Ref : Stack.Modules(Emitter))
		{
			if (Ref.Name == Module)
				return true;
		}
		return false;
	}
}

BEGIN_DEFINE_SPEC(FUnrealAINiagaraSpec, "UnrealAINiagara",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAINiagaraSpec)

void FUnrealAINiagaraSpec::Define()
{
	using namespace UnrealAINiagaraSpecLocal;

	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-niagara"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("Niagara")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and authoring guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			for (const TCHAR* Source : { TEXT("| `niagaraSystems` |"), TEXT("| `niagaraEmitters` |"), TEXT("| `niagaraTemplates` |"), TEXT("| `niagara` |") })
				TestTrue(Source, Skill.Contains(Source));
			TestTrue(TEXT("guide section"), Skill.Contains(TEXT("## Niagara")));
			for (const TCHAR* Step : { TEXT("CreateSystem("), TEXT("SetModuleInput("), TEXT("EnableCollision("), TEXT("Compile("), TEXT("EnableFluids()") })
				TestTrue(*FString::Printf(TEXT("guide shows %s"), Step), Skill.Contains(Step));
		});
	});

	Describe("sources", [this]()
	{
		It("niagaraSystems answers a query (count of the project's Niagara systems)", [this]()
		{
			// niagaraSystems | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagaraSystems"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("query ok"), bResultOk);
			TestTrue(TEXT("a count"), Result.IsValid() && Result->HasField(TEXT("value")));
		});

		It("niagaraTemplates lists the engine's templates with their paths and descriptions", [this]()
		{
			const TArray<UObject*> Rows = UnrealAINiagaraTemplates::List();
			const UUnrealAINiagaraTemplate* Fountain = nullptr;
			for (const UObject* Row : Rows)
			{
				const UUnrealAINiagaraTemplate* Template = CastChecked<UUnrealAINiagaraTemplate>(Row);
				if (Template->Name == TEXT("Fountain") && Template->Kind == TEXT("Emitter"))
					Fountain = Template;
			}
			if (TestNotNull(TEXT("the Fountain emitter template"), Fountain))
			{
				TestEqual(TEXT("path"), Fountain->Path, FString(TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain")));
				TestEqual(TEXT("plugin"), Fountain->Plugin, FString(TEXT("Niagara")));
				TestFalse(TEXT("described"), Fountain->Description.IsEmpty());
				TestTrue(TEXT("available"), Fountain->Available);
			}
			// niagaraTemplates | where Name == "Fountain" | count
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("query ok"), UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagaraTemplates"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Fountain"}]}},{"op":"count"}]})RQL"), false, &Result));
			TestTrue(TEXT("found by name"), Result.IsValid() && Result->GetNumberField(TEXT("value")) >= 1);
			if (UnrealAINiagaraTemplates::IsFluidsInstalled() && !UnrealAINiagaraTemplates::IsFluidsEnabled())
			{
				const bool bPluginRow = Rows.ContainsByPredicate([](const UObject* Row)
				{
					const UUnrealAINiagaraTemplate* Template = CastChecked<UUnrealAINiagaraTemplate>(Row);
					return Template->Kind == TEXT("Plugin") && !Template->Available && Template->Description.Contains(TEXT("EnableFluids()"));
				});
				TestTrue(TEXT("a disabled Niagara Fluids plugin row says how to enable it"), bPluginRow);
			}
		});
	});

	Describe("schema", [this]()
	{
		It("describes NiagaraSystem with its authoring members", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("NiagaraSystem"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Member : { TEXT("Emitters"), TEXT("Modules"), TEXT("Inputs"), TEXT("UserParameters"), TEXT("Renderers") })
				TestTrue(*FString::Printf(TEXT("%s member"), Member), Schema.Contains(FString::Printf(TEXT("\"name\":\"%s\""), Member)));
		});

		It("lists every operation of the niagara source", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("NiagaraOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Method : { TEXT("CreateSystem"), TEXT("AddEmitter"), TEXT("RemoveEmitter"), TEXT("RenameEmitter"), TEXT("SetEmitterEnabled"),
				TEXT("SetEmitterProperty"), TEXT("AddModule"), TEXT("RemoveModule"), TEXT("SetModuleEnabled"), TEXT("SetModuleInput"), TEXT("SetUserParameter"),
				TEXT("EnableCollision"), TEXT("SetMaterial"), TEXT("Compile"), TEXT("SpawnSystem"), TEXT("Activate"), TEXT("Deactivate"), TEXT("EnableFluids") })
				TestTrue(*FString::Printf(TEXT("lists %s"), Method), Schema.Contains(FString(Method) + TEXT("(")));
		});
	});

	Describe("authoring", [this]()
	{
		It("builds a fire + smoke system from templates: inputs, random, curve, collision, material, user parameter, compile, place", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			const FString Path = TEXT("/Game/RqlSpecNiagara/NS_Campfire_") + Id;
			const FString Label = TEXT("RqlSpecCampfire_") + Id;
			auto Run = [&Path, &Label](const TCHAR* Plan, TSharedPtr<FJsonObject>* Out = nullptr)
			{
				return UnrealMcpRqlTools::RunPlan(FString(Plan).Replace(TEXT("@SYS@"), *Path).Replace(TEXT("@LABEL@"), *Label), true, Out);
			};

			// niagara | call CreateSystem("@SYS@", "/Niagara/DefaultAssets/Templates/Emitters/Fountain")
			TestTrue(TEXT("created"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"CreateSystem","args":[{"lit":"@SYS@"},{"lit":"/Niagara/DefaultAssets/Templates/Emitters/Fountain"}]}]})RQL")));
			UNiagaraSystem* System = FindObject<UNiagaraSystem>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
			if (!TestNotNull(TEXT("the system asset"), System))
				return;
			TestEqual(TEXT("one emitter from the template"), System->GetEmitterHandles().Num(), 1);

			// niagara | call AddEmitter("@SYS@", "/Niagara/DefaultAssets/Templates/Emitters/HangingParticulates", "Smoke")
			TestTrue(TEXT("smoke added"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"AddEmitter","args":[{"lit":"@SYS@"},{"lit":"/Niagara/DefaultAssets/Templates/Emitters/HangingParticulates"},{"lit":"Smoke"}]}]})RQL")));
			// niagara | call RenameEmitter("@SYS@", "Fountain", "Fire")
			TestTrue(TEXT("renamed"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"RenameEmitter","args":[{"lit":"@SYS@"},{"lit":"Fountain"},{"lit":"Fire"}]}]})RQL")));
			TestEqual(TEXT("emitter names"), UnrealAINiagara::EmitterNames(*System), FString(TEXT("Fire, Smoke")));

			// niagara | call SetModuleInput("@SYS@", "Fire", "Spawn Rate", "SpawnRate", "120")
			TestTrue(TEXT("spawn rate set"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetModuleInput","args":[{"lit":"@SYS@"},{"lit":"Fire"},{"lit":"Spawn Rate"},{"lit":"SpawnRate"},{"lit":"120"}]}]})RQL")));
			TestEqual(TEXT("spawn rate"), InputValue(*System, TEXT("Fire"), TEXT("Spawn Rate"), TEXT("SpawnRate")), FString(TEXT("120")));
			// niagara | call SetModuleInput("@SYS@", "Fire", "Add Velocity", "Velocity Speed", "random(150, 300)")
			TestTrue(TEXT("random speed set"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetModuleInput","args":[{"lit":"@SYS@"},{"lit":"Fire"},{"lit":"Add Velocity"},{"lit":"Velocity Speed"},{"lit":"random(150, 300)"}]}]})RQL")));
			TestEqual(TEXT("random minimum"), InputValue(*System, TEXT("Fire"), TEXT("Add Velocity"), TEXT("Velocity Speed.Minimum")), FString(TEXT("150")));
			TestEqual(TEXT("random maximum"), InputValue(*System, TEXT("Fire"), TEXT("Add Velocity"), TEXT("Velocity Speed.Maximum")), FString(TEXT("300")));
			// niagara | call SetModuleInput("@SYS@", "Fire", "Add Velocity", "Velocity", "(0, 0, 100)")      hidden while Velocity Mode is In Cone
			TSharedPtr<FJsonObject> Hidden;
			TestFalse(TEXT("a hidden input is refused"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetModuleInput","args":[{"lit":"@SYS@"},{"lit":"Fire"},{"lit":"Add Velocity"},{"lit":"Velocity"},{"lit":"(0, 0, 100)"}]}]})RQL"), &Hidden));
			TestTrue(TEXT("saying it is hidden by an option"), Json(Hidden).Contains(TEXT("hidden by the module's current options")));
			// niagara | call SetModuleInput("@SYS@", "Fire", "Scale Color", "Scale Alpha", "curve(0: 1, 0.7: 0.8, 1: 0)")
			TestTrue(TEXT("alpha curve set"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetModuleInput","args":[{"lit":"@SYS@"},{"lit":"Fire"},{"lit":"Scale Color"},{"lit":"Scale Alpha"},{"lit":"curve(0: 1, 0.7: 0.8, 1: 0)"}]}]})RQL")));
			TestEqual(TEXT("alpha curve"), InputValue(*System, TEXT("Fire"), TEXT("Scale Color"), TEXT("Scale Alpha.FloatCurve")), FString(TEXT("curve(0: 1, 0.7: 0.8, 1: 0)")));

			// niagara | call EnableCollision("@SYS@", "Fire", "RayTrace")
			TestTrue(TEXT("collision enabled"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"EnableCollision","args":[{"lit":"@SYS@"},{"lit":"Fire"},{"lit":"RayTrace"}]}]})RQL")));
			TestTrue(TEXT("a Collision module"), HasModule(*System, TEXT("Fire"), TEXT("Collision")));
			TestEqual(TEXT("CPU ray traces"), InputValue(*System, TEXT("Fire"), TEXT("Collision"), TEXT("CPU Collision Type")), FString(TEXT("Ray Traced")));

			// niagara | call SetMaterial("@SYS@", "Smoke", "/Engine/EngineMaterials/DefaultMaterial")
			TestTrue(TEXT("material set"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetMaterial","args":[{"lit":"@SYS@"},{"lit":"Smoke"},{"lit":"/Engine/EngineMaterials/DefaultMaterial"}]}]})RQL")));
			const FNiagaraEmitterHandle* Smoke = System->GetEmitterHandles().FindByPredicate([](const FNiagaraEmitterHandle& Handle) { return Handle.GetName() == TEXT("Smoke"); });
			const UNiagaraRendererProperties* Renderer = Smoke && Smoke->GetEmitterData() && Smoke->GetEmitterData()->GetRenderers().Num() ? Smoke->GetEmitterData()->GetRenderers()[0] : nullptr;
			const FObjectPropertyBase* MaterialProperty = Renderer ? CastField<FObjectPropertyBase>(Renderer->GetClass()->FindPropertyByName(TEXT("Material"))) : nullptr;
			TestTrue(TEXT("the smoke sprite renderer uses it"), MaterialProperty && MaterialProperty->GetObjectPropertyValue_InContainer(Renderer)
				&& MaterialProperty->GetObjectPropertyValue_InContainer(Renderer)->GetName() == TEXT("DefaultMaterial"));

			// niagara | call SetUserParameter("@SYS@", "Intensity", "1.5")
			TestTrue(TEXT("user parameter added"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetUserParameter","args":[{"lit":"@SYS@"},{"lit":"Intensity"},{"lit":"1.5"}]}]})RQL")));
			const FNiagaraVariable Intensity(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.Intensity"));
			const uint8* IntensityData = System->GetExposedParameters().GetParameterData(Intensity);
			TestTrue(TEXT("User.Intensity = 1.5"), IntensityData && FMath::IsNearlyEqual(*reinterpret_cast<const float*>(IntensityData), 1.5f));

			// niagara | call Compile("@SYS@")
			TSharedPtr<FJsonObject> Compiled;
			TestTrue(TEXT("compile ran"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"Compile","args":[{"lit":"@SYS@"}]}]})RQL"), &Compiled));
			TestTrue(TEXT("compiled without errors"), Json(Compiled).Contains(TEXT("Succeeded\":true")));

			// niagara | call SpawnSystem("@SYS@", "(0, 0, 0)", "@LABEL@")      niagara | call Activate(unreal.find("@LABEL@"))
			TestTrue(TEXT("placed"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SpawnSystem","args":[{"lit":"@SYS@"},{"lit":"(0, 0, 0)"},{"lit":"@LABEL@"}]}]})RQL")));
			TestTrue(TEXT("activated"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"Activate","args":[{"call":"find","on":{"path":["unreal"]},"args":[{"lit":"@LABEL@"}]}]}]})RQL")));
			if (UWorld* World = UnrealMcpRql::EditorWorld())
			{
				for (TActorIterator<ANiagaraActor> It(World); It; ++It)
				{
					if (It->GetActorLabel() == Label)
						World->EditorDestroyActor(*It, true);
				}
			}

			// niagara | call RemoveModule("@SYS@", "Fire", "Collision")      niagara | call RemoveEmitter("@SYS@", "Smoke")
			TestTrue(TEXT("module removed"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"RemoveModule","args":[{"lit":"@SYS@"},{"lit":"Fire"},{"lit":"Collision"}]}]})RQL")));
			TestFalse(TEXT("no Collision module"), HasModule(*System, TEXT("Fire"), TEXT("Collision")));
			TestTrue(TEXT("emitter removed"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"RemoveEmitter","args":[{"lit":"@SYS@"},{"lit":"Smoke"}]}]})RQL")));
			TestEqual(TEXT("one emitter left"), UnrealAINiagara::EmitterNames(*System), FString(TEXT("Fire")));

			// editor | call DeleteAsset("@SYS@", true)      the test leaves no unsaved asset behind
			TestTrue(TEXT("test asset deleted"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"DeleteAsset","args":[{"lit":"@SYS@"},{"lit":true}]}]})RQL")));
		});

		It("fails with actionable errors that name what exists", [this]()
		{
			const FString Path = TEXT("/Game/RqlSpecNiagara/NS_Errors_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			auto Run = [&Path](const TCHAR* Plan, TSharedPtr<FJsonObject>* Out)
			{
				return UnrealMcpRqlTools::RunPlan(FString(Plan).Replace(TEXT("@SYS@"), *Path), true, Out);
			};
			// niagara | call CreateSystem("@SYS@", "/Niagara/DefaultAssets/Templates/Emitters/Fountain")
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("created"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"CreateSystem","args":[{"lit":"@SYS@"},{"lit":"/Niagara/DefaultAssets/Templates/Emitters/Fountain"}]}]})RQL"), &Result));

			// niagara | call SetModuleInput("@SYS@", "Fyre", "Spawn Rate", "SpawnRate", "1")
			TestFalse(TEXT("unknown emitter fails"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetModuleInput","args":[{"lit":"@SYS@"},{"lit":"Fyre"},{"lit":"Spawn Rate"},{"lit":"SpawnRate"},{"lit":"1"}]}]})RQL"), &Result));
			TestTrue(TEXT("names the emitters"), Json(Result).Contains(TEXT("emitters: Fountain")));
			// niagara | call SetModuleInput("@SYS@", "Fountain", "Spawn Burst", "Count", "1")
			TestFalse(TEXT("unknown module fails"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetModuleInput","args":[{"lit":"@SYS@"},{"lit":"Fountain"},{"lit":"Spawn Burst"},{"lit":"Count"},{"lit":"1"}]}]})RQL"), &Result));
			TestTrue(TEXT("names the modules"), Json(Result).Contains(TEXT("Emitter Update/Spawn Rate")));
			// niagara | call SetModuleInput("@SYS@", "Fountain", "Spawn Rate", "Rate", "1")
			TestFalse(TEXT("unknown input fails"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetModuleInput","args":[{"lit":"@SYS@"},{"lit":"Fountain"},{"lit":"Spawn Rate"},{"lit":"Rate"},{"lit":"1"}]}]})RQL"), &Result));
			TestTrue(TEXT("names the inputs"), Json(Result).Contains(TEXT("SpawnRate")));
			// niagara | call SetModuleInput("@SYS@", "Fountain", "Spawn Rate", "SpawnRate", "lots")
			TestFalse(TEXT("a bad value fails"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SetModuleInput","args":[{"lit":"@SYS@"},{"lit":"Fountain"},{"lit":"Spawn Rate"},{"lit":"SpawnRate"},{"lit":"lots"}]}]})RQL"), &Result));
			TestTrue(TEXT("says what it expects"), Json(Result).Contains(TEXT("is not a float")));
			// niagara | call EnableCollision("@SYS@", "", "Sideways")
			TestFalse(TEXT("an unknown collision mode fails"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"EnableCollision","args":[{"lit":"@SYS@"},{"lit":""},{"lit":"Sideways"}]}]})RQL"), &Result));
			TestTrue(TEXT("lists the modes"), Json(Result).Contains(TEXT("SceneDepth")));
			// niagara | call AddModule("@SYS@", "Fountain", "Particle Update", "NoSuchModuleScript")
			TestFalse(TEXT("an unknown module script fails"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"AddModule","args":[{"lit":"@SYS@"},{"lit":"Fountain"},{"lit":"Particle Update"},{"lit":"NoSuchModuleScript"}]}]})RQL"), &Result));
			TestTrue(TEXT("says how to find one"), Json(Result).Contains(TEXT("NiagaraScript")));
			if (!UnrealAINiagaraTemplates::IsFluidsEnabled())
			{
				// niagara | call CreateSystem("@SYS@_Fluid", "/NiagaraFluids/Templates/Gas/3D/Systems/Grid3D_Gas_Fire")
				TestFalse(TEXT("a Niagara Fluids template fails while the plugin is off"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"CreateSystem","args":[{"lit":"@SYS@_Fluid"},{"lit":"/NiagaraFluids/Templates/Gas/3D/Systems/Grid3D_Gas_Fire"}]}]})RQL"), &Result));
				TestTrue(TEXT("says how to enable it"), Json(Result).Contains(TEXT("EnableFluids()")));
			}
			// editor | call DeleteAsset("@SYS@", true)
			TestTrue(TEXT("test asset deleted"), Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"DeleteAsset","args":[{"lit":"@SYS@"},{"lit":true}]}]})RQL"), &Result));
		});

		It("fails defensively on a missing system and places nothing", [this]()
		{
			// niagara | call SpawnSystem("/Game/RqlSpecNoSuchSystem", "(0, 0, 0)", "RqlSpecFx")
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["niagara"]},"stages":[{"op":"call","method":"SpawnSystem","args":[{"lit":"/Game/RqlSpecNoSuchSystem"},{"lit":"(0, 0, 0)"},{"lit":"RqlSpecFx"}]}]})RQL"), true, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("reported as a failure"), bResultOk);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
