// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAINiagaraRqlApi.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAINiagaraStack.h"
#include "UnrealAINiagaraTemplates.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "IAssetTools.h"
#include "Interfaces/IProjectManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "NiagaraActor.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "Engine/StaticMesh.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraShared.h"
#include "NiagaraSystem.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"

namespace UnrealAINiagaraRqlApiLocal
{
	using namespace UnrealAINiagara;

	/** After an edit: the package is unsaved and the system recompiles in the background (Compile waits for it). */
	void Changed(UNiagaraSystem& System)
	{
		System.MarkPackageDirty();
		System.RequestCompile(false);
	}

	bool IsSystemStack(const FString& Emitter)
	{
		return Emitter.IsEmpty() || Normalize(Emitter) == TEXT("system");
	}

	/** A module script by path, or by asset name ("CurlNoiseForce", "Curl Noise Force"): Niagara's own modules first. */
	UNiagaraScript* ResolveModuleScript(const FString& Module)
	{
		if (Module.StartsWith(TEXT("/")))
		{
			UNiagaraScript* Script = Cast<UNiagaraScript>(UnrealMcpRql::LoadObject(UNiagaraScript::StaticClass(), Module));
			return Script ? Script : UnrealMcpRql::Failed<UNiagaraScript*>(FString::Printf(TEXT("no NiagaraScript at '%s'"), *Module));
		}
		TArray<FAssetData> Assets;
		IAssetRegistry::GetChecked().GetAssetsByClass(UNiagaraScript::StaticClass()->GetClassPathName(), Assets);
		const FString Wanted = Normalize(Module);
		const FAssetData* Best = nullptr;
		int32 BestRank = MAX_int32;
		TArray<FString> Similar;
		for (const FAssetData& Data : Assets)
		{
			const FString Path = Data.PackageName.ToString();
			const FString Name = Normalize(Data.AssetName.ToString());
			if (Name != Wanted)
			{
				if (Name.Contains(Wanted) && Path.Contains(TEXT("/Modules/")) && Similar.Num() < 8)
					Similar.Add(Path);
				continue;
			}
			// Engine modules first, deprecated or debug copies last.
			int32 Rank = Path.StartsWith(TEXT("/Niagara/Modules/")) ? 0 : Path.StartsWith(TEXT("/Niagara/")) ? 1 : Path.Contains(TEXT("/Modules/")) ? 2 : 3;
			if (Path.Contains(TEXT("Deprecated")) || Path.Contains(TEXT("Legacy")))
				Rank += 10;
			if (Rank < BestRank)
				Best = &Data, BestRank = Rank;
		}
		if (Best)
			return Cast<UNiagaraScript>(Best->GetAsset());
		return UnrealMcpRql::Failed<UNiagaraScript*>(FString::Printf(TEXT("no Niagara module script named '%s'%s; pass a NiagaraScript path (unreal.assets(\"NiagaraScript\", \"/Niagara/Modules\") lists the engine's)"),
			*Module, Similar.Num() ? *FString::Printf(TEXT(" (similar: %s)"), *FString::Join(Similar, TEXT(", "))) : TEXT("")));
	}

	/** The emitter handles an operation covers: one by name, or every one for "". */
	TArray<FNiagaraEmitterHandle*> Targets(UNiagaraSystem& System, const FString& Emitter)
	{
		TArray<FNiagaraEmitterHandle*> Out;
		if (Emitter.IsEmpty())
		{
			for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
				Out.Add(&Handle);
			if (Out.Num() == 0)
				UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no emitters: AddEmitter first"), *System.GetName()));
		}
		else if (FNiagaraEmitterHandle* Handle = FindEmitter(System, Emitter))
		{
			Out.Add(Handle);
		}
		return Out;
	}

	/** Whether an emitter is a Niagara Fluids grid simulation (its modules simulate on a Grid2D / Grid3D). */
	bool IsFluid(FSystemStack& Stack, const FString& Emitter)
	{
		for (const FModuleRef& Module : Stack.Modules(Emitter))
		{
			const UNiagaraScript* Script = Module.Item->GetModuleNode().FunctionScript;
			if (Script && Script->GetPathName().StartsWith(TEXT("/NiagaraFluids/")))
				return true;
		}
		return false;
	}

	/** The module of @p Emitter whose name is one of @p Names; false (no Fail) when it has none. */
	bool FindAnyModule(FSystemStack& Stack, const FString& Emitter, const TArray<FString>& Names, FModuleRef& Out)
	{
		for (const FModuleRef& Module : Stack.Modules(Emitter))
		{
			if (Names.Contains(Normalize(Module.Name)))
				return (Out = Module), true;
		}
		return false;
	}

	bool MeshDistanceFieldsEnabled()
	{
		static const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GenerateMeshDistanceFields"));
		return Variable && Variable->GetInt() != 0;
	}

	const TCHAR* DistanceFieldNote = TEXT("mesh distance fields are off in this project: unreal.settings(\"Rendering\") | set bGenerateMeshDistanceFields = true, then restart the editor (meshes rebuild their distance fields)");

