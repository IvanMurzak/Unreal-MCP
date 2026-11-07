// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIPCGRqlApi.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIPCGCatalog.h"
#include "UnrealAIPCGGraphEdit.h"

#include "Algo/AnyOf.h"
#include "AssetToolsModule.h"
#include "Async/TaskGraphInterfaces.h"
#include "Elements/PCGUserParameterGet.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/ITransaction.h"
#include "PCGComponent.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGVolume.h"
#include "StructUtils/PropertyBag.h"
#include "Subsystems/PCGSubsystem.h"
#include "Utils/PCGPreconfiguration.h"

namespace UnrealAIPCGRqlApiLocal
{
	using namespace UnrealAIPCG;

	/** Collects the PCG errors and warnings logged while a generation runs (PCG logs from worker threads too). */
	class FPCGLogCapture : public FOutputDevice
	{
	public:
		FPCGLogCapture() { GLog->AddOutputDevice(this); }
		virtual ~FPCGLogCapture() override { GLog->RemoveOutputDevice(this); }

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity > ELogVerbosity::Warning || !Category.ToString().StartsWith(TEXT("LogPCG")))
				return;
			FScopeLock Lock(&Mutex);
			TArray<FString>& Lines = Verbosity <= ELogVerbosity::Error ? Errors : Warnings;
			if (Lines.Num() < 20)
				Lines.AddUnique(FString(Message).TrimStartAndEnd());
		}

		void TakeInto(FUnrealAIPCGGenerateResult& Result)
		{
			FScopeLock Lock(&Mutex);
			Result.Errors.Append(Errors);
			Result.Warnings.Append(Warnings);
		}

	private:
		FCriticalSection Mutex;
		TArray<FString> Errors;
		TArray<FString> Warnings;
	};

	TArray<UPCGComponent*> Components(AActor& Actor)
	{
		TArray<UPCGComponent*> Out;
		Actor.GetComponents(Out);
		return Out;
	}

	/** Object names may not hold spaces or path characters: "Tree Scatter" becomes Tree_Scatter. */
	FString ObjectName(const FString& Name)
	{
		FString Out;
		for (const TCHAR Char : Name.TrimStartAndEnd())
			Out.AppendChar(FChar::IsAlnum(Char) ? Char : TEXT('_'));
		return Out;
	}

	/** Lay a new node out right of the rightmost node, so the graph editor shows the chain left to right. */
	void Place(UPCGGraph& Graph, UPCGNode& Node)
	{
		int32 MaxX = 0, X = 0, Y = 0;
		bool bAny = false;
		for (const UPCGNode* Other : Graph.GetNodes())
		{
			if (Other && Other != &Node)
			{
				Other->GetNodePosition(X, Y);
				MaxX = bAny ? FMath::Max(MaxX, X) : X;
				bAny = true;
			}
		}
		Node.SetNodePosition(bAny ? MaxX + 300 : 0, 0);
	}

	const FPropertyBagPropertyDesc* FindParameter(const UPCGGraph& Graph, const FString& Name)
	{
		const FInstancedPropertyBag* Bag = Graph.GetUserParametersStruct();
		const UPropertyBag* Struct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
		if (!Struct)
			return nullptr;
		for (const FPropertyBagPropertyDesc& Desc : Struct->GetPropertyDescs())
		{
			if (Normalize(Desc.Name.ToString()) == Normalize(Name))
				return &Desc;
		}
		return nullptr;
	}

	FString ParameterNames(const UPCGGraph& Graph)
	{
		TArray<FString> Names;
		const FInstancedPropertyBag* Bag = Graph.GetUserParametersStruct();
		if (const UPropertyBag* Struct = Bag ? Bag->GetPropertyBagStruct() : nullptr)
		{
			for (const FPropertyBagPropertyDesc& Desc : Struct->GetPropertyDescs())
				Names.Add(Desc.Name.ToString());
		}
		return Names.Num() ? FString::Join(Names, TEXT(", ")) : FString(TEXT("none: SetParameter adds one"));
	}

	/** The property bag type of a new parameter: from Type, else from the value. */
	bool ParameterType(const FString& TypeName, const FString& Value, FPropertyBagPropertyDesc& Out, FString& Error)
	{
		const FString Wanted = Normalize(TypeName);
		const FName Name = Out.Name;
		if (Wanted.IsEmpty())
		{
			const FString Text = Value.TrimStartAndEnd();
			const FString Lower = Text.ToLower();
			TArray<FString> Parts;
			Text.Replace(TEXT("("), TEXT("")).Replace(TEXT(")"), TEXT("")).ParseIntoArray(Parts, TEXT(","));
			double Number = 0.0;
			if (Lower == TEXT("true") || Lower == TEXT("false"))
				Out = FPropertyBagPropertyDesc(Name, EPropertyBagPropertyType::Bool);
			else if (Text.StartsWith(TEXT("(")) && Parts.Num() == 3)
				Out = FPropertyBagPropertyDesc(Name, EPropertyBagPropertyType::Struct, TBaseStructure<FVector>::Get());
			else if (Text.StartsWith(TEXT("(")) && Parts.Num() == 2)
				Out = FPropertyBagPropertyDesc(Name, EPropertyBagPropertyType::Struct, TBaseStructure<FVector2D>::Get());
			else if (Text.StartsWith(TEXT("(")) && Parts.Num() == 4)
				Out = FPropertyBagPropertyDesc(Name, EPropertyBagPropertyType::Struct, TBaseStructure<FVector4>::Get());
			else if (LexTryParseString(Number, *Text))
				Out = FPropertyBagPropertyDesc(Name, Text.Contains(TEXT(".")) || Text.Contains(TEXT("e")) ? EPropertyBagPropertyType::Double : EPropertyBagPropertyType::Int32);
			else
			{
				Error = FString::Printf(TEXT("cannot tell the type of '%s': pass Type (double, int, bool, string, name, Vector, Vector2, Vector4, Rotator, Transform, or an asset class such as StaticMesh)"), *Text);
				return false;
			}
			return true;
		}
		const TPair<const TCHAR*, EPropertyBagPropertyType> Scalars[] = {
			{ TEXT("double"), EPropertyBagPropertyType::Double }, { TEXT("float"), EPropertyBagPropertyType::Double },
			{ TEXT("number"), EPropertyBagPropertyType::Double }, { TEXT("int"), EPropertyBagPropertyType::Int32 },
			{ TEXT("int32"), EPropertyBagPropertyType::Int32 }, { TEXT("integer"), EPropertyBagPropertyType::Int32 },
			{ TEXT("int64"), EPropertyBagPropertyType::Int64 }, { TEXT("bool"), EPropertyBagPropertyType::Bool },
			{ TEXT("boolean"), EPropertyBagPropertyType::Bool }, { TEXT("string"), EPropertyBagPropertyType::String },
			{ TEXT("name"), EPropertyBagPropertyType::Name },
		};
		for (const TPair<const TCHAR*, EPropertyBagPropertyType>& Scalar : Scalars)
		{
			if (Wanted == Scalar.Key)
				return (Out = FPropertyBagPropertyDesc(Name, Scalar.Value)), true;
		}
		const TPair<const TCHAR*, UScriptStruct*> Structs[] = {
			{ TEXT("vector"), TBaseStructure<FVector>::Get() }, { TEXT("vector3"), TBaseStructure<FVector>::Get() },
			{ TEXT("vector2"), TBaseStructure<FVector2D>::Get() }, { TEXT("vector2d"), TBaseStructure<FVector2D>::Get() },
			{ TEXT("vector4"), TBaseStructure<FVector4>::Get() }, { TEXT("rotator"), TBaseStructure<FRotator>::Get() },
			{ TEXT("transform"), TBaseStructure<FTransform>::Get() },
		};
		for (const TPair<const TCHAR*, UScriptStruct*>& Struct : Structs)
		{
			if (Wanted == Struct.Key)
				return (Out = FPropertyBagPropertyDesc(Name, EPropertyBagPropertyType::Struct, Struct.Value)), true;
		}
		UClass* Class = UnrealMcpRql::ResolveClass(TypeName);
		if (!Class)
		{
			Error = FString::Printf(TEXT("unknown type '%s' (double, int, int64, bool, string, name, Vector, Vector2, Vector4, Rotator, Transform, or an asset class such as StaticMesh)"), *TypeName);
			return false;
		}
		Out = FPropertyBagPropertyDesc(Name, EPropertyBagPropertyType::SoftObject, Class);
		return true;
	}

	/** Parse @p Value for the parameter @p Desc into a scratch bag: a bad value changes nothing. */
	bool CheckParameterValue(const FPropertyBagPropertyDesc& Desc, const FString& Value, FString& Error)
	{
		FInstancedPropertyBag Scratch;
		Scratch.AddProperties({ Desc });
		const UPropertyBag* Struct = Scratch.GetPropertyBagStruct();
		const FPropertyBagPropertyDesc* Added = Struct ? Struct->FindPropertyDescByName(Desc.Name) : nullptr;
		uint8* Memory = Scratch.GetMutableValue().GetMemory();
		if (!Added || !Added->CachedProperty || !Memory)
		{
			Error = TEXT("cannot hold that type");
			return false;
		}
		return ParseValue(*Added->CachedProperty, Added->CachedProperty->ContainerPtrToValuePtr<void>(Memory), Value, Error);
	}

	UPCGNode* GraphNode(UPCGGraph* Graph, const FString& Node, const TCHAR* Operation)
	{
		if (!Graph)
			return UnrealMcpRql::Failed<UPCGNode*>(FString::Printf(TEXT("%s needs a PCG graph (an asset path, e.g. /Game/PCG/PCG_Forest)"), Operation));
		return FindNode(*Graph, Node);
	}

	/** A pin's data type for messages ("Point", "Landscape", "Spatial", ...). */
	FString PinType(const UPCGPin& Pin)
	{
		return Pin.GetCurrentTypesID().ToDisplayText().ToString();
	}
}

