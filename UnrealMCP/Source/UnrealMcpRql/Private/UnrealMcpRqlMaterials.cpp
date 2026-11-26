// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlMaterials.h"

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ITransaction.h"
#include "RHIGlobals.h"
#include "RHIStrings.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Package.h"

#include <memory>
#include <utility>
#include <vector>

/**
 * Material compile feedback (docs/rql.md "Materials"): a material's compile errors per feature level and shader platform
 * (CompileErrors, Compile()), its usage flags (UsageFlags) and the usage flags the components using it in the open level
 * need but it lacks (MissingUsages): a cooked game draws the default material wherever either goes wrong, while the
 * editor only logs it (and sets a missing usage flag in memory, unsaved).
 */
namespace UnrealMcpRqlMaterialsLocal
{
	using namespace UnrealMcpRqlUtil;

	/** At most this many MissingUsages rows (one per component and flag). */
	constexpr int32 MaxMissingUsages = 50;

	struct FMaterialsUsage
	{
		EMaterialUsage Usage;
		const TCHAR* Name; // the UMaterial property (the Details panel's Usage section)
	};

	const FMaterialsUsage MaterialsUsages[] = {
		{ MATUSAGE_SkeletalMesh, TEXT("bUsedWithSkeletalMesh") },
		{ MATUSAGE_MorphTargets, TEXT("bUsedWithMorphTargets") },
		{ MATUSAGE_Clothing, TEXT("bUsedWithClothing") },
		{ MATUSAGE_SplineMesh, TEXT("bUsedWithSplineMeshes") },
		{ MATUSAGE_InstancedStaticMeshes, TEXT("bUsedWithInstancedStaticMeshes") },
		{ MATUSAGE_Nanite, TEXT("bUsedWithNanite") },
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
		{ MATUSAGE_Voxels, TEXT("bUsedWithVoxels") },
#endif
		{ MATUSAGE_StaticLighting, TEXT("bUsedWithStaticLighting") },
		{ MATUSAGE_ParticleSprites, TEXT("bUsedWithParticleSprites") },
		{ MATUSAGE_BeamTrails, TEXT("bUsedWithBeamTrails") },
		{ MATUSAGE_MeshParticles, TEXT("bUsedWithMeshParticles") },
		{ MATUSAGE_NiagaraSprites, TEXT("bUsedWithNiagaraSprites") },
		{ MATUSAGE_NiagaraRibbons, TEXT("bUsedWithNiagaraRibbons") },
		{ MATUSAGE_NiagaraMeshParticles, TEXT("bUsedWithNiagaraMeshParticles") },
		{ MATUSAGE_GeometryCollections, TEXT("bUsedWithGeometryCollections") },
		{ MATUSAGE_GeometryCache, TEXT("bUsedWithGeometryCache") },
		{ MATUSAGE_Water, TEXT("bUsedWithWater") },
		{ MATUSAGE_HairStrands, TEXT("bUsedWithHairStrands") },
	};

	const TCHAR* UsageName(EMaterialUsage Usage)
	{
		for (const FMaterialsUsage& Entry : MaterialsUsages)
		{
			if (Entry.Usage == Usage)
				return Entry.Name;
		}
		return TEXT("?");
	}

	/** The material resource for @p Level (the engine looks resources up by shader platform since UE 5.7). */
	FMaterialResource* MaterialsResource(UMaterialInterface* Material, ERHIFeatureLevel::Type Level, EShaderPlatform Platform)
	{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		return Material->GetMaterialResource(Level);
#else
		return Platform == SP_NumPlatforms ? nullptr : Material->GetMaterialResource(Platform);
#endif
	}

	/** The usages the mesh kind of @p Component needs its materials to have (the engine falls back without them). */
	TArray<EMaterialUsage, TInlineAllocator<4>> MaterialsNeeds(const UPrimitiveComponent* Component)
	{
		TArray<EMaterialUsage, TInlineAllocator<4>> Out;
		if (Component->IsA<USkinnedMeshComponent>())
			Out.Add(MATUSAGE_SkeletalMesh);
		const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component);
		if (!MeshComponent)
			return Out;
		if (MeshComponent->IsA<UInstancedStaticMeshComponent>())
			Out.Add(MATUSAGE_InstancedStaticMeshes);
		if (MeshComponent->IsA<USplineMeshComponent>())
			Out.Add(MATUSAGE_SplineMesh);
		const UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
		if (Mesh && Mesh->IsNaniteEnabled())
		{
			Out.Add(MATUSAGE_Nanite);
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
			if (Mesh->GetNaniteSettings().ShapePreservation == ENaniteShapePreservation::Voxelize)
				Out.Add(MATUSAGE_Voxels);
#endif
		}
		return Out;
	}

	rql::Value Record(std::vector<std::pair<rql::Value, rql::Value>> Fields)
	{
		return rql::Value::map(std::make_shared<rql::OwnedMap>(nullptr, std::move(Fields)));
	}

	rql::Value ErrorList(const TArray<FUnrealMcpMaterialCompileError>& Errors)
	{
		std::vector<rql::Value> Out;
		for (const FUnrealMcpMaterialCompileError& Error : Errors)
		{
			Out.push_back(Record({ { Str(TEXT("FeatureLevel")), Str(Error.FeatureLevel) }, { Str(TEXT("Platform")), Str(Error.Platform) },
				{ Str(TEXT("Message")), Str(Error.Message) } }));
		}
		return rql::Value::list_of(std::move(Out));
	}

	/** The asset that holds @p Material's usage flags: since UE 5.8 an instance keeps its own (the editor sets a missing one on it). */
	const UMaterialInterface* MaterialsUsageOwner(const UMaterialInterface* Material)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		return Material->GetMaterial();