	/**
	 * A Niagara Fluids grid's world collision: the Grid 3D Compute Boundary switches (mesh distance fields, depth map,
	 * global distance field) and the rigid mesh collider search (physics bodies of the meshes in the grid, static ones too).
	 */
	FString FluidCollision(FSystemStack& Stack, const FString& Emitter, const FString& Mode)
	{
		const FString Wanted = Normalize(Mode);
		const bool bOff = Wanted == TEXT("none");
		const bool bAuto = Wanted == TEXT("auto");
		const bool bPhysics = bAuto || Wanted == TEXT("raytrace") || Wanted == TEXT("raytraced") || Wanted == TEXT("trace") || Wanted == TEXT("physics");
		const bool bDistanceFields = bAuto || Wanted.StartsWith(TEXT("distancefield"));
		const bool bDepth = Wanted == TEXT("scenedepth") || Wanted == TEXT("depthbuffer") || Wanted == TEXT("depth");
		const bool bGlobal = Wanted.StartsWith(TEXT("globaldistancefield"));
		if (!bOff && !bPhysics && !bDistanceFields && !bDepth && !bGlobal)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("unknown collision mode '%s' (Auto, SceneDepth, DistanceField, GlobalDistanceField, RayTrace, None)"), *Mode));
		const bool bFields = MeshDistanceFieldsEnabled();
		// Input (normalized) -> the value for this mode; inputs a mode does not name stay as they are, None turns all off.
		// Distance-field queries do not compile in a project without mesh distance fields: Auto turns them off there.
		TMap<FString, FString> Values;
		const auto Want = [&](const TCHAR* Input, bool bOn) { if (bOn || bOff) Values.Add(Input, bOn && !bOff ? TEXT("true") : TEXT("false")); };
		Want(TEXT("findcolliders"), bPhysics);
		if (bAuto && !bFields)
			Values.Add(TEXT("usemeshdistancefields"), TEXT("false"));
		else
			Want(TEXT("usemeshdistancefields"), bDistanceFields);
		Want(TEXT("usedepthmapcollisions"), bDepth);
		Want(TEXT("useglobaldistancefieldcollisions"), bGlobal);
		// Collect first: every SetInput rebuilds the stack, so the entries read here go stale.
		TArray<TPair<FString, FString>> Targets;
		FString Query;
		for (const FModuleRef& Module : Stack.Modules(Emitter))
		{
			for (const FInputRef& Input : Inputs(Module))
			{
				const FString Name = Normalize(Input.Path);
				if (Values.Contains(Name) && !Input.Path.Contains(TEXT(".")))
					Targets.Add({ Module.Stage + TEXT("/") + Module.Name, Input.Path });
				else if (Name == TEXT("collisionquery") && bPhysics && !bOff)
					Query = Module.Stage + TEXT("/") + Module.Name;
			}
		}
		if (Targets.Num() == 0)
			return FString::Printf(TEXT("%s: skipped (no world-collision inputs: a particle source that feeds the fluid grid)"), *Emitter);
		if ((Wanted.StartsWith(TEXT("distancefield")) || bGlobal) && !bFields)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: %s; or use RayTrace (physics shapes) or SceneDepth"), *Emitter, DistanceFieldNote));
		TArray<FString> Changes;
		for (const TPair<FString, FString>& Target : Targets)
		{
			const FString New = Stack.SetInput(Emitter, Target.Key, Target.Value, Values[Normalize(Target.Value)]);
			if (New.IsEmpty())
				return FString();
			Changes.Add(FString::Printf(TEXT("%s = %s"), *Target.Value, *New));
		}
		if (!Query.IsEmpty())
		{
			// The collider search ignores static meshes by default: props usually are static.
			if (Stack.SetInput(Emitter, Query, TEXT("CollisionQuery"), TEXT("OnlyUseMoveable=false")).IsEmpty())
				return FString();
			Changes.Add(TEXT("CollisionQuery OnlyUseMoveable = false (static meshes collide too)"));
		}
		FString Out = FString::Printf(TEXT("%s (fluid grid): %s"), *Emitter, *FString::Join(Changes, TEXT("; ")));
		if (bAuto && !bFields)
			Out += FString::Printf(TEXT("; note: mesh distance field collision stays off (%s; then EnableCollision(system, \"%s\", \"DistanceField\") for exact mesh shapes)"), DistanceFieldNote, *Emitter);
		if (bPhysics && !bOff)
			Out += TEXT("; note: the collider search uses the physics (simple collision) shapes of meshes inside the grid; props need collision");
		return Out;
	}

	/** Particle collision: the Collision module in Particle Update with the query type of @p Mode. */
	FString ParticleCollision(UUnrealAINiagaraRqlApi& Api, UNiagaraSystem& System, FSystemStack& Stack, FNiagaraEmitterHandle& Handle, const FString& Mode)
	{
		const FString Emitter = Handle.GetName().ToString();
		FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		const bool bGpu = Data && Data->SimTarget == ENiagaraSimTarget::GPUComputeSim;
		FString Wanted = Normalize(Mode);
		if (Wanted == TEXT("auto"))
			Wanted = bGpu ? TEXT("scenedepth") : TEXT("raytrace");
		const TArray<FString> CollisionNames = { TEXT("collision") };
		FModuleRef Module;
		const bool bHas = FindAnyModule(Stack, Emitter, CollisionNames, Module);
		if (Wanted == TEXT("none"))
		{
			if (bHas)
				Api.SetModuleEnabled(&System, Emitter, Module.Stage + TEXT("/") + Module.Name, false);
			return FString::Printf(TEXT("%s: collision %s"), *Emitter, bHas ? TEXT("disabled") : TEXT("was off"));
		}
		// The Collision module's query type entries: CPU "Ray Traced" / "Analytical Planes", GPU "GPU Depth Buffer" /
		// "GPU Distance Fields" / hardware ray tracing; picked by keyword so renamed entries still match.
		const bool bRayTrace = Wanted == TEXT("raytrace") || Wanted == TEXT("raytraced") || Wanted == TEXT("trace");
		const bool bDistanceField = Wanted.StartsWith(TEXT("distancefield"));
		const bool bDepth = Wanted == TEXT("scenedepth") || Wanted == TEXT("depthbuffer") || Wanted == TEXT("depth");
		if (!bRayTrace && !bDistanceField && !bDepth)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("unknown collision mode '%s' (Auto, SceneDepth, DistanceField, RayTrace, None)"), *Mode));
		if (bDistanceField && !MeshDistanceFieldsEnabled())
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: %s; or use SceneDepth (GPU) or RayTrace (CPU)"), *Emitter, DistanceFieldNote));
		const bool bNeedsGpu = bDepth || bDistanceField || (bRayTrace && bGpu);
		const TCHAR* Keyword = bRayTrace ? TEXT("ray") : bDistanceField ? TEXT("distance") : TEXT("depth");
		// The Collision module's query type entry for the mode (CPU / GPU Collision Type enum assets), before any change.
		const UEnum* Enum = Cast<UEnum>(UnrealMcpRql::LoadObject(UEnum::StaticClass(),
			bNeedsGpu ? TEXT("/Niagara/Enums/ENiagara_GPUCollisionType") : TEXT("/Niagara/Enums/ENiagara_CPUCollisionType")));
		FString Entry;
		TArray<FString> Entries;
		for (int32 Index = 0; Enum && Index < Enum->NumEnums() - 1; ++Index)
		{
			const FString Display = Enum->GetDisplayNameTextByIndex(Index).ToString();
			Entries.Add(Display);
			if (Entry.IsEmpty() && Normalize(Display).Contains(Keyword))
				Entry = Display;
		}
		if (Entry.IsEmpty())
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: the %s collision types (%s) have no %s mode%s"), *Emitter,
				bNeedsGpu ? TEXT("GPU") : TEXT("CPU"), *FString::Join(Entries, TEXT(", ")), *Mode,
				bRayTrace ? TEXT(": use RayTrace on a CPU emitter (SetEmitterProperty SimTarget CPU) or SceneDepth") : TEXT("")));

		TArray<FString> Changes;
		if (bNeedsGpu && !bGpu)
		{
			// Depth-buffer and distance-field queries run in the GPU simulation; a GPU emitter needs fixed bounds to stay visible.
			if (Api.SetEmitterProperty(&System, Emitter, TEXT("SimTarget"), TEXT("GPUComputeSim")).IsEmpty())
				return FString();
			Changes.Add(TEXT("SimTarget = GPU"));
			if (Data && Data->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Dynamic)
			{
				if (Api.SetEmitterProperty(&System, Emitter, TEXT("CalculateBoundsMode"), TEXT("Fixed")).IsEmpty()
					|| Api.SetEmitterProperty(&System, Emitter, TEXT("FixedBounds"), TEXT("(Min=(X=-500,Y=-500,Z=0),Max=(X=500,Y=500,Z=800))")).IsEmpty())
					return FString();
				Changes.Add(TEXT("fixed bounds 10 x 10 x 8 m (SetEmitterProperty FixedBounds to resize)"));
			}
			// The GPU inputs show only now: read the stack again.
			Stack.Refresh(Emitter);
			if (bHas && !FindAnyModule(Stack, Emitter, CollisionNames, Module))
				return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: the Collision module is gone after the GPU switch"), *Emitter));
		}
		if (!bHas)
		{
			const FString Added = Api.AddModule(&System, Emitter, TEXT("Particle Update"), TEXT("/Niagara/Modules/Collision/Collision.Collision"));
			if (Added.IsEmpty() || !FindAnyModule(Stack, Emitter, CollisionNames, Module))
				return FString();
			Changes.Add(TEXT("added ") + Added);
		}
		else if (!Module.Item->GetIsEnabled())
		{
			Api.SetModuleEnabled(&System, Emitter, Module.Stage + TEXT("/") + Module.Name, true);
			Changes.Add(TEXT("enabled ") + Module.Name);
		}
		// The query type input of the simulation target: CPU Collision Type or GPU Collision Type.
		FString TypeInput;
		for (const FInputRef& Input : Inputs(Module))
		{
			const FString Name = Normalize(Input.Path);
			if (Input.Input->GetInputType().GetEnum() == Enum && Name.Contains(TEXT("collisiontype")))
				TypeInput = Input.Path;
		}
		if (TypeInput.IsEmpty())
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: the Collision module shows no %s Collision Type input (see the Inputs member)"),
				*Emitter, bNeedsGpu ? TEXT("GPU") : TEXT("CPU")));
		const FString New = Stack.SetInput(Emitter, Module.Stage + TEXT("/") + Module.Name, TypeInput, Entry);
		if (New.IsEmpty())
			return FString();
		Changes.Add(FString::Printf(TEXT("%s = %s"), *TypeInput, *New));
		FString Out = FString::Printf(TEXT("%s: %s"), *Emitter, *FString::Join(Changes, TEXT("; ")));
		if (bDepth)
			Out += TEXT("; note: depth-buffer collision sees only what is on screen (RayTrace on a CPU emitter, or DistanceField, collide off screen too)");
		if (bRayTrace && bGpu)
			Out += TEXT("; note: GPU ray traced collision needs hardware ray tracing (Project Settings > Rendering > Support Hardware Ray Tracing)");
		else if (bRayTrace)
			Out += TEXT("; note: CPU ray traces hit collision geometry (props need collision enabled; the trace channel is the module's CPU Collision Trace Channel input)");
		return Out;
	}
}