UPCGGraph* UUnrealAIPCGRqlApi::CreateGraph(const FString& Path, const FString& Template)
{
	if (Template.IsEmpty())
		return Cast<UPCGGraph>(UnrealMcpRql::CreateAsset(UPCGGraph::StaticClass(), Path));
	UPCGGraph* Source = Cast<UPCGGraph>(UnrealMcpRql::LoadObject(UPCGGraph::StaticClass(), Template));
	if (!Source)
		return UnrealMcpRql::Failed<UPCGGraph*>(FString::Printf(TEXT("no PCG graph at '%s' (pcgTemplates | select Name Path Description lists the templates)"), *Template));
	FString Folder, Name;
	if (!FPackageName::IsValidLongPackageName(Path) || !Path.Split(TEXT("/"), &Folder, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || Folder.IsEmpty())
		return UnrealMcpRql::Failed<UPCGGraph*>(FString::Printf(TEXT("'%s' is not an asset path such as /Game/PCG/PCG_Forest"), *Path));
	if (FindPackage(nullptr, *Path) || FPackageName::DoesPackageExist(Path))
		return UnrealMcpRql::Failed<UPCGGraph*>(FString::Printf(TEXT("an asset already exists at '%s' (pick another path, or editor | call DeleteAsset(\"%s\"))"), *Path, *Path));
	// As the graph factory's "from template": a copy that is not a template itself.
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UPCGGraph* Created = Cast<UPCGGraph>(AssetTools.DuplicateAsset(Name, Folder, Source));
	if (!Created)
		return UnrealMcpRql::Failed<UPCGGraph*>(FString::Printf(TEXT("could not create '%s' from %s"), *Path, *Source->GetPathName()));
	Created->bIsTemplate = false;
	Created->MarkPackageDirty();
	return Created;
}

FString UUnrealAIPCGRqlApi::AddNode(UPCGGraph* Graph, const FString& Type, const FString& Name)
{
	using namespace UnrealAIPCGRqlApiLocal;
	if (!Graph)
		return UnrealMcpRql::Failed<FString>(TEXT("AddNode needs a PCG graph (an asset path, e.g. /Game/PCG/PCG_Forest)"));
	UnrealAIPCGCatalog::FNodeType NodeType;
	if (!UnrealAIPCGCatalog::FindNodeType(Type, NodeType))
		return FString();
	const FString Wanted = ObjectName(Name);
	if (!Name.IsEmpty() && Wanted.IsEmpty())
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("'%s' is not a node name (letters, digits and _)"), *Name));
	if (!Wanted.IsEmpty() && Algo::AnyOf(Graph->GetNodes(), [&Wanted](const UPCGNode* Node) { return Node && Node->GetName() == Wanted; }))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s already has a node named %s"), *Graph->GetName(), *Wanted));
	Graph->Modify();
	UPCGSettings* Settings = nullptr;
	UPCGNode* Node = Graph->AddNodeOfType(NodeType.Class, Settings);
	if (!Node || !Settings)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not add a %s node"), *UnrealAIPCGCatalog::TypeName(NodeType.Class)));
	if (NodeType.Preset != INDEX_NONE)
	{
		// A preconfigured variant (as the palette entry does): e.g. a typed Get Graph Parameter, an attribute operation.
		for (const FPCGPreConfiguredSettingsInfo& Preset : Settings->GetPreconfiguredInfo())
		{
			if (Preset.PreconfiguredIndex == NodeType.Preset)
			{
				Settings->ApplyPreconfiguredSettings(Preset);
				Node->UpdateAfterSettingsChangeDuringCreation();
				break;
			}
		}
	}
	if (!Wanted.IsEmpty())
	{
		Node->Rename(*Wanted, nullptr, REN_DontCreateRedirectors);
		Node->SetNodeTitle(FName(*Name.TrimStartAndEnd()));
	}
	else if (const FName Base = Settings->GetDefaultNodeName(); Base != NAME_None && Node->GetFName() != Base && !StaticFindObjectFast(nullptr, Graph, Base))
	{
		// The first node of a type is named after it ("SurfaceSampler"), as the agent would call it; later ones get a number.
		Node->Rename(*Base.ToString(), nullptr, REN_DontCreateRedirectors);
	}
	Place(*Graph, *Node);
	Changed(*Graph);
	return Node->GetName();
}