#else
		return Material;
#endif
	}

	bool MaterialsHasUsage(const UMaterialInterface* Material, EMaterialUsage Usage)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		const UMaterial* Base = Material->GetMaterial();
		return Base && Base->GetUsageByFlag(Usage);
#else
		return Material->GetUsageByFlag(Usage);
#endif
	}

	bool MaterialsIsUnsaved(const UObject* Asset)
	{
		const UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
		return Package && Package->IsDirty();
	}

	rql::Value UsageFlags(UMaterialInterface* Material)
	{
		TArray<FString> On;
		for (const FMaterialsUsage& Entry : MaterialsUsages)
		{
			if (MaterialsHasUsage(Material, Entry.Usage))
				On.Add(Entry.Name);
		}
		return StrList(On);
	}

	/**
	 * The components of the open level that draw @p Material's base material (directly or through an instance) on a mesh
	 * kind whose usage flag the drawn material lacks (its base's before UE 5.8): off, or on but unsaved (the editor sets a
	 * missing flag in memory and marks the asset unsaved; a cooked game reads the saved flag).
	 */
	rql::Value MissingUsages(FUnrealMcpRqlTypes& Types, UMaterialInterface* Material)
	{
		std::vector<rql::Value> Out;
		UMaterial* Base = Material->GetMaterial();
		UWorld* World = EditorWorld();
		if (!Base || !World)
			return rql::Value::list_of(std::move(Out));
		for (TActorIterator<AActor> It(World); It && static_cast<int32>(Out.size()) < MaxMissingUsages; ++It)
		{
			TInlineComponentArray<UPrimitiveComponent*> Components(*It);
			for (UPrimitiveComponent* Component : Components)
			{
				const TArray<EMaterialUsage, TInlineAllocator<4>> Needs = MaterialsNeeds(Component);
				if (Needs.IsEmpty())
					continue;
				TArray<UMaterialInterface*> Used;
				Component->GetUsedMaterials(Used);
				TArray<const UMaterialInterface*, TInlineAllocator<2>> Owners; // one row per flag holder, however many slots use it
				for (const UMaterialInterface* Each : Used)
				{
					if (Each && Each->GetMaterial() == Base)
						Owners.AddUnique(MaterialsUsageOwner(Each));
				}
				for (const UMaterialInterface* Owner : Owners)
				{
					const bool bUnsaved = MaterialsIsUnsaved(Owner);
					const FString OwnerPath = Owner->GetOutermost()->GetName();
					for (const EMaterialUsage Usage : Needs)
					{
						const bool bOn = MaterialsHasUsage(Owner, Usage);
						if ((bOn && !bUnsaved) || static_cast<int32>(Out.size()) >= MaxMissingUsages)
							continue;
						const FString Problem = bOn
							? FString::Printf(TEXT("%s is on but %s has unsaved changes: save it, a cooked game reads the saved flag"), UsageName(Usage), *OwnerPath)
							: FString::Printf(TEXT("%s is off on %s: the game draws the default material here"), UsageName(Usage), *OwnerPath);
						Out.push_back(Record({ { Str(TEXT("Usage")), Str(UsageName(Usage)) }, { Str(TEXT("Actor")), Str(It->GetActorNameOrLabel()) },
							{ Str(TEXT("Component")), Types.WrapObject(Component) }, { Str(TEXT("Problem")), Str(Problem) } }));
					}
				}
			}
		}
		return rql::Value::list_of(std::move(Out));
	}

	/**
	 * Recompile @p Material now (a material instance: its static permutation), as the material editor's Apply does. A
	 * recompile changes nothing saved, so a package that was saved stays so (RecompileMaterial marks it unsaved; left
	 * that way, MissingUsages would report every flag as unsaved).
	 */
	void Recompile(UMaterialInterface* Material)
	{
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		UPackage* Package = Material->GetOutermost();
		const bool bWasUnsaved = Package->IsDirty();
		if (UMaterial* Base = Cast<UMaterial>(Material))
			UMaterialEditingLibrary::RecompileMaterial(Base);
		else if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material))
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		else
			Throw(FString::Printf(TEXT("%s is a %s: only materials and material instances compile"), *Material->GetName(), *Material->GetClass()->GetName()));
		if (!bWasUnsaved)
			Package->SetDirtyFlag(false);
	}

	void MaterialCompileMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* Records = T.Registry().list_type(rql::builtin::any_type(), false);
		FUnrealMcpRqlTypes::SetMember(D, Member("CompileErrors", Records, [](void* Self)
		{
			// No wait here (a query over many materials would block the editor): a compile in flight is one row saying so.
			TArray<FString> Pending;
			TArray<FUnrealMcpMaterialCompileError> Errors = UnrealMcpRqlMaterialCompileErrors(static_cast<UMaterialInterface*>(Self), /*bWait*/ false, nullptr, &Pending);
			for (const FString& Level : Pending)
				Errors.Add({ Level, FString(), TEXT("still compiling: call Compile() to wait for the result") });
			return ErrorList(Errors);
		}, {}, "compile errors per feature level and shader platform (FeatureLevel Platform Message) of the last finished compile; empty when it compiles. A material with errors draws as the default material"));
		FUnrealMcpRqlTypes::SetMember(D, Member("UsageFlags", T.Registry().list_type(rql::builtin::string_type(), false), [](void* Self)
		{
			return UsageFlags(static_cast<UMaterialInterface*>(Self));
		}, {}, "the usage flags that are on (bUsedWithNanite, bUsedWithInstancedStaticMeshes, ...; an instance before UE 5.8: its base material's)"));
		FUnrealMcpRqlTypes::SetMember(D, Member("MissingUsages", Records, [Types](void* Self)
		{
			return MissingUsages(*Types, static_cast<UMaterialInterface*>(Self));
		}, {}, "components of the open level whose mesh kind needs a usage flag this material lacks or has not saved (Usage Actor Component Problem): a cooked game draws the default material there"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("Compile", {}, rql::builtin::any_type(),
			[Types](void* Self, const std::vector<rql::Value>&) -> rql::Value
			{
				UMaterialInterface* Material = static_cast<UMaterialInterface*>(Self);
				Recompile(Material);
				TArray<FString> Compiled;
				const TArray<FUnrealMcpMaterialCompileError> Errors = UnrealMcpRqlMaterialCompileErrors(Material, /*bWait*/ true, &Compiled);
				const rql::Value Missing = MissingUsages(*Types, Material);
				std::vector<std::pair<rql::Value, rql::Value>> Out;
				Out.emplace_back(Str(TEXT("Succeeded")), rql::Value::boolean(Compiled.Num() > 0 && Errors.IsEmpty()));
				Out.emplace_back(Str(TEXT("Compiled")), StrList(Compiled));
				Out.emplace_back(Str(TEXT("Errors")), ErrorList(Errors));
				Out.emplace_back(Str(TEXT("MissingUsages")), Missing);
				if (Compiled.IsEmpty())
					Out.emplace_back(Str(TEXT("Note")), Str(FApp::CanEverRender()
						? TEXT("no shaders were compiled for it")
						: TEXT("not compiled: the editor runs without a renderer (-nullrhi); start it with -RenderOffscreen")));
				return Record(std::move(Out));
			}, "recompile now and wait; returns {Succeeded, Compiled (feature levels / platforms), Errors [{FeatureLevel, Platform, Message}], MissingUsages}"));
	}
}