UNiagaraSystem* UUnrealAINiagaraRqlApi::CreateSystem(const FString& Path, const FString& Template)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	const FString Disabled = UnrealAINiagaraTemplates::DisabledPluginMessage(Template);
	if (!Disabled.IsEmpty())
		return UnrealMcpRql::Failed<UNiagaraSystem*>(Disabled);
	UObject* Source = nullptr;
	if (!Template.IsEmpty())
	{
		Source = UnrealMcpRql::LoadObject(UNiagaraSystem::StaticClass(), Template);
		if (!Source)
			Source = UnrealMcpRql::LoadObject(UNiagaraEmitter::StaticClass(), Template);
		if (!Source)
			return UnrealMcpRql::Failed<UNiagaraSystem*>(FString::Printf(TEXT("no Niagara system or emitter at '%s' (niagaraTemplates | where Name contains \"Fire\" | select Name Path Kind)"), *Template));
	}
	if (UNiagaraSystem* SourceSystem = Cast<UNiagaraSystem>(Source))
	{
		// The Content Browser's "new system from template": a duplicate without the template's description.
		FString Folder, Name;
		if (!FPackageName::IsValidLongPackageName(Path) || !Path.Split(TEXT("/"), &Folder, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || Folder.IsEmpty())
			return UnrealMcpRql::Failed<UNiagaraSystem*>(FString::Printf(TEXT("'%s' is not an asset path such as /Game/VFX/NS_Fire"), *Path));
		if (FindPackage(nullptr, *Path) || FPackageName::DoesPackageExist(Path))
			return UnrealMcpRql::Failed<UNiagaraSystem*>(FString::Printf(TEXT("an asset already exists at '%s' (pick another path, or editor | call DeleteAsset(\"%s\"))"), *Path, *Path));
		SourceSystem->WaitForCompilationComplete(false, false);
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UNiagaraSystem* Created = Cast<UNiagaraSystem>(AssetTools.DuplicateAsset(Name, Folder, SourceSystem));
		if (!Created)
			return UnrealMcpRql::Failed<UNiagaraSystem*>(FString::Printf(TEXT("could not create '%s' from %s"), *Path, *SourceSystem->GetPathName()));
		Created->TemplateAssetDescription = FText();
		Created->Category = FText();
		Changed(*Created);
		return Created;
	}
	UNiagaraSystem* Created = Cast<UNiagaraSystem>(UnrealMcpRql::CreateAsset(UNiagaraSystem::StaticClass(), Path));
	if (!Created)
		return nullptr;
	if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Source))
	{
		if (AddEmitter(Created, Emitter, FString()).IsEmpty())
			return nullptr;
	}
	Changed(*Created);
	return Created;
}