void UUnrealAIPCGRqlApi::RemoveNode(UPCGGraph* Graph, const FString& Node)
{
	using namespace UnrealAIPCGRqlApiLocal;
	UPCGNode* Found = GraphNode(Graph, Node, TEXT("RemoveNode"));
	if (!Found)
		return;
	if (Found == Graph->GetInputNode() || Found == Graph->GetOutputNode())
		return UnrealMcpRql::Fail(TEXT("the graph's Input and Output nodes cannot be removed"));
	Graph->RemoveNode(Found);
	Changed(*Graph);
}

FString UUnrealAIPCGRqlApi::Connect(UPCGGraph* Graph, const FString& From, const FString& To, const FString& FromPin, const FString& ToPin)
{
	using namespace UnrealAIPCGRqlApiLocal;
	UPCGNode* FromNode = GraphNode(Graph, From, TEXT("Connect"));
	UPCGNode* ToNode = FromNode ? FindNode(*Graph, To) : nullptr;
	if (!ToNode)
		return FString();
	if (FromNode == ToNode)
		return UnrealMcpRql::Failed<FString>(TEXT("a node cannot feed itself"));
	UPCGPin* Output = FindPin(*FromNode, FromPin, true);
	UPCGPin* Input = Output ? FindPin(*ToNode, ToPin, false) : nullptr;
	if (!Input)
		return FString();
	const FString Edge = PinPath(*Output) + TEXT(" -> ") + PinPath(*Input);
	if (Algo::AnyOf(Output->Edges, [Input](const UPCGEdge* Existing) { return Existing && Existing->OutputPin == Input; }))
		return Edge + TEXT(" (already connected)");
	FText Why;
	if (!PCGDataTypeCompatibilityResult::IsValid(Output->GetCompatibilityWithOtherPin(Input, &Why)))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s gives %s data, %s takes %s%s; put a node between that converts it (a sampler turns surfaces and splines into points)"),
			*PinPath(*Output), *PinType(*Output), *PinPath(*Input), *PinType(*Input), Why.IsEmpty() ? TEXT("") : *(TEXT(": ") + Why.ToString())));
	const bool bReplaced = Input->IsConnected() && !Input->AllowsMultipleConnections();
	Graph->Modify();
	Graph->AddEdge(FromNode, Output->Properties.Label, ToNode, Input->Properties.Label);
	if (!Algo::AnyOf(Output->Edges, [Input](const UPCGEdge* Existing) { return Existing && Existing->OutputPin == Input; }))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("PCG refused %s"), *Edge));
	Changed(*Graph);
	return bReplaced ? Edge + TEXT(" (replaced the pin's previous connection: it takes one)") : Edge;
}