TArray<FUnrealMcpMaterialCompileError> UnrealMcpRqlMaterialCompileErrors(UMaterialInterface* Material, bool bWait, TArray<FString>* OutCompiled,
	TArray<FString>* OutPending)
{
	using namespace UnrealMcpRqlMaterialsLocal;
	TArray<FUnrealMcpMaterialCompileError> Out;
	if (!Material)
		return Out;
	for (int32 Index = 0; Index < ERHIFeatureLevel::Num; ++Index)
	{
		const ERHIFeatureLevel::Type Level = static_cast<ERHIFeatureLevel::Type>(Index);
		const EShaderPlatform Platform = GShaderPlatformForFeatureLevel[Level];
		FMaterialResource* Resource = MaterialsResource(Material, Level, Platform);
		if (!Resource)
			continue;
		FString LevelName;
		GetFeatureLevelName(Level, LevelName);
		if (!Resource->IsCompilationFinished())
		{
			if (bWait)
				Resource->FinishCompilation();
			else if (OutPending)
				OutPending->Add(LevelName);
		}
		const TArray<FString>& Errors = Resource->GetCompileErrors();
		if (!Resource->GetGameThreadShaderMap() && Errors.IsEmpty())
			continue; // never compiled for this feature level
		const FString PlatformName = Platform == SP_NumPlatforms ? FString(TEXT("?")) : LexToString(Platform, /*bError*/ false);
		if (OutCompiled)
			OutCompiled->AddUnique(LevelName + TEXT(" ") + PlatformName);
		for (const FString& Message : Errors)
			Out.Add({ LevelName, PlatformName, Message });
	}
	return Out;
}

void UnrealMcpRqlAddMaterials(FUnrealMcpRqlSession& Session)
{
	Session.Types().AddClassHook(UMaterialInterface::StaticClass(), &UnrealMcpRqlMaterialsLocal::MaterialCompileMembers);
}