FString UUnrealAINiagaraRqlApi::AddEmitter(UNiagaraSystem* System, UNiagaraEmitter* Emitter, const FString& Name)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	if (!System || !Emitter)
		return UnrealMcpRql::Failed<FString>(TEXT("AddEmitter needs a system and an emitter (a template path from niagaraTemplates)"));
	System->Modify();
	FSystemStack Stack(*System);
	if (!Stack.Check())
		return FString();
	const TSharedPtr<FNiagaraEmitterHandleViewModel> Added = Stack.ViewModel()->AddEmitter(*Emitter, Emitter->GetExposedVersion().VersionGuid);
	FNiagaraEmitterHandle* Handle = Added.IsValid() ? Added->GetEmitterHandle() : nullptr;
	if (!Handle)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not add %s to %s"), *Emitter->GetName(), *System->GetName()));
	if (!Name.IsEmpty())
		Added->SetName(FName(*Name));
	Changed(*System);
	return Handle->GetName().ToString();
}

void UUnrealAINiagaraRqlApi::RemoveEmitter(UNiagaraSystem* System, const FString& Emitter)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	FNiagaraEmitterHandle* Handle = System ? FindEmitter(*System, Emitter) : nullptr;
	if (!Handle)
		return;
	System->Modify();
	FSystemStack Stack(*System);
	if (!Stack.Check())
		return;
	Stack.ViewModel()->DeleteEmitters({ Handle->GetId() });
	Changed(*System);
}

FString UUnrealAINiagaraRqlApi::RenameEmitter(UNiagaraSystem* System, const FString& Emitter, const FString& NewName)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	FNiagaraEmitterHandle* Handle = System ? FindEmitter(*System, Emitter) : nullptr;
	if (!Handle)
		return FString();
	if (NewName.TrimStartAndEnd().IsEmpty())
		return UnrealMcpRql::Failed<FString>(TEXT("the new emitter name is empty"));
	System->Modify();
	FSystemStack Stack(*System);
	const TSharedPtr<FNiagaraEmitterHandleViewModel> HandleViewModel = Stack.Check() ? Stack.ViewModel()->GetEmitterHandleViewModelById(Handle->GetId()) : nullptr;
	if (!HandleViewModel.IsValid())
		return Stack.Check() ? UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("emitter '%s' cannot be renamed"), *Emitter)) : FString();
	// As the stack's rename does: the emitter's parameters in the system graph follow the new name.
	HandleViewModel->SetName(FName(*NewName.TrimStartAndEnd()));
	Changed(*System);
	return HandleViewModel->GetName().ToString();
}

void UUnrealAINiagaraRqlApi::SetEmitterEnabled(UNiagaraSystem* System, const FString& Emitter, bool bEnabled)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	FNiagaraEmitterHandle* Handle = System ? FindEmitter(*System, Emitter) : nullptr;
	if (!Handle)
		return;
	System->Modify();
	FSystemStack Stack(*System);
	if (!Stack.Check())
		return;
	if (const TSharedPtr<FNiagaraEmitterHandleViewModel> HandleViewModel = Stack.ViewModel()->GetEmitterHandleViewModelById(Handle->GetId()))
		HandleViewModel->SetIsEnabled(bEnabled, /*bRequestRecompile*/ false);
	Changed(*System);
}