int32 UUnrealAIPCGRqlApi::Disconnect(UPCGGraph* Graph, const FString& From, const FString& To, const FString& FromPin, const FString& ToPin)
{
	using namespace UnrealAIPCGRqlApiLocal;
	UPCGNode* FromNode = GraphNode(Graph, From, TEXT("Disconnect"));
	UPCGNode* ToNode = FromNode ? FindNode(*Graph, To) : nullptr;
	if (!ToNode)
		return 0;
	const UPCGPin* OnlyOutput = FromPin.IsEmpty() ? nullptr : FindPin(*FromNode, FromPin, true);
	const UPCGPin* OnlyInput = ToPin.IsEmpty() ? nullptr : FindPin(*ToNode, ToPin, false);
	if ((!FromPin.IsEmpty() && !OnlyOutput) || (!ToPin.IsEmpty() && !OnlyInput))
		return 0;
	TArray<TPair<FName, FName>> Edges;
	for (const UPCGPin* Output : FromNode->GetOutputPins())
	{
		if (!Output || (OnlyOutput && Output != OnlyOutput))
			continue;
		for (const UPCGEdge* Edge : Output->Edges)
		{
			const UPCGPin* Input = Edge ? Edge->OutputPin.Get() : nullptr;
			if (Input && Input->Node == ToNode && (!OnlyInput || Input == OnlyInput))
				Edges.Add({ Output->Properties.Label, Input->Properties.Label });
		}
	}
	if (Edges.Num() == 0)
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("%s does not feed %s%s (the graph's Edges member lists the connections)"), *From, *To,
			OnlyOutput || OnlyInput ? TEXT(" through those pins") : TEXT("")));
	Graph->Modify();
	for (const TPair<FName, FName>& Edge : Edges)
		Graph->RemoveEdge(FromNode, Edge.Key, ToNode, Edge.Value);
	Changed(*Graph);
	return Edges.Num();
}

