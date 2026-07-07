// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"
#include "Tools/UnrealMcpObjectRef.h"   // the de-facto standard class resolver (FUnrealMcpObjectRef::ResolveClass)

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_Event.h"

#include "Editor.h"
#include "Engine/World.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#if __has_include("Misc/StringOutputDevice.h")
#include "Misc/StringOutputDevice.h"
#else
#include "Containers/UnrealString.h"
#endif

/**
 * The Blueprint tool family (docs/ARCHITECTURE.md §10). MVP floor — read + structure-edit + compile +
 * spawn (11 tools). All edits go through the PUBLIC FKismetEditorUtilities / FBlueprintEditorUtils
 * surface (plus the public SCS surface for components, which those two utilities do not expose). Every
 * Handle() runs ON the game thread (the §4 dispatcher guarantees it), so direct UObject/editor calls
 * are safe; no modal UI, no bridge waits.
 */
namespace
{
	// ---- Resolution helpers ----------------------------------------------------------------------
	//
	// Class resolution uses the shared FUnrealMcpObjectRef::ResolveClass (Tools/UnrealMcpObjectRef.h) — the
	// de-facto standard the actor/asset/level families already use. It is a strict superset of the resolver this
	// file once carried locally: it ADDS an FSoftClassPath::TryLoadClass step (resolves native + generated
	// Blueprint class paths without a scan) and a FindFirstObject fallback on top of the
	// TryFindTypeSlow + object/Blueprint-asset load chain. Broadening the Blueprint family's resolution to match
	// is the intended convergence.

	/** Resolve a UBlueprint by object path. Prefers an already-in-memory object (assets created earlier
	 *  this session are registered with the AssetRegistry but not saved to disk), then falls back to load. */
	UBlueprint* ResolveBlueprint(const FString& Path)
	{
		if (Path.IsEmpty())
			return nullptr;
		if (UBlueprint* Found = FindObject<UBlueprint>(nullptr, *Path))
			return Found;
		// LOAD_NoWarn|LOAD_Quiet: a missing asset is an expected outcome here (the caller turns it into a
		// structured "not found" error); don't let the speculative load warn-spam the editor log.
		return LoadObject<UBlueprint>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** Reject names that are empty, too long, or contain invalid characters, using the same validator the
	 *  Blueprint editor applies. Name COLLISIONS are intentionally NOT rejected here — each caller owns a
	 *  collision check with a tool-specific message — so the in-use results pass through as well-formed. */
	bool IsNameWellFormed(UBlueprint* Blueprint, const FString& Name, FString& OutError)
	{
		const EValidatorResult Result = FKismetNameValidator(Blueprint).IsValid(Name);
		if (Result == EValidatorResult::Ok || Result == EValidatorResult::AlreadyInUse
			|| Result == EValidatorResult::ExistingName || Result == EValidatorResult::LocallyInUse)
			return true;
		OutError = FString::Printf(TEXT("invalid name '%s': %s"), *Name,
			*INameValidatorInterface::GetErrorString(Name, Result));
		return false;
	}

	// ---- §3.2 pin-type mapping (forward: type string -> FEdGraphPinType) --------------------------

	/** Map a friendly type token to a Blueprint pin type (§3.2). Recognised primitives plus the common
	 *  math structs; anything else is treated as an object/class/struct path. Returns false when the
	 *  token names an object/struct path that cannot be resolved. `bIsArray` wraps it in an array. */
	bool MakePinType(const FString& InType, bool bIsArray, FEdGraphPinType& OutPinType, FString& OutError)
	{
		const FString Type = InType.TrimStartAndEnd();
		const FString Lower = Type.ToLower();

		auto Set = [&OutPinType](FName Category, FName SubCategory, UObject* SubObject)
		{
			OutPinType = FEdGraphPinType();
			OutPinType.PinCategory = Category;
			OutPinType.PinSubCategory = SubCategory;
			OutPinType.PinSubCategoryObject = SubObject;
		};

		if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
			Set(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr);
		else if (Lower == TEXT("int") || Lower == TEXT("integer") || Lower == TEXT("int32"))
			Set(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr);
		else if (Lower == TEXT("int64"))
			Set(UEdGraphSchema_K2::PC_Int64, NAME_None, nullptr);
		else if (Lower == TEXT("byte"))
			Set(UEdGraphSchema_K2::PC_Byte, NAME_None, nullptr);
		else if (Lower == TEXT("float") || Lower == TEXT("double") || Lower == TEXT("number") || Lower == TEXT("real"))
			Set(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr);
		else if (Lower == TEXT("string"))
			Set(UEdGraphSchema_K2::PC_String, NAME_None, nullptr);
		else if (Lower == TEXT("name"))
			Set(UEdGraphSchema_K2::PC_Name, NAME_None, nullptr);
		else if (Lower == TEXT("text"))
			Set(UEdGraphSchema_K2::PC_Text, NAME_None, nullptr);
		else if (Lower == TEXT("vector"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector>::Get());
		else if (Lower == TEXT("vector2d"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector2D>::Get());
		else if (Lower == TEXT("rotator"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FRotator>::Get());
		else if (Lower == TEXT("transform"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FTransform>::Get());
		else if (Lower == TEXT("color") || Lower == TEXT("linearcolor"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get());
		else
		{
			// Object / class / struct path: resolve to a UClass (object ref) or a UScriptStruct.
			if (UClass* Class = FUnrealMcpObjectRef::ResolveClass(Type))
				Set(UEdGraphSchema_K2::PC_Object, NAME_None, Class);
			else if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Type, nullptr, LOAD_NoWarn | LOAD_Quiet))
				Set(UEdGraphSchema_K2::PC_Struct, NAME_None, Struct);
			else
			{
				OutError = FString::Printf(TEXT("unknown variable type '%s' (expected a primitive, a math struct, or a resolvable object/struct path)"), *Type);
				return false;
			}
		}

		OutPinType.ContainerType = bIsArray ? EPinContainerType::Array : EPinContainerType::None;
		return true;
	}

	// ---- §3.2 pin-type mapping (reverse: FEdGraphPinType -> friendly string) for blueprint-get -----

	FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		const FName Cat = PinType.PinCategory;
		FString Base;
		if (Cat == UEdGraphSchema_K2::PC_Boolean) Base = TEXT("bool");
		else if (Cat == UEdGraphSchema_K2::PC_Int) Base = TEXT("int");
		else if (Cat == UEdGraphSchema_K2::PC_Int64) Base = TEXT("int64");
		else if (Cat == UEdGraphSchema_K2::PC_Byte) Base = TEXT("byte");
		else if (Cat == UEdGraphSchema_K2::PC_Real) Base = TEXT("float"); // float/double both map to PC_Real (PC_Float/PC_Double are legacy-unreachable here)
		else if (Cat == UEdGraphSchema_K2::PC_String) Base = TEXT("string");
		else if (Cat == UEdGraphSchema_K2::PC_Name) Base = TEXT("name");
		else if (Cat == UEdGraphSchema_K2::PC_Text) Base = TEXT("text");
		else if (Cat == UEdGraphSchema_K2::PC_Struct)
			Base = PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetName() : TEXT("struct");
		else if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Class)
			Base = PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetName() : TEXT("object");
		else
			Base = Cat.ToString();

		if (PinType.ContainerType == EPinContainerType::Array)
			return Base + TEXT("[]");
		if (PinType.ContainerType == EPinContainerType::Set)
			return Base + TEXT("{}");
		if (PinType.ContainerType == EPinContainerType::Map)
			return Base + TEXT("<map>");
		return Base;
	}

	// ---- Result helpers --------------------------------------------------------------------------

	TSharedPtr<FJsonObject> BlueprintRefStruct(UBlueprint* Blueprint)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Blueprint->GetName());
		Obj->SetStringField(TEXT("path"), Blueprint->GetPathName());
		if (Blueprint->ParentClass)
			Obj->SetStringField(TEXT("parentClass"), Blueprint->ParentClass->GetPathName());
		return Obj;
	}
}