FString UUnrealAINiagaraRqlApi::SetEmitterProperty(UNiagaraSystem* System, const FString& Emitter, const FString& Property, const FString& Value)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	FNiagaraEmitterHandle* Handle = System ? FindEmitter(*System, Emitter) : nullptr;
	FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
	const FVersionedNiagaraEmitter Instance = Handle ? Handle->GetInstance() : FVersionedNiagaraEmitter();
	if (!Data || !Instance.Emitter)
		return Handle ? UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("emitter '%s' has no data"), *Emitter)) : FString();
	const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.VersionedNiagaraEmitterData"));
	FProperty* Found = nullptr;
	TArray<FString> Names;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit))
			continue;
		Names.Add(It->GetName());
		if (Normalize(It->GetName()) == Normalize(Property) || Normalize(It->GetDisplayNameText().ToString()) == Normalize(Property)
			|| (It->GetName().StartsWith(TEXT("b")) && Normalize(It->GetName().Mid(1)) == Normalize(Property)))
			Found = *It;
	}
	if (!Found)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("emitters have no property '%s' (properties: %s)"), *Property, *FString::Join(Names, TEXT(", "))));
	FString Text = Value.TrimStartAndEnd();
	if (Found->GetName() == TEXT("SimTarget"))
	{
		// The stack shows "CPU" / "GPU"; the enum entries are CPUSim / GPUComputeSim.
		if (Normalize(Text) == TEXT("gpu") || Normalize(Text) == TEXT("gpucompute"))
			Text = TEXT("GPUComputeSim");
		else if (Normalize(Text) == TEXT("cpu"))
			Text = TEXT("CPUSim");
	}
	void* Address = Found->ContainerPtrToValuePtr<void>(Data);
	// Import into a scratch value first: text that does not fully import changes nothing.
	void* Scratch = FMemory::Malloc(Found->GetSize(), Found->GetMinAlignment());
	Found->InitializeValue(Scratch);
	ON_SCOPE_EXIT { Found->DestroyValue(Scratch); FMemory::Free(Scratch); };
	FStringOutputDevice Errors;
	const TCHAR* End = Found->ImportText_Direct(*Text, Scratch, Instance.Emitter, PPF_None, &Errors);
	if (!End || !FString(End).TrimStartAndEnd().IsEmpty())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a value of %s"), *Value, *Found->GetName()) + (Errors.IsEmpty() ? FString() : TEXT(": ") + Errors));
	Instance.Emitter->Modify();
	Found->CopyCompleteValue(Address, Scratch);
	FPropertyChangedEvent Event(Found, EPropertyChangeType::ValueSet);
	Instance.Emitter->PostEditChangeVersionedProperty(Event, Instance.Version);
	Changed(*System);
	FString New;
	Found->ExportTextItem_Direct(New, Address, nullptr, Instance.Emitter, PPF_None);
	return New;
}

FString UUnrealAINiagaraRqlApi::AddModule(UNiagaraSystem* System, const FString& Emitter, const FString& Stage, const FString& Module, int32 Index)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	if (!System)
		return UnrealMcpRql::Failed<FString>(TEXT("AddModule needs a system"));
	UNiagaraScript* Script = ResolveModuleScript(Module);
	if (!Script)
		return FString();
	if (Script->GetUsage() != ENiagaraScriptUsage::Module)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s is not a module script (a dynamic input or function: set it on an input instead)"), *Script->GetPathName()));
	FSystemStack Stack(*System);
	UNiagaraStackScriptItemGroup* Group = Stack.FindStage(IsSystemStack(Emitter) ? FString() : Emitter, Stage);
	UNiagaraNodeOutput* Output = Group ? Group->GetScriptOutputNode() : nullptr;
	if (!Output)
		return Group ? UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("stage '%s' takes no modules"), *Stage)) : FString();
	System->Modify();
	UNiagaraNodeFunctionCall* Node = FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *Output, Index);
	if (!Node)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not add %s to %s"), *Script->GetName(), *Stage));
	Changed(*System);
	for (const FModuleRef& Added : Stack.Modules(IsSystemStack(Emitter) ? FString() : Emitter))
	{
		if (&Added.Item->GetModuleNode() == Node)
			return Added.Stage + TEXT("/") + Added.Name;
	}
	return Group->GetDisplayName().ToString() + TEXT("/") + Node->GetFunctionName();
}

void UUnrealAINiagaraRqlApi::RemoveModule(UNiagaraSystem* System, const FString& Emitter, const FString& Module)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	if (!System)
		return UnrealMcpRql::Fail(TEXT("RemoveModule needs a system"));
	FSystemStack Stack(*System);
	FModuleRef Found;
	if (!Stack.FindModule(IsSystemStack(Emitter) ? FString() : Emitter, Module, Found))
		return;
	FText Why;
	if (!Found.Item->TestCanDeleteWithMessage(Why))
		return UnrealMcpRql::Fail(FString::Printf(TEXT("%s/%s cannot be removed: %s (disable it with SetModuleEnabled)"), *Found.Stage, *Found.Name, *Why.ToString()));
	System->Modify();
	Found.Item->Delete();
	Changed(*System);
}

void UUnrealAINiagaraRqlApi::SetModuleEnabled(UNiagaraSystem* System, const FString& Emitter, const FString& Module, bool bEnabled)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	if (!System)
		return UnrealMcpRql::Fail(TEXT("SetModuleEnabled needs a system"));
	FSystemStack Stack(*System);
	FModuleRef Found;
	if (!Stack.FindModule(IsSystemStack(Emitter) ? FString() : Emitter, Module, Found))
		return;
	System->Modify();
	Found.Item->SetIsEnabled(bEnabled);
	Changed(*System);
}