FString UUnrealAIPCGRqlApi::SetNodeSetting(UPCGGraph* Graph, const FString& Node, const FString& Setting, const FString& Value)
{
	using namespace UnrealAIPCGRqlApiLocal;
	UPCGNode* Found = GraphNode(Graph, Node, TEXT("SetNodeSetting"));
	return Found ? SetSetting(*Found, Setting, Value) : FString();
}

FString UUnrealAIPCGRqlApi::SetParameter(UPCGGraph* Graph, const FString& Name, const FString& Value, const FString& Type)
{
	using namespace UnrealAIPCGRqlApiLocal;
	if (!Graph)
		return UnrealMcpRql::Failed<FString>(TEXT("SetParameter needs a PCG graph"));
	const FString Trimmed = ObjectName(Name);
	if (Trimmed.IsEmpty())
		return UnrealMcpRql::Failed<FString>(TEXT("the parameter name is empty"));
	FString Error;
	const FPropertyBagPropertyDesc* Existing = FindParameter(*Graph, Name);
	FPropertyBagPropertyDesc Desc = Existing ? *Existing : FPropertyBagPropertyDesc(FName(*Trimmed), EPropertyBagPropertyType::Double);
	if (!Existing && !ParameterType(Type, Value, Desc, Error))
		return UnrealMcpRql::Failed<FString>(Error);
	// Check the value first: a bad one adds no parameter and changes nothing.
	if (!CheckParameterValue(Desc, Value, Error))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("parameter %s: %s"), *Desc.Name.ToString(), *Error));
	Graph->Modify();
	if (!Existing && Graph->AddUserParameters({ Desc }) != EPropertyBagAlterationResult::Success)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("PCG graphs cannot hold a parameter of that type (%s)"), *Type));
	const FName ParameterName = Desc.Name;
	FString New;
	Graph->UpdateUserParametersStruct([&](FInstancedPropertyBag& Bag)
	{
		const UPropertyBag* Struct = Bag.GetPropertyBagStruct();
		const FPropertyBagPropertyDesc* Stored = Struct ? Struct->FindPropertyDescByName(ParameterName) : nullptr;
		uint8* Memory = Bag.GetMutableValue().GetMemory();
		if (!Stored || !Stored->CachedProperty || !Memory)
			return;
		void* Address = Stored->CachedProperty->ContainerPtrToValuePtr<void>(Memory);
		FString Ignored;
		if (ParseValue(*Stored->CachedProperty, Address, Value, Ignored))
			New = FormatValue(*Stored->CachedProperty, Address);
	});
	Changed(*Graph);
	return New.IsEmpty() ? UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("could not set parameter %s"), *ParameterName.ToString())) : New;
}

