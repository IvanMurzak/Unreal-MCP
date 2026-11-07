// Copyright (c) 2026 IvanMurzak/Unreal-AI-GAS. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealAIAbilitySystemComponent.h"
#include "UnrealAIAttributeSet.h"
#include "UnrealAIAttributeTextComponent.h"
#include "UnrealAIGameplayAbility.h"
#include "UnrealAIGASAttributes.h"
#include "UnrealAIGASProjectile.h"

#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/WorldSettings.h"
#include "GameplayEffect.h"
#include "GameplayTagsManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions") and its runtime classes.
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::RunPlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it. Authored assets are read back from
//  their class defaults; the fireball scenario then runs in a transient game world with those assets.
//
//  Run via:  Automation RunTests UnrealAIGAS
// ============================================================================================

namespace UnrealAIGASSpecLocal
{
	FString Json(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		if (Object.IsValid())
			FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		return Out;
	}

	/** Unique asset paths for one test; @@ in a plan is replaced with the folder. */
	FString Folder()
	{
		return TEXT("/Game/RqlSpecGAS_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	}

	bool Run(const FString& Folder, const TCHAR* Plan, bool bMutate = true, TSharedPtr<FJsonObject>* Out = nullptr)
	{
		return UnrealMcpRqlTools::RunPlan(FString(Plan).Replace(TEXT("@@"), *Folder), bMutate, Out);
	}

	UClass* GeneratedClass(const FString& Path)
	{
		const UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		return Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
	}

	/** editor | call DeleteAsset(path, true) for every path. */
	void DeleteAssets(const FString& Folder, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* Name : Names)
		{
			const FString Plan = FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"DeleteAsset","args":[{"lit":"%s/%s"},{"lit":true}]}]})RQL"), *Folder, Name);
			UnrealMcpRqlTools::RunPlan(Plan, true, nullptr);
		}
	}

	/** The fireball's assets, made as an agent makes them (the guide's sequence). */
	bool AuthorFireball(FAutomationTestBase& Test, const FString& Folder)
	{
		bool bOk = true;
		// gas | call CreateEffect("@@/GE_Damage")
		bOk &= Test.TestTrue(TEXT("damage effect"), Run(Folder, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"CreateEffect","args":[{"lit":"@@/GE_Damage"}]}]})RQL")));
		// gas | call AddModifier("@@/GE_Damage", "Damage", "Add", "SetByCaller")
		bOk &= Test.TestTrue(TEXT("damage modifier"), Run(Folder, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"AddModifier","args":[{"lit":"@@/GE_Damage"},{"lit":"Damage"},{"lit":"Add"},{"lit":"SetByCaller"}]}]})RQL")));
		// gas | call CreateEffect("@@/GE_ManaRegen", "Infinite", 1)
		bOk &= Test.TestTrue(TEXT("regen effect"), Run(Folder, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"CreateEffect","args":[{"lit":"@@/GE_ManaRegen"},{"lit":"Infinite"},{"lit":1}]}]})RQL")));
		// gas | call AddModifier("@@/GE_ManaRegen", "Mana", "Add", "5")
		bOk &= Test.TestTrue(TEXT("regen modifier"), Run(Folder, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"AddModifier","args":[{"lit":"@@/GE_ManaRegen"},{"lit":"Mana"},{"lit":"Add"},{"lit":"5"}]}]})RQL")));
		// gas | call CreateAbility("@@/GA_Fireball")
		bOk &= Test.TestTrue(TEXT("ability"), Run(Folder, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"CreateAbility","args":[{"lit":"@@/GA_Fireball"}]}]})RQL")));
		// gas | call SetCost("@@/GA_Fireball", "Mana", 20)
		bOk &= Test.TestTrue(TEXT("cost"), Run(Folder, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetCost","args":[{"lit":"@@/GA_Fireball"},{"lit":"Mana"},{"lit":20}]}]})RQL")));
		// gas | call SetCooldown("@@/GA_Fireball", 3, "RqlSpec.GAS.Cooldown")
		bOk &= Test.TestTrue(TEXT("cooldown"), Run(Folder, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetCooldown","args":[{"lit":"@@/GA_Fireball"},{"lit":3},{"lit":"RqlSpec.GAS.Cooldown"}]}]})RQL")));
		// gas | call SetAbilityProjectile("@@/GA_Fireball", "@@/GE_Damage", 25)
		bOk &= Test.TestTrue(TEXT("projectile"), Run(Folder, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetAbilityProjectile","args":[{"lit":"@@/GA_Fireball"},{"lit":"@@/GE_Damage"},{"lit":25}]}]})RQL")));
		return bOk;
	}

	/** A game world that has begun play (no game mode): spawned actors and registered components initialize and begin play. */
	struct FGameWorld
	{
		UWorld* World = nullptr;

		FGameWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("UnrealAIGASSpecWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			if (AWorldSettings* Settings = World->GetWorldSettings())
				Settings->NotifyBeginPlay();
		}

		~FGameWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		/** An actor with an UnrealAIAbilitySystemComponent configured by Configure before it registers. */
		UUnrealAIAbilitySystemComponent* SpawnWithAbilitySystem(TFunctionRef<void(UUnrealAIAbilitySystemComponent&)> Configure) const
		{
			AActor* Actor = World->SpawnActor<AActor>();
			USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
			Actor->SetRootComponent(Root);
			Root->RegisterComponent();
			UUnrealAIAbilitySystemComponent* Asc = NewObject<UUnrealAIAbilitySystemComponent>(Actor, TEXT("AbilitySystem"));
			Configure(*Asc);
			Actor->AddInstanceComponent(Asc);
			Asc->RegisterComponent();
			if (!Asc->HasBeenInitialized())
				Asc->InitializeComponent();
			if (!Asc->HasBegunPlay())
				Asc->BeginPlay();
			return Asc;
		}
	};

	float Value(const UAbilitySystemComponent& Asc, const TCHAR* Name)
	{
		const FGameplayAttribute Attribute = UnrealAIGAS::FindAttribute(Asc, Name);
		return Attribute.IsValid() ? Asc.GetNumericAttribute(Attribute) : -1.f;
	}
}