FString UUnrealAINiagaraRqlApi::SetModuleInput(UNiagaraSystem* System, const FString& Emitter, const FString& Module, const FString& Input, const FString& Value)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	if (!System)
		return UnrealMcpRql::Failed<FString>(TEXT("SetModuleInput needs a system"));
	System->Modify();
	FSystemStack Stack(*System);
	const FString New = Stack.SetInput(IsSystemStack(Emitter) ? FString() : Emitter, Module, Input, Value);
	if (!New.IsEmpty())
		Changed(*System);
	return New;
}

FString UUnrealAINiagaraRqlApi::SetUserParameter(UNiagaraSystem* System, const FString& Name, const FString& Value, const FString& TypeName)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	if (!System)
		return UnrealMcpRql::Failed<FString>(TEXT("SetUserParameter needs a system"));
	const FString Short = Name.StartsWith(TEXT("User.")) ? Name.Mid(5) : Name;
	if (Short.TrimStartAndEnd().IsEmpty())
		return UnrealMcpRql::Failed<FString>(TEXT("the user parameter name is empty"));
	FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
	FNiagaraVariable Variable;
	bool bNew = false;
	for (const FNiagaraVariableWithOffset& Existing : Store.ReadParameterVariables())
	{
		FString ExistingName = Existing.GetName().ToString();
		ExistingName.RemoveFromStart(TEXT("User."));
		if (Normalize(ExistingName) == Normalize(Short))
			Variable = FNiagaraVariable(Existing);
	}
	const FString Text = Value.TrimStartAndEnd();
	if (!Variable.IsValid())
	{
		FNiagaraTypeDefinition Type;
		const FString Wanted = Normalize(TypeName);
		TArray<FString> Parts;
		Text.Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT("")).ParseIntoArray(Parts, TEXT(","));
		if (Wanted.IsEmpty())
		{
			// From the value: true/false, "#rrggbb" or four numbers = colour, 3 = vector, 2 = vector2, a whole number = int.
			const FString Lower = Text.ToLower();
			Type = Lower == TEXT("true") || Lower == TEXT("false") ? FNiagaraTypeDefinition::GetBoolDef()
				: Text.StartsWith(TEXT("#")) || Parts.Num() == 4 ? FNiagaraTypeDefinition::GetColorDef()
				: Parts.Num() == 3 ? FNiagaraTypeDefinition::GetVec3Def()
				: Parts.Num() == 2 ? FNiagaraTypeDefinition::GetVec2Def()
				: Text.IsNumeric() && !Text.Contains(TEXT(".")) ? FNiagaraTypeDefinition::GetIntDef()
				: Text.IsNumeric() ? FNiagaraTypeDefinition::GetFloatDef() : FNiagaraTypeDefinition();
			if (!Type.IsValid())
				return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("cannot tell the type of '%s': pass Type (float, int, bool, Vector, Vector2, Vector4, LinearColor, Position, or an object class)"), *Text));
		}
		else
		{
			const TPair<const TCHAR*, FNiagaraTypeDefinition> Known[] = {
				{ TEXT("float"), FNiagaraTypeDefinition::GetFloatDef() }, { TEXT("int"), FNiagaraTypeDefinition::GetIntDef() },
				{ TEXT("int32"), FNiagaraTypeDefinition::GetIntDef() }, { TEXT("bool"), FNiagaraTypeDefinition::GetBoolDef() },
				{ TEXT("vector"), FNiagaraTypeDefinition::GetVec3Def() }, { TEXT("vector3"), FNiagaraTypeDefinition::GetVec3Def() },
				{ TEXT("vector2"), FNiagaraTypeDefinition::GetVec2Def() }, { TEXT("vector2d"), FNiagaraTypeDefinition::GetVec2Def() },
				{ TEXT("vector4"), FNiagaraTypeDefinition::GetVec4Def() }, { TEXT("linearcolor"), FNiagaraTypeDefinition::GetColorDef() },
				{ TEXT("color"), FNiagaraTypeDefinition::GetColorDef() }, { TEXT("position"), FNiagaraTypeDefinition::GetPositionDef() },
			};
			for (const TPair<const TCHAR*, FNiagaraTypeDefinition>& Entry : Known)
			{
				if (Wanted == Entry.Key)
					Type = Entry.Value;
			}
			if (!Type.IsValid())
			{
				UClass* Class = UnrealMcpRql::ResolveClass(TypeName);
				if (!Class)
					return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("unknown type '%s' (float, int, bool, Vector, Vector2, Vector4, LinearColor, Position, or an object class such as StaticMesh)"), *TypeName));
				Type = FNiagaraTypeDefinition(Class);
			}
		}
		Variable = FNiagaraVariable(Type, FName(*(TEXT("User.") + Short)));
		bNew = true;
	}

	const FNiagaraTypeDefinition& Type = Variable.GetType();
	if (Type.IsDataInterface())
	{
		// A data interface parameter (a static mesh to emit from, a curve, ...): its properties, or the asset it takes.
		UNiagaraDataInterface* Data = Store.GetDataInterface(Variable);
		FString Error;
		if (!Data)
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("User.%s has no data interface object"), *Short));
		System->Modify();
		if (!WriteDataInterface(*Data, Text, Error))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("User.%s: %s"), *Short, *Error));
		FNiagaraSystemUpdateContext Update(System, true);
		System->MarkPackageDirty();
		return FormatDataInterface(*Data);
	}
	// Read the value first: a bad one adds no parameter and changes nothing.
	UObject* Asset = nullptr;
	TArray<uint8> Buffer;
	if (Type.IsUObject())
	{
		Asset = Normalize(Text) == TEXT("none") ? nullptr : UnrealMcpRql::LoadObject(Type.GetClass(), Text);
		if (!Asset && Normalize(Text) != TEXT("none"))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("User.%s takes a %s asset path; '%s' is none"), *Short, *Type.GetClass()->GetName(), *Text));
	}
	else
	{
		Buffer.SetNumZeroed(Type.GetSize());
		FString Error;
		if (!ParseValue(Type, Text, Buffer.GetData(), Error))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("User.%s: %s"), *Short, *Error));
	}
	System->Modify();
	if (bNew)
		Store.AddParameter(Variable, true, true);
	if (Type.IsUObject())
		Store.SetUObject(Asset, Variable);
	else
		Store.SetParameterData(Buffer.GetData(), Variable, true);
	{
		// Placed effects restart with the new default (as an edit in the User Parameters panel does).
		FNiagaraSystemUpdateContext Update(System, true);
	}
	System->MarkPackageDirty();
	if (Type.IsUObject())
	{
		const UObject* Stored = Store.GetUObject(Variable);
		return Stored ? Stored->GetPathName() : FString(TEXT("None"));
	}
	const uint8* Data = Store.GetParameterData(Variable);
	return Data ? FormatValue(Type, Data) : FString();
}