void UUnrealAIPCGRqlApi::RemoveParameter(UPCGGraph* Graph, const FString& Name)
{
	using namespace UnrealAIPCGRqlApiLocal;
	if (!Graph)
		return UnrealMcpRql::Fail(TEXT("RemoveParameter needs a PCG graph"));
	const FPropertyBagPropertyDesc* Desc = FindParameter(*Graph, Name);
	if (!Desc)
		return UnrealMcpRql::Fail(FString::Printf(TEXT("%s has no parameter '%s' (parameters: %s)"), *Graph->GetName(), *Name, *ParameterNames(*Graph)));
	const FName ParameterName = Desc->Name;
	Graph->Modify();
	Graph->UpdateUserParametersStruct([ParameterName](FInstancedPropertyBag& Bag) { Bag.RemovePropertyByName(ParameterName); });
	Changed(*Graph);
}

FString UUnrealAIPCGRqlApi::BindParameter(UPCGGraph* Graph, const FString& Parameter, const FString& Node, const FString& Setting)
{
	using namespace UnrealAIPCGRqlApiLocal;
	UPCGNode* Target = GraphNode(Graph, Node, TEXT("BindParameter"));
	if (!Target)
		return FString();
	const FPropertyBagPropertyDesc* Desc = FindParameter(*Graph, Parameter);
	if (!Desc)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no parameter '%s' (parameters: %s)"), *Graph->GetName(), *Parameter, *ParameterNames(*Graph)));
	UPCGPin* Pin = FindOverridePin(*Target, Setting);
	if (!Pin)
	{
		TArray<FString> Labels;
		if (const UPCGSettings* Settings = Target->GetSettings())
		{
			for (const FPCGSettingsOverridableParam& Param : Settings->OverridableParams())
				Labels.Add(FString::JoinBy(Param.PropertiesNames, TEXT("."), [](FName Name) { return Name.ToString(); }));
		}
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s cannot take '%s' from a parameter (settings a parameter can drive: %s)"), *NodeTitle(*Target), *Setting,
			Labels.Num() ? *FString::Join(Labels, TEXT(", ")) : TEXT("none")));
	}
	const FName ParameterName = Desc->Name;
	const FGuid ParameterId = Desc->ID;
	Graph->Modify();
	// One Get Graph Parameter node per parameter: reuse it for every setting the parameter drives.
	UPCGNode* Getter = nullptr;
	for (UPCGNode* Candidate : Graph->GetNodes())
	{
		const UPCGUserParameterGetSettings* Get = Candidate ? Cast<UPCGUserParameterGetSettings>(Candidate->GetSettings()) : nullptr;
		if (Get && Get->PropertyName == ParameterName)
			Getter = Candidate;
	}
	if (!Getter)
	{
		UPCGSettings* Settings = nullptr;
		Getter = Graph->AddNodeOfType(UPCGUserParameterGetSettings::StaticClass(), Settings);
		UPCGUserParameterGetSettings* Get = Cast<UPCGUserParameterGetSettings>(Settings);
		if (!Getter || !Get)
			return UnrealMcpRql::Failed<FString>(TEXT("could not add a Get Graph Parameter node"));
		// As dragging a parameter onto the graph does: the node reads that parameter, titled by it.
		Get->PropertyGuid = ParameterId;
		Get->PropertyName = ParameterName;
		Getter->SetNodeTitle(ParameterName, /*bApplySanitization*/ false);
		if (!StaticFindObjectFast(nullptr, Graph, ParameterName))
			Getter->Rename(*ParameterName.ToString(), nullptr, REN_DontCreateRedirectors); // the Edges member reads "Density.Density -> ..."
		Getter->SetSettingsInterface(Get);
		int32 X = 0, Y = 0;
		Target->GetNodePosition(X, Y);
		Getter->SetNodePosition(X - 250, Y + 200);
	}
	UPCGPin* Output = Getter->GetOutputPin(ParameterName);
	if (!Output)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("the Get Graph Parameter node of %s has no output pin"), *ParameterName.ToString()));
	Graph->AddEdge(Getter, Output->Properties.Label, Target, Pin->Properties.Label);
	Changed(*Graph);
	return FString::Printf(TEXT("%s -> %s"), *ParameterName.ToString(), *PinPath(*Pin));
}