BEGIN_DEFINE_SPEC(FUnrealAIGASSpec, "UnrealAIGAS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIGASSpec)

void FUnrealAIGASSpec::Define()
{
	using namespace UnrealAIGASSpecLocal;

	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-gas"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("GAS")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `gasAbilities` |"), Skill.Contains(TEXT("| `gasAbilities` |")));
			TestTrue(TEXT("| `gasEffects` |"), Skill.Contains(TEXT("| `gasEffects` |")));
			TestTrue(TEXT("| `gasAttributeSets` |"), Skill.Contains(TEXT("| `gasAttributeSets` |")));
			TestTrue(TEXT("| `gasComponents` |"), Skill.Contains(TEXT("| `gasComponents` |")));
			TestTrue(TEXT("| `gas` |"), Skill.Contains(TEXT("| `gas` |")));
			TestTrue(TEXT("## Gameplay Ability System"), Skill.Contains(TEXT("## Gameplay Ability System")));
			TestTrue(TEXT("the guide teaches the no-C++ fireball"), Skill.Contains(TEXT("no project C++")) && Skill.Contains(TEXT("SetCooldown(")));
		});
	});

	Describe("sources", [this]()
	{
		It("gasAbilities answers a query", [this]()
		{
			// gasAbilities | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasAbilities"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("gasEffects answers a query", [this]()
		{
			// gasEffects | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasEffects"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("gasAttributeSets answers a query and lists the ready-made set", [this]()
		{
			// gasAttributeSets | where $type == "UnrealAIAttributeSet" | select Name
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasAttributeSets"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"UnrealAIAttributeSet"}]}},{"op":"select","fields":[{"name":"Name","expr":{"path":["Name"]}}]}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
			TestTrue(TEXT("UnrealAIAttributeSet is listed"), Json(Result).Contains(TEXT("UnrealAIAttributeSet")));
		});

		It("gasComponents answers a query", [this]()
		{
			// gasComponents | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gasComponents"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("describes AbilitySystemComponent with its runtime members", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("AbilitySystemComponent"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Member : { TEXT("GrantedAbilities"), TEXT("Attributes"), TEXT("OwnedTags"), TEXT("ActiveEffects") })
				TestTrue(FString::Printf(TEXT("lists name:%s"), Member), Schema.Contains(FString::Printf(TEXT("\"name\":\"%s\""), Member)));
		});
	});

	Describe("operations", [this]()
	{
		It("are listed on the gas source", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("GASOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Operation : { TEXT("EnableGameplayAbilities("), TEXT("CreateEffect("), TEXT("AddModifier("), TEXT("SetEffectTags("),
				TEXT("SetStacking("), TEXT("CreateAbility("), TEXT("SetCost("), TEXT("SetCooldown("), TEXT("SetAbilityTags("), TEXT("SetAbilityProjectile("),
				TEXT("SetDefault("), TEXT("AddAbilitySystem("), TEXT("AddStartingAbility("), TEXT("AddStartingEffect("), TEXT("BindAbilityInput("),
				TEXT("AddAttributeDisplay("), TEXT("GrantAbility("), TEXT("ActivateAbility("), TEXT("ApplyEffect("), TEXT("SetAttribute(") })
				TestTrue(FString::Printf(TEXT("lists %s"), Operation), Schema.Contains(Operation));
		});

		It("GrantAbility fails defensively on an unknown actor", [this]()
		{
			// gas | call GrantAbility("RqlSpecNoActor", "GameplayAbility")
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"GrantAbility","args":[{"lit":"RqlSpecNoActor"},{"lit":"GameplayAbility"}]}]})RQL"), true, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), bResultOk);
		});
	});

	Describe("authoring", [this]()
	{
		It("creates effects with durations, periods, modifiers, tags and stacking", [this]()
		{
			const FString Path = Folder();
			if (!AuthorFireball(*this, Path))
				return;
			const UGameplayEffect* Damage = GetDefault<UGameplayEffect>(GeneratedClass(Path + TEXT("/GE_Damage")));
			const UGameplayEffect* Regen = GetDefault<UGameplayEffect>(GeneratedClass(Path + TEXT("/GE_ManaRegen")));
			if (TestNotNull(TEXT("GE_Damage"), Damage) && TestEqual(TEXT("one damage modifier"), Damage->Modifiers.Num(), 1))
			{
				TestTrue(TEXT("instant"), Damage->DurationPolicy == EGameplayEffectDurationType::Instant);
				TestTrue(TEXT("on Damage"), Damage->Modifiers[0].Attribute == UUnrealAIAttributeSet::GetDamageAttribute());
				TestTrue(TEXT("SetByCaller.Damage"), Damage->Modifiers[0].ModifierMagnitude.GetSetByCallerFloat().DataTag == FGameplayTag(TAG_UnrealAI_SetByCaller_Damage));
			}
			if (TestNotNull(TEXT("GE_ManaRegen"), Regen) && TestEqual(TEXT("one regen modifier"), Regen->Modifiers.Num(), 1))
			{
				TestTrue(TEXT("infinite"), Regen->DurationPolicy == EGameplayEffectDurationType::Infinite);
				TestEqual(TEXT("every second"), Regen->Period.GetValueAtLevel(1.f), 1.f);
				float Magnitude = 0.f;
				TestTrue(TEXT("+5 mana"), Regen->Modifiers[0].ModifierMagnitude.GetStaticMagnitudeIfPossible(1.f, Magnitude) && Magnitude == 5.f
					&& Regen->Modifiers[0].Attribute == UUnrealAIAttributeSet::GetManaAttribute());
			}

			// gas | call SetEffectDuration("@@/GE_ManaRegen", "10", 0.5)      gas | call SetStacking("@@/GE_ManaRegen", "AggregateByTarget", 3)
			TestTrue(TEXT("duration"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetEffectDuration","args":[{"lit":"@@/GE_ManaRegen"},{"lit":"10"},{"lit":0.5}]}]})RQL")));
			TestTrue(TEXT("stacking"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetStacking","args":[{"lit":"@@/GE_ManaRegen"},{"lit":"AggregateByTarget"},{"lit":3}]}]})RQL")));
			// gas | call SetEffectTags("@@/GE_ManaRegen", "Granted", "RqlSpec.GAS.Regenerating")
			TestTrue(TEXT("granted tags"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetEffectTags","args":[{"lit":"@@/GE_ManaRegen"},{"lit":"Granted"},{"lit":"RqlSpec.GAS.Regenerating"}]}]})RQL")));
			if (Regen)
			{
				TestTrue(TEXT("10 s"), Regen->DurationPolicy == EGameplayEffectDurationType::HasDuration);
				TestEqual(TEXT("every 0.5 s"), Regen->Period.GetValueAtLevel(1.f), 0.5f);
				TestTrue(TEXT("stacks by target"), *FindFProperty<FProperty>(UGameplayEffect::StaticClass(), TEXT("StackingType"))->ContainerPtrToValuePtr<uint8>(Regen) == static_cast<uint8>(EGameplayEffectStackingType::AggregateByTarget));
				TestEqual(TEXT("3 stacks"), Regen->GetStackLimitCount(), 3);
				TestTrue(TEXT("grants the tag"), Regen->GetGrantedTags().HasTagExact(UGameplayTagsManager::Get().RequestGameplayTag(TEXT("RqlSpec.GAS.Regenerating"), false)));
			}

			// gasEffects | where $type == "GE_Damage_C" | select Summary
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("summary read"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gasEffects"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":[{"meta":"type"}]},{"lit":"GE_Damage_C"}]}},{"op":"select","fields":[{"name":"Summary","expr":{"path":["Summary"]}}]}]})RQL"), false, &Result));
			TestTrue(TEXT("the summary names the modifier"), Json(Result).Contains(TEXT("UnrealAIAttributeSet.Damage Add SetByCaller(SetByCaller.Damage)")));
			DeleteAssets(Path, { TEXT("GA_Fireball"), TEXT("GA_Fireball_Cost"), TEXT("GA_Fireball_Cooldown"), TEXT("GE_Damage"), TEXT("GE_ManaRegen") });
		});

		It("gives an ability a cost, a cooldown, tags and a projectile", [this]()
		{
			const FString Path = Folder();
			if (!AuthorFireball(*this, Path))
				return;
			// gas | call SetAbilityTags("@@/GA_Fireball", "Ability", "RqlSpec.GAS.Fireball")
			TestTrue(TEXT("ability tags"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetAbilityTags","args":[{"lit":"@@/GA_Fireball"},{"lit":"Ability"},{"lit":"RqlSpec.GAS.Fireball"}]}]})RQL")));
			// gas | call SetDefault("@@/GA_Fireball", "ProjectileOffset", "(X=50,Y=0,Z=20)")
			TestTrue(TEXT("a class default"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetDefault","args":[{"lit":"@@/GA_Fireball"},{"lit":"ProjectileOffset"},{"lit":"(X=50,Y=0,Z=20)"}]}]})RQL")));

			UGameplayAbility* Ability = GetMutableDefault<UGameplayAbility>(GeneratedClass(Path + TEXT("/GA_Fireball")));
			const UUnrealAIGameplayAbility* Data = Cast<UUnrealAIGameplayAbility>(Ability);
			if (!TestNotNull(TEXT("GA_Fireball derives from UnrealAIGameplayAbility"), Data))
				return;
			const UGameplayEffect* Cost = Ability->GetCostGameplayEffect();
			const UGameplayEffect* Cooldown = Ability->GetCooldownGameplayEffect();
			if (TestNotNull(TEXT("cost effect"), Cost) && TestEqual(TEXT("one cost modifier"), Cost->Modifiers.Num(), 1))
			{
				float Magnitude = 0.f;
				TestTrue(TEXT("-20 mana"), Cost->Modifiers[0].ModifierMagnitude.GetStaticMagnitudeIfPossible(1.f, Magnitude) && Magnitude == -20.f
					&& Cost->Modifiers[0].Attribute == UUnrealAIAttributeSet::GetManaAttribute());
				TestEqual(TEXT("named after the ability"), Cost->GetClass()->GetName(), FString(TEXT("GA_Fireball_Cost_C")));
			}
			if (TestNotNull(TEXT("cooldown effect"), Cooldown))
			{
				float Seconds = 0.f;
				TestTrue(TEXT("3 s"), Cooldown->DurationPolicy == EGameplayEffectDurationType::HasDuration && Cooldown->DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, Seconds) && Seconds == 3.f);
				TestTrue(TEXT("cooldown tags"), Ability->GetCooldownTags() && Ability->GetCooldownTags()->HasTagExact(UGameplayTagsManager::Get().RequestGameplayTag(TEXT("RqlSpec.GAS.Cooldown"), false)));
			}
			TestTrue(TEXT("ability tag"), Ability->GetAssetTags().HasTagExact(UGameplayTagsManager::Get().RequestGameplayTag(TEXT("RqlSpec.GAS.Fireball"), false)));
			TestTrue(TEXT("fires the default projectile"), Data->ProjectileClass.Get() == AUnrealAIGASProjectile::StaticClass());
			TestTrue(TEXT("hit effect"), Data->HitEffect.Get() == GeneratedClass(Path + TEXT("/GE_Damage")));
			TestEqual(TEXT("25 damage"), Data->HitMagnitude, 25.f);
			TestEqual(TEXT("offset"), Data->ProjectileOffset, FVector(50.f, 0.f, 20.f));
			DeleteAssets(Path, { TEXT("GA_Fireball"), TEXT("GA_Fireball_Cost"), TEXT("GA_Fireball_Cooldown"), TEXT("GE_Damage"), TEXT("GE_ManaRegen") });
		});

		It("gives an actor Blueprint an ability system, starting effects, an input binding and an attribute display", [this]()
		{
			const FString Path = Folder();
			if (!AuthorFireball(*this, Path))
				return;
			UPackage* Package = CreatePackage(*(Path + TEXT("/BP_Hero")));
			UBlueprint* Hero = FKismetEditorUtilities::CreateBlueprint(ACharacter::StaticClass(), Package, TEXT("BP_Hero"), BPTYPE_Normal);
			FAssetRegistryModule::AssetCreated(Hero);
			// gas | call AddAbilitySystem("@@/BP_Hero", "Health=150, MaxHealth=150, Mana=80")
			TestTrue(TEXT("ability system"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"AddAbilitySystem","args":[{"lit":"@@/BP_Hero"},{"lit":"Health=150, MaxHealth=150, Mana=80"}]}]})RQL")));
			// gas | call AddStartingEffect("@@/BP_Hero", "@@/GE_ManaRegen")
			TestTrue(TEXT("starting effect"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"AddStartingEffect","args":[{"lit":"@@/BP_Hero"},{"lit":"@@/GE_ManaRegen"}]}]})RQL")));
			// gas | call BindAbilityInput("@@/BP_Hero", "@@/GA_Fireball", "@@/IA_Fireball", "One")
			TestTrue(TEXT("input binding"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"BindAbilityInput","args":[{"lit":"@@/BP_Hero"},{"lit":"@@/GA_Fireball"},{"lit":"@@/IA_Fireball"},{"lit":"One"}]}]})RQL")));
			// gas | call AddAttributeDisplay("@@/BP_Hero", "HP {Health} MP {Mana}")
			TestTrue(TEXT("attribute display"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"AddAttributeDisplay","args":[{"lit":"@@/BP_Hero"},{"lit":"HP {Health} MP {Mana}"}]}]})RQL")));
			// gas | call AddAbilitySystem("@@/BP_Hero", "Mana=90")      a second call keeps the component and merges values
			TestTrue(TEXT("again"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"AddAbilitySystem","args":[{"lit":"@@/BP_Hero"},{"lit":"Mana=90"}]}]})RQL")));

			int32 Systems = 0;
			UUnrealAIAbilitySystemComponent* Template = nullptr;
			UUnrealAIAttributeTextComponent* Text = nullptr;
			for (USCS_Node* Node : Hero->SimpleConstructionScript->GetAllNodes())
			{
				if (UUnrealAIAbilitySystemComponent* Asc = Cast<UUnrealAIAbilitySystemComponent>(Node->ComponentTemplate))
				{
					Template = Asc;
					++Systems;
				}
				Text = Text ? Text : Cast<UUnrealAIAttributeTextComponent>(Node->ComponentTemplate);
			}
			TestEqual(TEXT("one ability system component"), Systems, 1);
			if (TestNotNull(TEXT("ability system template"), Template))
			{
				TestTrue(TEXT("the ready-made set"), Template->AttributeSets.Contains(UUnrealAIAttributeSet::StaticClass()));
				TestEqual(TEXT("Health=150"), Template->InitialAttributes.FindRef(TEXT("Health")), 150.f);
				TestEqual(TEXT("Mana merged to 90"), Template->InitialAttributes.FindRef(TEXT("Mana")), 90.f);
				TestTrue(TEXT("starting regen"), Template->StartingEffects.Num() == 1 && Template->StartingEffects[0].Get() == GeneratedClass(Path + TEXT("/GE_ManaRegen")));
				if (TestEqual(TEXT("one input binding"), Template->AbilityInputs.Num(), 1))
				{
					TestTrue(TEXT("binds the fireball"), Template->AbilityInputs[0].Ability.Get() == GeneratedClass(Path + TEXT("/GA_Fireball")));
					const UInputAction* Action = Template->AbilityInputs[0].InputAction;
					TestTrue(TEXT("created IA_Fireball"), Action && Action->GetName() == TEXT("IA_Fireball"));
					const UInputMappingContext* Context = Template->InputMappingContext;
					TestTrue(TEXT("maps One in IMC_Abilities"), Context && Context->GetName() == TEXT("IMC_Abilities") && Context->GetMappings().ContainsByPredicate(
						[Action](const FEnhancedActionKeyMapping& Mapping) { return Mapping.Action == Action && Mapping.Key == EKeys::One; }));
				}
			}
			if (TestNotNull(TEXT("attribute display template"), Text))
				TestEqual(TEXT("format"), Text->Format, FString(TEXT("HP {Health} MP {Mana}")));
			DeleteAssets(Path, { TEXT("BP_Hero"), TEXT("IMC_Abilities"), TEXT("IA_Fireball"), TEXT("GA_Fireball"), TEXT("GA_Fireball_Cost"), TEXT("GA_Fireball_Cooldown"),
				TEXT("GE_Damage"), TEXT("GE_ManaRegen") });
		});

		It("fails with actionable errors and changes nothing", [this]()
		{
			const FString Path = Folder();
			TSharedPtr<FJsonObject> Result;
			// gas | call CreateEffect("@@/GE_Bad", "Sometimes")
			TestFalse(TEXT("an unknown duration fails"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"CreateEffect","args":[{"lit":"@@/GE_Bad"},{"lit":"Sometimes"}]}]})RQL"), true, &Result));
			TestTrue(TEXT("says what a duration is"), Json(Result).Contains(TEXT("Infinite")));
			TestNull(TEXT("no asset made"), GeneratedClass(Path + TEXT("/GE_Bad")));
			// gas | call CreateEffect("@@/GE_Bad", "Instant", 1)
			TestFalse(TEXT("a periodic instant effect fails"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"CreateEffect","args":[{"lit":"@@/GE_Bad"},{"lit":"Instant"},{"lit":1}]}]})RQL"), true, &Result));
			// gas | call CreateEffect("@@/GE_Ok")
			TestTrue(TEXT("an effect"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"CreateEffect","args":[{"lit":"@@/GE_Ok"}]}]})RQL")));
			// gas | call AddModifier("@@/GE_Ok", "Manna", "Add", "1")
			TestFalse(TEXT("an unknown attribute fails"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"AddModifier","args":[{"lit":"@@/GE_Ok"},{"lit":"Manna"},{"lit":"Add"},{"lit":"1"}]}]})RQL"), true, &Result));
			TestTrue(TEXT("lists the attributes"), Json(Result).Contains(TEXT("UnrealAIAttributeSet.Mana")));
			// gas | call AddModifier("@@/GE_Ok", "Mana", "Add", "lots")
			TestFalse(TEXT("a bad magnitude fails"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"AddModifier","args":[{"lit":"@@/GE_Ok"},{"lit":"Mana"},{"lit":"Add"},{"lit":"lots"}]}]})RQL"), true, &Result));
			TestTrue(TEXT("names the magnitude forms"), Json(Result).Contains(TEXT("SetByCaller")));
			const UGameplayEffect* Effect = GetDefault<UGameplayEffect>(GeneratedClass(Path + TEXT("/GE_Ok")));
			TestTrue(TEXT("no modifier added"), Effect && Effect->Modifiers.IsEmpty());
			// gas | call SetEffectTags("@@/GE_Ok", "Sideways", "A.B")
			TestFalse(TEXT("an unknown tag kind fails"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetEffectTags","args":[{"lit":"@@/GE_Ok"},{"lit":"Sideways"},{"lit":"A.B"}]}]})RQL"), true, &Result));
			TestTrue(TEXT("lists the kinds"), Json(Result).Contains(TEXT("Granted")));
			// gas | call SetStacking("@@/GE_Ok")
			TestFalse(TEXT("an instant effect does not stack"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetStacking","args":[{"lit":"@@/GE_Ok"}]}]})RQL"), true, &Result));
			// gas | call SetCost("GameplayAbility", "Mana", 10)
			TestFalse(TEXT("a native ability is refused"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetCost","args":[{"lit":"GameplayAbility"},{"lit":"Mana"},{"lit":10}]}]})RQL"), true, &Result));
			TestTrue(TEXT("says to make a Blueprint"), Json(Result).Contains(TEXT("CreateAbility")));
			// gas | call CreateEffect("@@/GE_Ok")
			TestFalse(TEXT("a taken path fails"), Run(Path, TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"CreateEffect","args":[{"lit":"@@/GE_Ok"}]}]})RQL"), true, &Result));
			TestTrue(TEXT("says it exists"), Json(Result).Contains(TEXT("already exists")));
			DeleteAssets(Path, { TEXT("GE_Ok") });
		});

		It("needs the running game for the runtime operations", [this]()
		{
			// gas | call SetAttribute("Health", 10)
			TSharedPtr<FJsonObject> Result;
			TestFalse(TEXT("SetAttribute outside Play In Editor fails"), UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["gas"]},"stages":[{"op":"call","method":"SetAttribute","args":[{"lit":"Health"},{"lit":10}]}]})RQL"), true, &Result));
			TestTrue(TEXT("says how to start it"), Json(Result).Contains(TEXT("IsPlaying = true")));
		});
	});

	Describe("runtime", [this]()
	{
		It("keeps attributes within their maximums and turns Damage and Healing into Health", [this]()
		{
			FGameWorld Game;
			UUnrealAIAbilitySystemComponent* Asc = Game.SpawnWithAbilitySystem([](UUnrealAIAbilitySystemComponent& Component)
			{
				// Health before MaxHealth on purpose: maximums are applied first.
				Component.InitialAttributes.Add(TEXT("Health"), 250.f);
				Component.InitialAttributes.Add(TEXT("UnrealAIAttributeSet.MaxHealth"), 250.f);
				Component.InitialAttributes.Add(TEXT("Mana"), 40.f);
			});
			TestEqual(TEXT("Health 250"), Value(*Asc, TEXT("Health")), 250.f);
			TestEqual(TEXT("Mana 40"), Value(*Asc, TEXT("Mana")), 40.f);
			TestEqual(TEXT("Stamina default 100"), Value(*Asc, TEXT("Stamina")), 100.f);
			Asc->SetNumericAttributeBase(UUnrealAIAttributeSet::GetManaAttribute(), 500.f);
			TestEqual(TEXT("Mana clamped to MaxMana"), Value(*Asc, TEXT("Mana")), 100.f);

			UGameplayEffect* Hit = NewObject<UGameplayEffect>(GetTransientPackage(), TEXT("RqlSpecHit"));
			Hit->DurationPolicy = EGameplayEffectDurationType::Instant;
			FGameplayModifierInfo& Damage = Hit->Modifiers.AddDefaulted_GetRef();
			Damage.Attribute = UUnrealAIAttributeSet::GetDamageAttribute();
			Damage.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(100.f));
			Asc->ApplyGameplayEffectToSelf(Hit, 1.f, Asc->MakeEffectContext());
			TestEqual(TEXT("Damage subtracts from Health"), Value(*Asc, TEXT("Health")), 150.f);
			TestEqual(TEXT("Damage resets"), Value(*Asc, TEXT("Damage")), 0.f);
			Asc->ApplyGameplayEffectToSelf(Hit, 1.f, Asc->MakeEffectContext());
			Asc->ApplyGameplayEffectToSelf(Hit, 1.f, Asc->MakeEffectContext());
			TestEqual(TEXT("Health stops at 0"), Value(*Asc, TEXT("Health")), 0.f);
			Damage.Attribute = UUnrealAIAttributeSet::GetHealingAttribute();
			Asc->ApplyGameplayEffectToSelf(Hit, 1.f, Asc->MakeEffectContext());
			TestEqual(TEXT("Healing adds to Health"), Value(*Asc, TEXT("Health")), 100.f);
			bool bFound = false;
			TestEqual(TEXT("GetAttributeValue"), Asc->GetAttributeValue(TEXT("Health"), bFound), 100.f);
			TestTrue(TEXT("found"), bFound);
		});

		It("runs the authored fireball: cost, cooldown, projectile damage and regeneration", [this]()
		{
			const FString Path = Folder();
			if (!AuthorFireball(*this, Path))
				return;
			UClass* Fireball = GeneratedClass(Path + TEXT("/GA_Fireball"));
			UClass* Regen = GeneratedClass(Path + TEXT("/GE_ManaRegen"));
			{
				FGameWorld Game;
				UUnrealAIAbilitySystemComponent* Hero = Game.SpawnWithAbilitySystem([Fireball](UUnrealAIAbilitySystemComponent& Component)
				{
					Component.InitialAttributes.Add(TEXT("Mana"), 50.f);
					Component.StartingAbilities.Add(Fireball);
				});
				UUnrealAIAbilitySystemComponent* Dummy = Game.SpawnWithAbilitySystem([](UUnrealAIAbilitySystemComponent& Component)
				{
					Component.InitialAttributes.Add(TEXT("MaxHealth"), 200.f);
					Component.InitialAttributes.Add(TEXT("Health"), 200.f);
				});
				TestNotNull(TEXT("the fireball is granted at BeginPlay"), Hero->FindAbilitySpecFromClass(Fireball));

				TestTrue(TEXT("first cast"), Hero->TryActivateAbilityByClass(Fireball));
				TestEqual(TEXT("costs 20 mana"), Value(*Hero, TEXT("Mana")), 30.f);
				TestTrue(TEXT("cooling down"), Hero->GetOwnedGameplayTags().HasTagExact(UGameplayTagsManager::Get().RequestGameplayTag(TEXT("RqlSpec.GAS.Cooldown"), false)));
				TestFalse(TEXT("no second cast during the cooldown"), Hero->TryActivateAbilityByClass(Fireball));
				TestEqual(TEXT("no second cost"), Value(*Hero, TEXT("Mana")), 30.f);

				AUnrealAIGASProjectile* Projectile = nullptr;
				for (TActorIterator<AUnrealAIGASProjectile> It(Game.World); It; ++It)
					Projectile = *It;
				if (TestNotNull(TEXT("a projectile was fired"), Projectile))
				{
					TestTrue(TEXT("it carries the damage spec"), Projectile->HitEffectSpec.IsValid());
					TestTrue(TEXT("it hits the dummy"), Projectile->ApplyHitEffect(Dummy->GetOwner()));
					TestEqual(TEXT("25 damage"), Value(*Dummy, TEXT("Health")), 175.f);
				}

				// Regeneration: an infinite effect that adds 5 mana every second.
				Hero->ApplyGameplayEffectToSelf(GetDefault<UGameplayEffect>(Regen), 1.f, Hero->MakeEffectContext());
				const float Before = Value(*Hero, TEXT("Mana"));
				for (int32 Step = 0; Step < 12; ++Step)
					Game.World->Tick(LEVELTICK_All, 0.1f);
				TestTrue(TEXT("mana regenerates"), Value(*Hero, TEXT("Mana")) > Before);
			}
			DeleteAssets(Path, { TEXT("GA_Fireball"), TEXT("GA_Fireball_Cost"), TEXT("GA_Fireball_Cooldown"), TEXT("GE_Damage"), TEXT("GE_ManaRegen") });
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