FString UUnrealAINiagaraRqlApi::EnableCollision(UNiagaraSystem* System, const FString& Emitter, const FString& Mode)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	if (!System)
		return UnrealMcpRql::Failed<FString>(TEXT("EnableCollision needs a system"));
	TArray<FString> Names;
	for (FNiagaraEmitterHandle* Handle : Targets(*System, Emitter))
		Names.Add(Handle->GetName().ToString());
	if (Names.Num() == 0)
		return FString();
	System->Modify();
	// Emitters done before a failure keep their changes (a call is not rolled back): mark them on every exit.
	ON_SCOPE_EXIT { Changed(*System); };
	TArray<FString> Report;
	for (const FString& Name : Names)
	{
		FSystemStack Stack(*System);
		if (!Stack.Check())
			return FString();
		FNiagaraEmitterHandle* Handle = FindEmitter(*System, Name);
		if (!Handle)
			return FString();
		const FString Line = IsFluid(Stack, Name) ? FluidCollision(Stack, Name, Mode) : ParticleCollision(*this, *System, Stack, *Handle, Mode);
		if (Line.IsEmpty())
			return FString();
		Report.Add(Line);
	}
	return FString::Join(Report, TEXT(" | "));
}

int32 UUnrealAINiagaraRqlApi::SetMaterial(UNiagaraSystem* System, const FString& Emitter, UMaterialInterface* Material, int32 Renderer)
{
	using namespace UnrealAINiagaraRqlApiLocal;
	FNiagaraEmitterHandle* Handle = System ? FindEmitter(*System, Emitter) : nullptr;
	FVersionedNiagaraEmitterData* Data = Handle ? Handle->GetEmitterData() : nullptr;
	if (!Data)
		return 0;
	if (!Material)
		return UnrealMcpRql::Failed<int32>(TEXT("SetMaterial needs a material (an asset path)"));
	const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
	if (Renderer < -1 || Renderer >= Renderers.Num())
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("emitter '%s' has %d renderer(s): Renderer is 0 to %d, or -1 for all"), *Emitter, Renderers.Num(), Renderers.Num() - 1));
	int32 Count = 0;
	TArray<FString> Kinds;
	for (int32 Index = 0; Index < Renderers.Num(); ++Index)
	{
		UNiagaraRendererProperties* Properties = Renderers[Index];
		if (!Properties || (Renderer >= 0 && Index != Renderer))
			continue;
		Kinds.Add(Properties->GetClass()->GetName());
		FProperty* Edited = nullptr;
		if (UNiagaraMeshRendererProperties* Mesh = Cast<UNiagaraMeshRendererProperties>(Properties))
		{
			Mesh->Modify();
			Mesh->bOverrideMaterials = true;
			int32 Slots = FMath::Max(1, Mesh->OverrideMaterials.Num());
			for (const FNiagaraMeshRendererMeshProperties& Entry : Mesh->Meshes)
				Slots = FMath::Max(Slots, Entry.Mesh ? Entry.Mesh->GetStaticMaterials().Num() : 0);
			Mesh->OverrideMaterials.SetNum(Slots);
			for (FNiagaraMeshMaterialOverride& Override : Mesh->OverrideMaterials)
				Override.ExplicitMat = Material;
			Edited = FindFProperty<FProperty>(Mesh->GetClass(), TEXT("OverrideMaterials"));
		}
		else if (FObjectPropertyBase* Slot = CastField<FObjectPropertyBase>(Properties->GetClass()->FindPropertyByName(TEXT("Material"))))
		{
			if (!Material->IsA(Slot->PropertyClass))
				continue;
			Properties->Modify();
			Slot->SetObjectPropertyValue_InContainer(Properties, Material);
			Edited = Slot;
		}
		if (!Edited)
			continue;
		FPropertyChangedEvent Event(Edited, EPropertyChangeType::ValueSet);
		Properties->PostEditChangeProperty(Event);
		++Count;
	}
	if (Count == 0)
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("no renderer of emitter '%s' takes a material (renderers: %s)"), *Emitter,
			Kinds.Num() ? *FString::Join(Kinds, TEXT(", ")) : TEXT("none")));
	Changed(*System);
	return Count;
}