UPCGComponent* UUnrealAIPCGRqlApi::AddComponent(AActor* Actor, UPCGGraph* Graph)
{
	// The core AddComponent (instance component, transactional); the graph is bound before the component registers.
	return Cast<UPCGComponent>(UnrealMcpRql::AddComponent(Actor, UPCGComponent::StaticClass(), FString(),
		[Graph](UActorComponent* Component)
		{
			if (Graph)
				CastChecked<UPCGComponent>(Component)->SetGraphLocal(Graph);
		}));
}

UPCGComponent* UUnrealAIPCGRqlApi::SetGraph(AActor* Actor, UPCGGraph* Graph)
{
	if (!Actor || !Graph)
		return UnrealMcpRql::Failed<UPCGComponent*>(TEXT("SetGraph needs an actor (label, name or ref) and a PCG graph"));
	UPCGComponent* Component = Actor->FindComponentByClass<UPCGComponent>();
	if (!Component)
		return AddComponent(Actor, Graph);
	Component->Modify();
	Component->SetGraphLocal(Graph);
	return Component;
}

AActor* UUnrealAIPCGRqlApi::SpawnVolume(UPCGGraph* Graph, FVector Location, FVector Size, const FString& Label)
{
	if (!Graph)
		return UnrealMcpRql::Failed<AActor*>(TEXT("SpawnVolume needs a PCG graph (an asset path)"));
	if (Size.X <= 0.0 || Size.Y <= 0.0 || Size.Z <= 0.0)
		return UnrealMcpRql::Failed<AActor*>(TEXT("every Size component must be > 0 (cm, the full box: \"(50000, 50000, 20000)\" = 500 x 500 m, 200 m tall)"));
	// The core spawn gives a volume the editor's 200 cm box brush; the scale sizes it (as the PCG Volume factory does).
	APCGVolume* Volume = Cast<APCGVolume>(UnrealMcpRql::SpawnActor(APCGVolume::StaticClass(), Location, FRotator::ZeroRotator, Label));
	if (!Volume)
		return nullptr;
	Volume->SetActorScale3D(Size / 200.0);
	if (!Volume->PCGComponent)
		return UnrealMcpRql::Failed<AActor*>(TEXT("the PCG Volume has no PCG component"));
	Volume->PCGComponent->SetGraphLocal(Graph);
	return Volume;
}