namespace UnrealMcpBlueprintTools
{
	// ---- blueprint-create ------------------------------------------------------------------------
	static void RegisterCreate(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-create"))
			.Title(TEXT("Create Blueprint"))
			.Description(TEXT("Create a new Blueprint class from a parent UClass path via the public "
			                  "FKismetEditorUtilities::CreateBlueprint. 'path' is the /Game package path for the "
			                  "asset (e.g. '/Game/MCP/BP_Thing'); the object-path form ('/Game/MCP/BP_Thing.BP_Thing') "
			                  "is also accepted and normalised. 'parentClass' is a native class path or short name "
			                  "(e.g. '/Script/Engine.Actor' or 'Actor'). The asset is registered in-session; "
			                  "saving to disk is out of scope for the MVP."))
			.ParamString(TEXT("path"), TEXT("/Game package path for the new Blueprint asset (object-path form also accepted)."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("parentClass"), TEXT("Parent UClass path or short name. Defaults to Actor."))
			.DestructiveHint(false).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				const FString ParentPath = Call.GetString(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
				if (Path.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));

				UClass* ParentClass = FUnrealMcpObjectRef::ResolveClass(ParentPath);
				if (!ParentClass)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("could not resolve parent class '%s'."), *ParentPath));
				if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' cannot be used as a Blueprint parent class."), *ParentPath));

				// Accept either a package path ("/Game/MCP/BP_Thing") or the object-path form
				// ("/Game/MCP/BP_Thing.BP_Thing"); normalise to the package path before validating.
				FString PackageName = Path;
				int32 DotIndex = INDEX_NONE;
				if (Path.FindChar(TEXT('.'), DotIndex))
					PackageName = Path.Left(DotIndex);