FUnrealAINiagaraCompileResult UUnrealAINiagaraRqlApi::Compile(UNiagaraSystem* System)
{
	FUnrealAINiagaraCompileResult Result;
	if (!System)
		return UnrealMcpRql::Failed<FUnrealAINiagaraCompileResult>(TEXT("Compile needs a system"));
	System->RequestCompile(false);
	System->WaitForCompilationComplete(/*bIncludingGPUShaders*/ false, /*bShowProgress*/ false);
	const auto Collect = [&Result](const UNiagaraScript* Script, const FString& Owner)
	{
		if (!Script)
			return;
		const FString Where = FString::Printf(TEXT("%s %s"), *Owner, *StaticEnum<ENiagaraScriptUsage>()->GetDisplayNameTextByValue(static_cast<int64>(Script->GetUsage())).ToString());
		for (const FNiagaraCompileEvent& Event : Script->GetVMExecutableData().LastCompileEvents)
		{
			if (Event.Severity == FNiagaraCompileEventSeverity::Error)
				Result.Errors.AddUnique(FString::Printf(TEXT("%s: %s"), *Where, *Event.Message));
			else if (Event.Severity == FNiagaraCompileEventSeverity::Warning)
				Result.Warnings.AddUnique(FString::Printf(TEXT("%s: %s"), *Where, *Event.Message));
		}
		if (Script->GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_Error && Result.Errors.Num() == 0)
			Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Where, Script->GetVMExecutableData().ErrorMsg.IsEmpty() ? TEXT("failed to compile") : *Script->GetVMExecutableData().ErrorMsg));
	};
	Collect(System->GetSystemSpawnScript(), TEXT("System"));
	Collect(System->GetSystemUpdateScript(), TEXT("System"));
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetIsEnabled() ? Handle.GetEmitterData() : nullptr;
		if (!Data)
			continue;
		TArray<UNiagaraScript*> Scripts;
		Data->GetScripts(Scripts, /*bCompilableOnly*/ true, /*bEnabledOnly*/ true);
		for (const UNiagaraScript* Script : Scripts)
			Collect(Script, Handle.GetName().ToString());
	}
	Result.Succeeded = Result.Errors.Num() == 0;
	return Result;
}

AActor* UUnrealAINiagaraRqlApi::SpawnSystem(UNiagaraSystem* System, FVector Location, const FString& Label, bool bAutoActivate)
{
	// The core spawn (current level, transactional, labelled): Ctrl+Z removes it with the rest of the mutation.
	AActor* Actor = UnrealMcpRql::SpawnActor(ANiagaraActor::StaticClass(), Location, FRotator::ZeroRotator, Label);
	if (!Actor)
		return nullptr;
	UNiagaraComponent* Component = Actor->FindComponentByClass<UNiagaraComponent>();
	if (!Component)
	{
		Actor->GetWorld()->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true);
		return UnrealMcpRql::Failed<AActor*>(FString::Printf(TEXT("could not place %s"), *System->GetName()));
	}
	// Before the asset: a placed component starts with it when bAutoActivate (SetAutoActivate after construction warns).
	Component->bAutoActivate = bAutoActivate;
	Component->SetAsset(System);
	return Actor;
}

int32 UUnrealAINiagaraRqlApi::Activate(AActor* Actor, bool bReset)
{
	if (!Actor)
		return UnrealMcpRql::Failed<int32>(TEXT("Activate needs an actor (its label, e.g. unreal.find(\"Campfire\"))"));
	TArray<UNiagaraComponent*> Components;
	Actor->GetComponents(Components);
	for (UNiagaraComponent* Component : Components)
	{
		if (bReset)
			Component->ReinitializeSystem();
		Component->Activate(bReset);
	}
	return Components.Num() ? Components.Num() : UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("%s has no Niagara component"), *Actor->GetActorNameOrLabel()));
}

int32 UUnrealAINiagaraRqlApi::Deactivate(AActor* Actor)
{
	if (!Actor)
		return UnrealMcpRql::Failed<int32>(TEXT("Deactivate needs an actor (its label, e.g. unreal.find(\"Campfire\"))"));
	TArray<UNiagaraComponent*> Components;
	Actor->GetComponents(Components);
	for (UNiagaraComponent* Component : Components)
		Component->Deactivate();
	return Components.Num();
}

FString UUnrealAINiagaraRqlApi::EnableFluids()
{
	if (UnrealAINiagaraTemplates::IsFluidsEnabled())
		return TEXT("NiagaraFluids is already enabled: niagaraTemplates | where Plugin == \"NiagaraFluids\" lists its templates");
	if (!UnrealAINiagaraTemplates::IsFluidsInstalled())
		return UnrealMcpRql::Failed<FString>(UnrealAINiagaraTemplates::DisabledPluginMessage(TEXT("/NiagaraFluids/")));
	FText Reason;
	// The Fluids modules use ChaosNiagara's data interfaces (geometry collection collisions) without declaring it.
	if (!IProjectManager::Get().SetPluginEnabled(TEXT("ChaosNiagara"), true, Reason) || !IProjectManager::Get().SetPluginEnabled(TEXT("NiagaraFluids"), true, Reason)
		|| !IProjectManager::Get().SaveCurrentProjectToDisk(Reason))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not enable NiagaraFluids: %s"), *Reason.ToString()));
	return TEXT("NiagaraFluids (and ChaosNiagara, which its modules use) is enabled in the project file; restart the editor to load it (save your work first: editor | call SaveAll()), then niagaraTemplates | where Plugin == \"NiagaraFluids\"");
}