FUnrealAIPCGGenerateResult UUnrealAIPCGRqlApi::Generate(AActor* Actor, int32 Seed, double TimeoutSeconds)
{
	using namespace UnrealAIPCGRqlApiLocal;
	FUnrealAIPCGGenerateResult Result;
	if (!Actor)
		return UnrealMcpRql::Failed<FUnrealAIPCGGenerateResult>(TEXT("Generate needs an actor (label, name or ref)"));
	TArray<UPCGComponent*> Targets = Components(*Actor);
	if (Targets.Num() == 0)
		return UnrealMcpRql::Failed<FUnrealAIPCGGenerateResult>(FString::Printf(TEXT("%s has no PCG component (pcg | call SetGraph(actor, graph) adds one, or SpawnVolume places a PCG Volume)"),
			*Actor->GetActorNameOrLabel()));
	for (const UPCGComponent* Component : Targets)
	{
		if (!Component->GetGraph())
			return UnrealMcpRql::Failed<FUnrealAIPCGGenerateResult>(FString::Printf(TEXT("the PCG component of %s has no graph (pcg | call SetGraph(actor, graph))"), *Actor->GetActorNameOrLabel()));
	}
	if (Seed != -1)
	{
		for (UPCGComponent* Component : Targets)
		{
			Component->Modify();
			Component->Seed = Seed;
		}
	}
	UWorld* World = Actor->GetWorld();
	UPCGSubsystem* Subsystem = World ? UPCGSubsystem::GetInstance(World) : nullptr;
	if (!Subsystem)
		return UnrealMcpRql::Failed<FUnrealAIPCGGenerateResult>(TEXT("the world of that actor has no PCG subsystem"));
	FPCGLogCapture Log;
	const double Start = FPlatformTime::Seconds();
	// PCG pauses while an editor transaction is open (pcg.PauseExecutionWhileTransactionActive): this call runs inside
	// the RQL mutation's transaction, so let PCG run while it waits.
	IConsoleVariable* PauseInTransaction = IConsoleManager::Get().FindConsoleVariable(TEXT("pcg.PauseExecutionWhileTransactionActive"));
	const bool bWasPaused = PauseInTransaction && PauseInTransaction->GetBool();
	if (bWasPaused)
		PauseInTransaction->Set(false, ECVF_SetByCode);
	ON_SCOPE_EXIT
	{
		if (bWasPaused)
			PauseInTransaction->Set(true, ECVF_SetByCode);
	};
	{
		// What generation spawns is PCG's (managed resources), not part of the undo step: as generating in the editor.
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		for (UPCGComponent* Component : Targets)
			Component->GenerateLocal(/*bForce*/ true);
		// The editor ticks PCG every frame; a tool call waits for the result instead.
		const double Deadline = Start + FMath::Max(1.0, TimeoutSeconds);
		while (Algo::AnyOf(Targets, [](const UPCGComponent* Component) { return Component->IsGenerating(); }) && FPlatformTime::Seconds() < Deadline)
		{
			Subsystem->Tick(1.0f / 60.0f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::SleepNoStats(0.001f);
		}
	}
	Result.Seconds = FPlatformTime::Seconds() - Start;
	Result.Generated = !Algo::AnyOf(Targets, [](const UPCGComponent* Component) { return Component->IsGenerating(); });
	Result.Seed = Targets[0]->Seed;
	TMap<FString, int32> Meshes;
	for (UPCGComponent* Component : Targets)
	{
		const FGenerated Generated = CountGenerated(*Component);
		Result.Instances += Generated.Instances;
		Result.Points += Generated.Points;
		Result.Components += Generated.Components;
		Result.Actors += Generated.Actors;
		for (const TPair<FString, int32>& Mesh : Generated.Meshes)
			Meshes.FindOrAdd(Mesh.Key) += Mesh.Value;
	}
	FGenerated Merged;
	Merged.Meshes = MoveTemp(Meshes);
	Result.Meshes = Merged.MeshLines();
	Log.TakeInto(Result);
	if (!Result.Generated)
		Result.Errors.Add(FString::Printf(TEXT("still generating after %.0f s: read the Generated member of its PCG component later, or call Generate with a longer TimeoutSeconds"), Result.Seconds));
	return Result;
}

int32 UUnrealAIPCGRqlApi::Cleanup(AActor* Actor)
{
	using namespace UnrealAIPCGRqlApiLocal;
	if (!Actor)
		return UnrealMcpRql::Failed<int32>(TEXT("Cleanup needs an actor (label, name or ref)"));
	TArray<UPCGComponent*> Targets = Components(*Actor);
	if (Targets.Num() == 0)
		return UnrealMcpRql::Failed<int32>(FString::Printf(TEXT("%s has no PCG component"), *Actor->GetActorNameOrLabel()));
	int32 Removed = 0;
	TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
	for (UPCGComponent* Component : Targets)
	{
		Removed += CountGenerated(*Component).Instances;
		Component->CleanupLocalImmediate(/*bRemoveComponents*/ true);
	}
	return Removed;
}