				FText PackageError;
				if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ false, &PackageError))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("invalid Blueprint package path '%s': %s"), *PackageName, *PackageError.ToString()));

				const FString AssetName = FPackageName::GetShortName(PackageName);
				if (AssetName.IsEmpty())
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("invalid Blueprint path '%s'."), *Path));

				const FString ObjectPath = PackageName + TEXT(".") + AssetName;
				// Probe for ANY UObject, not just UBlueprint: a non-Blueprint asset (e.g. an on-disk UStaticMesh)
				// at this path slips past a UBlueprint-scoped probe (class mismatch -> null), CreatePackage then
				// returns the existing package, and CreateBlueprint -> NewObject<UBlueprint> hits the engine's
				// fatal "same fully qualified name, different class" allocation check instead of a structured error.
				// LOAD_NoWarn|LOAD_Quiet: this is the expected-miss path (we WANT no asset here); without it the
				// probe warn-spams the editor log on every successful create.
				if (FindObject<UObject>(nullptr, *ObjectPath) != nullptr
					|| LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet) != nullptr)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("an asset already exists at '%s'."), *ObjectPath));

				UPackage* Package = CreatePackage(*PackageName);
				if (!Package)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("could not create package '%s'."), *PackageName));

				UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
					ParentClass, Package, FName(*AssetName), BPTYPE_Normal,
					UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("UnrealMcpBlueprintCreate")));
				if (!Blueprint)
				{
					// CreateBlueprint failed after the package was minted; drop the now-empty package so the
					// path isn't left half-claimed in memory (FindObject<UBlueprint> won't match it, so retry
					// would otherwise be blocked only by the stale package) for the rest of the session.
					Package->MarkAsGarbage();
					return FUnrealMcpToolResult::Error(TEXT("FKismetEditorUtilities::CreateBlueprint returned null."));
				}

				FAssetRegistryModule::AssetCreated(Blueprint);
				Package->MarkPackageDirty();

				UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] blueprint-create: '%s' (parent %s)."), *Blueprint->GetPathName(), *ParentClass->GetName());
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Created Blueprint '%s' from parent '%s'."), *Blueprint->GetPathName(), *ParentClass->GetName()),
					BlueprintRefStruct(Blueprint));
			});
	}

	// ---- blueprint-get ---------------------------------------------------------------------------
	static void RegisterGet(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-get"))
			.Title(TEXT("Get Blueprint Summary"))
			.Description(TEXT("Return a scoped graph summary of a Blueprint for LLM inspection: member variables "
			                  "(name/type/array), SCS components (name/class/parent), functions and events with "
			                  "node-counts, implemented interfaces, and the parent class chain."))
			.ParamString(TEXT("path"), TEXT("Full object path of the Blueprint asset."), EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true).IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("name"), Blueprint->GetName());
				Out->SetStringField(TEXT("path"), Blueprint->GetPathName());
				if (Blueprint->ParentClass)
					Out->SetStringField(TEXT("parentClass"), Blueprint->ParentClass->GetPathName());

				// Variables (NewVariables holds member vars added via the builder/UI; visible pre-compile).
				TArray<TSharedPtr<FJsonValue>> Variables;
				for (const FBPVariableDescription& Var : Blueprint->NewVariables)
				{
					TSharedPtr<FJsonObject> V = MakeShared<FJsonObject>();
					V->SetStringField(TEXT("name"), Var.VarName.ToString());
					V->SetStringField(TEXT("type"), PinTypeToString(Var.VarType));
					V->SetBoolField(TEXT("isArray"), Var.VarType.ContainerType == EPinContainerType::Array);
					if (!Var.DefaultValue.IsEmpty())
						V->SetStringField(TEXT("default"), Var.DefaultValue);
					Variables.Add(MakeShared<FJsonValueObject>(V));
				}
				Out->SetArrayField(TEXT("variables"), Variables);

				// SCS components.
				TArray<TSharedPtr<FJsonValue>> Components;
				if (Blueprint->SimpleConstructionScript)
				{
					for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
					{
						if (!Node)
							continue;
						TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
						C->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
						C->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT(""));
						// 'parent' is the SCS attach parent (empty for a root node); the LLM needs it to drive
						// blueprint-add-component's 'parentComponent'.
						if (USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(Node))
							C->SetStringField(TEXT("parent"), ParentNode->GetVariableName().ToString());
						else
							C->SetStringField(TEXT("parent"), TEXT(""));
						Components.Add(MakeShared<FJsonValueObject>(C));
					}
				}
				Out->SetArrayField(TEXT("components"), Components);

				// Functions (user function graphs) with node-counts.
				TArray<TSharedPtr<FJsonValue>> Functions;
				for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
				{
					if (!Graph)
						continue;
					TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
					F->SetStringField(TEXT("name"), Graph->GetName());
					F->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
					Functions.Add(MakeShared<FJsonValueObject>(F));
				}
				Out->SetArrayField(TEXT("functions"), Functions);

				// Events (UK2Node_Event nodes across the ubergraph pages).
				TArray<TSharedPtr<FJsonValue>> Events;
				for (const UEdGraph* Graph : Blueprint->UbergraphPages)
				{
					if (!Graph)
						continue;
					for (const UEdGraphNode* GraphNode : Graph->Nodes)
					{
						if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(GraphNode))
						{
							TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
							E->SetStringField(TEXT("name"), EventNode->EventReference.GetMemberName().ToString());
							// Fresh Actor Blueprints are pre-seeded with DISABLED ghost event nodes
							// (ReceiveBeginPlay/ReceiveTick/...). Emit 'enabled' so the read surface matches
							// blueprint-add-event's write semantics: a disabled ghost is inert (the event does NOT
							// fire) until add-event enables it. Without this flag an agent reads the ghost names and
							// wrongly concludes the events already run, skipping the add and shipping a dead BeginPlay.
							E->SetBoolField(TEXT("enabled"), EventNode->IsNodeEnabled());
							Events.Add(MakeShared<FJsonValueObject>(E));
						}
					}
				}
				Out->SetArrayField(TEXT("events"), Events);

				// Implemented interfaces.
				TArray<TSharedPtr<FJsonValue>> Interfaces;
				for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
				{
					if (Iface.Interface)
						Interfaces.Add(MakeShared<FJsonValueString>(Iface.Interface->GetPathName()));
				}
				Out->SetArrayField(TEXT("interfaces"), Interfaces);

				// Parent chain (parent class up to UObject).
				TArray<TSharedPtr<FJsonValue>> ParentChain;
				for (UClass* C = Blueprint->ParentClass; C; C = C->GetSuperClass())
					ParentChain.Add(MakeShared<FJsonValueString>(C->GetName()));
				Out->SetArrayField(TEXT("parentChain"), ParentChain);

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Blueprint '%s': %d variable(s), %d component(s), %d function(s), %d event(s)."),
						*Blueprint->GetName(), Variables.Num(), Components.Num(), Functions.Num(), Events.Num()),
					Out);
			});
	}

	// ---- blueprint-add-component / blueprint-remove-component (SCS) -------------------------------
	static void RegisterComponents(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-add-component"))
			.Title(TEXT("Add Blueprint Component"))
			.Description(TEXT("Add a component to a Blueprint's Simple Construction Script (the public SCS surface). "
			                  "'componentClass' is a UActorComponent subclass path or short name. Optionally attach "
			                  "under an existing scene-component node named by 'parentComponent'."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("componentClass"), TEXT("UActorComponent subclass path or short name."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Variable name for the new component."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("parentComponent"), TEXT("Optional existing scene-component node to attach under."))
			.DestructiveHint(false).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));
				if (!Blueprint->SimpleConstructionScript)
					return FUnrealMcpToolResult::Error(TEXT("Blueprint has no Simple Construction Script (not an Actor-based Blueprint?)."));

				const FString CompName = Call.GetString(TEXT("name"));
				UClass* CompClass = FUnrealMcpObjectRef::ResolveClass(Call.GetString(TEXT("componentClass")));
				if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a UActorComponent subclass."), *Call.GetString(TEXT("componentClass"))));
				// SCS CreateNode calls NewObject on the class; an abstract/deprecated class fatally asserts there
				// (e.g. '/Script/Engine.LightComponentBase'). Reject up front with a structured error.
				if (CompClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is abstract/deprecated and cannot be instantiated as a component."), *CompClass->GetName()));
				if (CompName.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("'name' is required."));
				FString NameError;
				if (!IsNameWellFormed(Blueprint, CompName, NameError))
					return FUnrealMcpToolResult::Error(NameError);
				if (Blueprint->SimpleConstructionScript->FindSCSNode(FName(*CompName)) != nullptr)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("a component named '%s' already exists."), *CompName));
				// SCS component names, member-variable names, and inherited parent properties all share the
				// generated class's property namespace, so a component named after an existing variable or an
				// inherited property collides — but only fails later at blueprint-compile. Reject up front with
				// the same structured-error style, since FindSCSNode alone covers only the component namespace.
				if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*CompName)) != INDEX_NONE)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("a variable named '%s' already exists, so a component cannot reuse that name."), *CompName));
				if (Blueprint->ParentClass && FindFProperty<FProperty>(Blueprint->ParentClass, FName(*CompName)) != nullptr)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is already a property on parent class '%s', so a component cannot reuse that name."), *CompName, *Blueprint->ParentClass->GetName()));

				const FString ParentName = Call.GetString(TEXT("parentComponent"));
				USCS_Node* ParentNode = ParentName.IsEmpty() ? nullptr : Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ParentName));
				if (!ParentName.IsEmpty() && !ParentNode)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("parent component '%s' not found."), *ParentName));
				// Attachment is a scene-graph operation: both the new component and its parent must be
				// USceneComponents, otherwise AddChildNode produces an invalid (non-attachable) hierarchy.
				if (ParentNode)
				{
					if (!CompClass->IsChildOf(USceneComponent::StaticClass()))
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a USceneComponent, so it cannot be attached under a parent component."), *CompClass->GetName()));
					if (!ParentNode->ComponentClass || !ParentNode->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("parent component '%s' is not a USceneComponent and cannot host child components."), *ParentName));
				}

				USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(CompClass, FName(*CompName));
				if (!NewNode)
					return FUnrealMcpToolResult::Error(TEXT("USimpleConstructionScript::CreateNode returned null."));

				if (ParentNode)
					ParentNode->AddChildNode(NewNode);
				else
					Blueprint->SimpleConstructionScript->AddNode(NewNode);

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("component"), CompName);
				Out->SetStringField(TEXT("class"), CompClass->GetName());
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Added component '%s' (%s) to '%s'."), *CompName, *CompClass->GetName(), *Blueprint->GetName()), Out);
			});

		Registry.Tool(TEXT("blueprint-remove-component"))
			.Title(TEXT("Remove Blueprint Component"))
			.Description(TEXT("Remove a component node from a Blueprint's Simple Construction Script by variable name."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Variable name of the SCS component node to remove."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(true).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));
				if (!Blueprint->SimpleConstructionScript)
					return FUnrealMcpToolResult::Error(TEXT("Blueprint has no Simple Construction Script."));

				const FString CompName = Call.GetString(TEXT("name"));
				USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*CompName));
				if (!Node)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("component '%s' not found."), *CompName));

				Blueprint->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Removed component '%s' from '%s'."), *CompName, *Blueprint->GetName()));
			});
	}

	// ---- blueprint-add-variable / blueprint-modify-variable --------------------------------------
	static void RegisterVariables(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-add-variable"))
			.Title(TEXT("Add Blueprint Variable"))
			.Description(TEXT("Add a typed member variable via FBlueprintEditorUtils::AddMemberVariable using the "
			                  "§3.2 pin-type mapping. 'type' accepts a primitive (bool/int/int64/byte/float/string/"
			                  "name/text), a math struct (vector/vector2d/rotator/transform/color), or an object/"
			                  "struct path. Set 'isArray' for an array variable."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("New variable name."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("type"), TEXT("Variable type token (see description)."), EUnrealMcpParamRequirement::Required)
			.ParamBool(TEXT("isArray"), TEXT("Wrap the type in an array. Defaults to false."))
			.ParamString(TEXT("defaultValue"), TEXT("Optional default value (UE text format)."))
			.DestructiveHint(false).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				const FString VarName = Call.GetString(TEXT("name"));
				if (VarName.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("'name' is required."));
				FString NameError;
				if (!IsNameWellFormed(Blueprint, VarName, NameError))
					return FUnrealMcpToolResult::Error(NameError);
				if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VarName)) != INDEX_NONE)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("a variable named '%s' already exists."), *VarName));
				// Symmetric with blueprint-add-component: a variable named after an existing SCS component
				// collides in the shared generated-class property namespace and would only fail at compile.
				if (Blueprint->SimpleConstructionScript && Blueprint->SimpleConstructionScript->FindSCSNode(FName(*VarName)) != nullptr)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("a component named '%s' already exists, so a variable cannot reuse that name."), *VarName));

				FEdGraphPinType PinType;
				FString PinError;
				if (!MakePinType(Call.GetString(TEXT("type")), Call.GetBool(TEXT("isArray")), PinType, PinError))
					return FUnrealMcpToolResult::Error(PinError);

				const FString DefaultValue = Call.GetString(TEXT("defaultValue"));
				if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType, DefaultValue))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("AddMemberVariable failed for '%s' (type may be unsupported as a member variable)."), *VarName));

				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("variable"), VarName);
				Out->SetStringField(TEXT("type"), PinTypeToString(PinType));
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Added variable '%s' (%s) to '%s'."), *VarName, *PinTypeToString(PinType), *Blueprint->GetName()), Out);
			});

		Registry.Tool(TEXT("blueprint-modify-variable"))
			.Title(TEXT("Modify Blueprint Variable"))
			.Description(TEXT("Rename and/or retype an existing member variable. Provide 'newName' to rename and/or "
			                  "'newType' (with optional 'isArray') to change its pin type."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Existing variable name."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("newName"), TEXT("Optional new variable name."))
			.ParamString(TEXT("newType"), TEXT("Optional new type token (see blueprint-add-variable)."))
			.ParamBool(TEXT("isArray"), TEXT("Array flag applied with 'newType'. Defaults to false."))
			.DestructiveHint(false).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				const FString VarName = Call.GetString(TEXT("name"));
				if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VarName)) == INDEX_NONE)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("variable '%s' not found."), *VarName));

				const FString NewName = Call.GetString(TEXT("newName"));
				const FString NewType = Call.GetString(TEXT("newType"));
				if (NewName.IsEmpty() && NewType.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("provide at least one of 'newName' or 'newType'."));

				FName EffectiveName = FName(*VarName);

				// Validate the rename BEFORE any mutation: the retype below (ChangeMemberVariableType) commits
				// immediately, so if a colliding/ill-formed newName were checked afterwards the tool would report
				// failure while the type change had already landed — the agent's model and the asset disagree.
				// RenameMemberVariable is void and does no validity/collision check of its own — renaming onto an
				// existing name silently produces duplicate member names that break a later compile.
				if (!NewName.IsEmpty())
				{
					FString NameError;
					if (!IsNameWellFormed(Blueprint, NewName, NameError))
						return FUnrealMcpToolResult::Error(NameError);
					if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*NewName)) != INDEX_NONE)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("cannot rename to '%s': a variable with that name already exists."), *NewName));
				}

				if (!NewType.IsEmpty())
				{
					FEdGraphPinType PinType;
					FString PinError;
					if (!MakePinType(NewType, Call.GetBool(TEXT("isArray")), PinType, PinError))
						return FUnrealMcpToolResult::Error(PinError);
					FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, EffectiveName, PinType);
				}
				if (!NewName.IsEmpty())
				{
					FBlueprintEditorUtils::RenameMemberVariable(Blueprint, EffectiveName, FName(*NewName));
					EffectiveName = FName(*NewName);
				}

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Modified variable '%s' on '%s' (now '%s')."), *VarName, *Blueprint->GetName(), *EffectiveName.ToString()));
			});
	}

	// ---- blueprint-set-default (CDO property write) ----------------------------------------------
	static void RegisterSetDefault(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-set-default"))
			.Title(TEXT("Set Blueprint Default"))
			.Description(TEXT("Set a default property value on the Blueprint's Class Default Object (CDO). 'value' is "
			                  "parsed with the property's own text importer, so it accepts the same text format the "
			                  "editor uses (numbers, '(X=1,Y=2,Z=3)' for structs, asset paths for object refs). This "
			                  "changes the class default, so it affects newly-spawned instances only — not actors "
			                  "already placed in a level."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("property"), TEXT("CDO property name."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("value"), TEXT("Value in UE text format."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(false).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));
				if (!Blueprint->GeneratedClass)
					return FUnrealMcpToolResult::Error(TEXT("Blueprint has no GeneratedClass; compile it first (blueprint-compile)."));

				UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
				if (!CDO)
					return FUnrealMcpToolResult::Error(TEXT("could not resolve the Class Default Object."));

				const FString PropName = Call.GetString(TEXT("property"));
				FProperty* Prop = FindFProperty<FProperty>(Blueprint->GeneratedClass, FName(*PropName));
				if (!Prop)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("property '%s' not found on '%s' (if you just added it via blueprint-add-variable, run blueprint-compile first so it lands on the generated class)."), *PropName, *Blueprint->GeneratedClass->GetName()));

				const FString Value = Call.GetString(TEXT("value"));
				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
				// Bracket the CDO write with Pre/PostEditChange so any open Details panel or property-change
				// observers refresh — ImportText_Direct alone mutates the memory silently. PostEditChange runs
				// on both paths because a parse failure can still leave the value partially written.
				CDO->PreEditChange(Prop);
				FStringOutputDevice ImportError;
				const TCHAR* Result = Prop->ImportText_Direct(*Value, ValuePtr, CDO, PPF_None, &ImportError);
				FPropertyChangedEvent ChangeEvent(Prop);
				CDO->PostEditChangeProperty(ChangeEvent);
				if (Result == nullptr || !ImportError.IsEmpty())
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("failed to parse value '%s' for property '%s': %s"), *Value, *PropName, *ImportError));

				CDO->MarkPackageDirty();
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("property"), PropName);
				Out->SetStringField(TEXT("value"), Value);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Set %s.%s = %s (affects new instances only)."), *Blueprint->GetName(), *PropName, *Value), Out);
			});
	}

	// ---- blueprint-add-function / blueprint-add-event --------------------------------------------
	static void RegisterGraphStubs(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-add-function"))
			.Title(TEXT("Add Blueprint Function"))
			.Description(TEXT("Add a new user function graph (entry/result nodes auto-wired by the K2 schema) via "
			                  "FBlueprintEditorUtils::CreateNewGraph + AddFunctionGraph. Body authoring (free-form "
			                  "node wiring) is backlog; this creates the callable stub."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("New function name."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(false).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				const FString FuncName = Call.GetString(TEXT("name"));
				if (FuncName.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("'name' is required."));
				FString NameError;
				if (!IsNameWellFormed(Blueprint, FuncName, NameError))
					return FUnrealMcpToolResult::Error(NameError);
				for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
					if (Graph && Graph->GetFName() == FName(*FuncName))
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("a function named '%s' already exists."), *FuncName));
				// CreateNewGraph resolves an outer-name clash by renaming the EXISTING object aside, so a name
				// colliding with the ubergraph (EventGraph) or any other graph object outered to the Blueprint
				// would silently hijack it and report success. Reject up front whenever the name is already taken.
				if (FindObject<UObject>(Blueprint, *FuncName) != nullptr)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("a graph or object named '%s' already exists on this Blueprint."), *FuncName));

				UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
					Blueprint, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				if (!NewGraph)
					return FUnrealMcpToolResult::Error(TEXT("CreateNewGraph returned null."));

				FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated*/ true, /*SignatureFromObject*/ static_cast<UClass*>(nullptr));

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Added function '%s' to '%s'."), *FuncName, *Blueprint->GetName()));
			});

		Registry.Tool(TEXT("blueprint-add-event"))
			.Title(TEXT("Add Blueprint Event"))
			.Description(TEXT("Add an overridable parent event node (e.g. ReceiveBeginPlay, ReceiveTick) to the event "
			                  "graph via FKismetEditorUtilities::AddDefaultEventNode. 'name' is the parent UFunction "
			                  "name; it must be an overridable event on the parent class."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Parent event function name (e.g. ReceiveBeginPlay)."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(false).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));
				if (!Blueprint->ParentClass)
					return FUnrealMcpToolResult::Error(TEXT("Blueprint has no parent class."));

				UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
				if (!EventGraph)
					return FUnrealMcpToolResult::Error(TEXT("Blueprint has no event graph."));

				const FString EventName = Call.GetString(TEXT("name"));
				UFunction* EventFunc = Blueprint->ParentClass->FindFunctionByName(FName(*EventName));
				if (!EventFunc)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a function on parent class '%s' (overridable events are named e.g. ReceiveBeginPlay/ReceiveTick)."), *EventName, *Blueprint->ParentClass->GetName()));
				// FindFunctionByName matches ANY parent UFunction (e.g. K2_DestroyActor, which is BlueprintCallable
				// but not a BlueprintEvent); only blueprint-overridable events may be placed as event nodes,
				// otherwise we'd seed a nonsense node and report success. Use the K2 schema's own canonical check
				// (it requires FUNC_BlueprintEvent and excludes static/const/deprecated/thread-safe functions).
				if (!UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(EventFunc))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not an overridable Blueprint event on '%s'."), *EventName, *Blueprint->ParentClass->GetName()));

				// A fresh Actor event graph is pre-seeded with DISABLED ghost nodes for the common events
				// (ReceiveBeginPlay/ReceiveTick), and AddDefaultEventNode returns that ghost rather than minting a
				// second node. So an ENABLED existing node is a real duplicate (reject), but a disabled ghost is
				// exactly the node we want to ENABLE — enabling the ghost IS the "add event" operation.
				UK2Node_Event* EventNode = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, EventFunc->GetOwnerClass(), FName(*EventName));
				if (EventNode && EventNode->IsNodeEnabled())
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("event '%s' already exists on '%s'."), *EventName, *Blueprint->GetName()));
				if (!EventNode)
				{
					int32 NodePosY = 0;
					EventNode = FKismetEditorUtilities::AddDefaultEventNode(
						Blueprint, EventGraph, FName(*EventName), EventFunc->GetOwnerClass(), NodePosY);
				}
				if (!EventNode)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("could not add event node for '%s'."), *EventName));
				// The auto-placed ghost node is EnabledState=Disabled and excluded from compilation; enable it so
				// the event actually fires.
				EventNode->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Added event '%s' to '%s'."), *EventName, *Blueprint->GetName()));
			});
	}

	// ---- blueprint-compile (structured error/warning list — the mandatory AI feedback loop) ------
	static void RegisterCompile(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-compile"))
			.Title(TEXT("Compile Blueprint"))
			.Description(TEXT("Compile a Blueprint via FKismetEditorUtilities::CompileBlueprint and return a STRUCTURED "
			                  "error/warning list. Each message entry carries 'severity', 'message', and best-effort "
			                  "'node'/'graph' context — this is the feedback loop an AI uses to self-correct."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(false).IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));

				FCompilerResultsLog Results;
				Results.bSilentMode = true;
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

				TArray<TSharedPtr<FJsonValue>> Messages;
				for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
				{
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					const EMessageSeverity::Type Sev = Msg->GetSeverity();
					Entry->SetStringField(TEXT("severity"),
						Sev == EMessageSeverity::Error ? TEXT("error") :
						Sev == EMessageSeverity::Warning ? TEXT("warning") : TEXT("info"));
					Entry->SetStringField(TEXT("message"), Msg->ToText().ToString());

					// Best-effort node/graph attribution from any UObject tokens the compiler attached.
					FString NodeName, GraphName;
					for (const TSharedRef<IMessageToken>& Token : Msg->GetMessageTokens())
					{
						if (Token->GetType() != EMessageToken::Object)
							continue;
						const FUObjectToken& ObjToken = static_cast<const FUObjectToken&>(Token.Get());
						const UObject* Obj = ObjToken.GetObject().Get();
						if (!Obj)
							continue;
						if (NodeName.IsEmpty() && Obj->IsA(UEdGraphNode::StaticClass()))
							NodeName = Obj->GetName();
						else if (GraphName.IsEmpty() && Obj->IsA(UEdGraph::StaticClass()))
							GraphName = Obj->GetName();
					}
					Entry->SetStringField(TEXT("node"), NodeName);
					Entry->SetStringField(TEXT("graph"), GraphName);
					Messages.Add(MakeShared<FJsonValueObject>(Entry));
				}

				const bool bCompiled = (Results.NumErrors == 0);
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("succeeded"), bCompiled);
				Out->SetNumberField(TEXT("numErrors"), Results.NumErrors);
				Out->SetNumberField(TEXT("numWarnings"), Results.NumWarnings);
				Out->SetArrayField(TEXT("messages"), Messages);

				const FString Summary = FString::Printf(TEXT("Compiled '%s': %d error(s), %d warning(s)."),
					*Blueprint->GetName(), Results.NumErrors, Results.NumWarnings);

				// A failed compile is a NORMAL, expected result for the AI loop — return success:false but carry the
				// full structured diagnostics so the agent can fix and recompile.
				return bCompiled
					? FUnrealMcpToolResult::Success(Summary, Out)
					: FUnrealMcpToolResult{ false, Summary, Out };
			});
	}

	// ---- blueprint-spawn -------------------------------------------------------------------------
	static void RegisterSpawn(FUnrealMcpToolRegistry& Registry)
	{
		Registry.Tool(TEXT("blueprint-spawn"))
			.Title(TEXT("Spawn Blueprint"))
			.Description(TEXT("Instance a Blueprint's generated Actor class into the current editor level — closing the "
			                  "create -> edit -> compile -> verify loop. Optionally set a world location and a label "
			                  "(spawn rotation is fixed at identity in the MVP)."))
			.ParamString(TEXT("path"), TEXT("Blueprint asset object path."), EUnrealMcpParamRequirement::Required)
			.ParamVector(TEXT("location"), TEXT("World location. Defaults to origin."))
			.ParamString(TEXT("name"), TEXT("Optional actor label."))
			.DestructiveHint(false).ReadOnlyHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UBlueprint* Blueprint = ResolveBlueprint(Call.GetString(TEXT("path")));
				if (!Blueprint)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Blueprint not found at '%s'."), *Call.GetString(TEXT("path"))));
				if (!Blueprint->GeneratedClass)
					return FUnrealMcpToolResult::Error(TEXT("Blueprint has no GeneratedClass; compile it first (blueprint-compile)."));
				if (!Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
					return FUnrealMcpToolResult::Error(TEXT("Blueprint's generated class is not an Actor; only Actor Blueprints can be spawned."));
				if (!GEditor)
					return FUnrealMcpToolResult::Error(TEXT("GEditor is unavailable (no editor context)."));

				// Spawn through the EDITOR WORLD directly rather than UEditorActorSubsystem::SpawnActorFromClass:
				// the subsystem path is viewport-aware (it positions relative to the active level-editor viewport)
				// and divides by zero in a headless -nullrhi world with no viewport. World->SpawnActor is
				// viewport-independent and works in both the editor and headless automation/e2e contexts.
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("no editor world available to spawn into."));

				const FVector Location = Call.GetVector(TEXT("location"), FVector::ZeroVector);
				const FTransform SpawnTransform(FRotator::ZeroRotator, Location);
				FActorSpawnParameters SpawnParams;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AActor* Actor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, SpawnTransform, SpawnParams);
				if (!Actor)
					return FUnrealMcpToolResult::Error(TEXT("World->SpawnActor failed."));

				if (Call.Has(TEXT("name")))
					Actor->SetActorLabel(Call.GetString(TEXT("name")));

				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("actor"), Actor->GetActorLabel());
				Out->SetStringField(TEXT("class"), Blueprint->GeneratedClass->GetName());
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Spawned '%s' (%s) into the level."), *Actor->GetActorLabel(), *Blueprint->GeneratedClass->GetName()), Out);
			});
	}

	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		RegisterCreate(Registry);
		RegisterGet(Registry);
		RegisterComponents(Registry);
		RegisterVariables(Registry);
		RegisterSetDefault(Registry);
		RegisterGraphStubs(Registry);
		RegisterCompile(Registry);
		RegisterSpawn(Registry);
	}
}
