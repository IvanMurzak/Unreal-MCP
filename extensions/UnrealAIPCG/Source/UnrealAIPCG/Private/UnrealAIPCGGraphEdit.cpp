// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealAIPCGGraphEdit.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIPCGCatalog.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Editor.h"
#include "Elements/PCGAttributeFilter.h"
#include "Engine/StaticMesh.h"
#include "MeshSelectors/PCGMeshSelectorWeighted.h"
#include "Metadata/PCGAttributePropertySelector.h"
#include "Metadata/PCGMetadataTypesConstantStruct.h"
#include "Containers/Ticker.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "PCGCommon.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGManagedResource.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Data/PCGBasePointData.h"
#include "Data/Registry/PCGDataTypeIdentifier.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "StructUtils/PropertyBag.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/PCGSubsystem.h"
#include "UObject/UObjectHash.h"

namespace UnrealAIPCGGraphEditLocal
{
	using namespace UnrealAIPCG;

	TArray<UPCGNode*> AllNodesOf(const UPCGGraph& Graph)
	{
		TArray<UPCGNode*> Out;
		if (Graph.GetInputNode())
			Out.Add(Graph.GetInputNode());
		if (Graph.GetOutputNode())
			Out.Add(Graph.GetOutputNode());
		for (UPCGNode* Node : Graph.GetNodes())
		{
			if (Node)
				Out.Add(Node);
		}
		return Out;
	}

	/** What agents call a node: Input / Output for the graph's own nodes, else its Name. */
	FString NodeName(const UPCGNode& Node)
	{
		const UPCGGraph* Graph = Node.GetGraph();
		return Graph && (&Node == Graph->GetInputNode() || &Node == Graph->GetOutputNode()) ? NodeTitle(Node) : Node.GetName();
	}

	FString NodeLabel(const UPCGNode& Node)
	{
		const FString Title = NodeTitle(Node);
		return Title == NodeName(Node) ? Title : FString::Printf(TEXT("%s (%s)"), *NodeName(Node), *Title);
	}

	/** The overridable setting an input pin overrides; null for a data pin. */
	const FPCGSettingsOverridableParam* OverrideOf(const UPCGPin& Pin)
	{
		const UPCGSettings* Settings = !Pin.IsOutputPin() && Pin.Node ? Pin.Node->GetSettings() : nullptr;
		return Settings ? Settings->OverridableParams().FindByPredicate([&Pin](const FPCGSettingsOverridableParam& Param) { return Param.Label == Pin.Properties.Label; })
			: nullptr;
	}

	FString SettingPathOf(const FPCGSettingsOverridableParam& Param)
	{
		return FString::JoinBy(Param.PropertiesNames, TEXT("."), [](FName Name) { return Name.ToString(); });
	}

	/** The data pins (override pins are many and named by the Settings member). */
	FString PinLabels(const TArray<TObjectPtr<UPCGPin>>& Pins)
	{
		TArray<FString> Labels;
		for (const UPCGPin* Pin : Pins)
		{
			if (Pin && !OverrideOf(*Pin))
				Labels.Add(Pin->Properties.Label.ToString());
		}
		return Labels.Num() ? FString::Join(Labels, TEXT(", ")) : FString(TEXT("none"));
	}

	bool IsMathStruct(const UScriptStruct* Struct)
	{
		return Struct == TBaseStructure<FVector>::Get() || Struct == TBaseStructure<FVector2D>::Get() || Struct == TBaseStructure<FVector4>::Get()
			|| Struct == TBaseStructure<FRotator>::Get() || Struct == TBaseStructure<FLinearColor>::Get() || Struct == TBaseStructure<FIntPoint>::Get()
			|| Struct == TBaseStructure<FIntVector>::Get() || Struct == TBaseStructure<FQuat>::Get();
	}

	/** Structs read and written as one value (math, selectors, constants, thresholds, paths); others list their members. */
	bool IsValueStruct(const UScriptStruct* Struct)
	{
		return !Struct || IsMathStruct(Struct) || Struct->IsChildOf(FPCGAttributePropertySelector::StaticStruct())
			|| Struct == FPCGMetadataTypesConstantStruct::StaticStruct() || Struct == FPCGAttributeFilterThresholdSettings::StaticStruct()
			|| Struct->GetOutermost()->GetName() == TEXT("/Script/CoreUObject");
	}

	bool IsInstancedObject(const FProperty* Property)
	{
		return CastField<FObjectProperty>(Property) && Property->HasAnyPropertyFlags(CPF_InstancedReference);
	}

	/** A setting agents may change: editable, not deprecated, not debug / GPU / asset-info plumbing (Enabled stays). */
	bool IsSetting(const FProperty* Property, bool bTopLevel)
	{
		// An instanced object (a mesh selector) is VisibleAnywhere: the object stays, its members are the settings.
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Deprecated)
			|| (Property->HasAnyPropertyFlags(CPF_EditConst) && !IsInstancedObject(Property)))
			return false;
		if (!bTopLevel)
			return true;
		const FString Category = Property->GetMetaData(TEXT("Category"));
		if (Property->GetFName() == TEXT("bEnabled"))
			return true;
		return !(Category == TEXT("Debug") || Category == TEXT("GPU") || Category == TEXT("AssetInfo"));
	}

	/** A setting as a path from the settings object: properties, crossing structs and instanced objects. */
	struct FSettingPath
	{
		TArray<FProperty*> Chain;
		FString Name;
	};

	/** The settings a node lists: top-level settings, the members of structs and instanced objects one level down. */
	TArray<FSettingPath> ListSettings(const UPCGSettings& Settings)
	{
		TArray<FSettingPath> Out;
		for (TFieldIterator<FProperty> It(Settings.GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsSetting(Property, true))
				continue;
			const FStructProperty* Struct = CastField<FStructProperty>(Property);
			const UStruct* Inner = nullptr;
			if (Struct && !IsValueStruct(Struct->Struct))
				Inner = Struct->Struct;
			else if (IsInstancedObject(Property))
			{
				const UObject* Object = CastField<FObjectProperty>(Property)->GetObjectPropertyValue_InContainer(&Settings);
				Inner = Object ? Object->GetClass() : nullptr;
			}
			if (!Inner)
			{
				Out.Add({ { Property }, Property->GetName() });
				continue;
			}
			for (TFieldIterator<FProperty> Child(Inner); Child; ++Child)
			{
				if (IsSetting(*Child, false))
					Out.Add({ { Property, *Child }, Property->GetName() + TEXT(".") + Child->GetName() });
			}
		}
		return Out;
	}

	/** The address of the value @p Path ends at, walking structs and instanced objects; the objects it passes are in @p Objects. */
	void* Resolve(UPCGSettings& Settings, const TArray<FProperty*>& Path, TArray<UObject*>& Objects)
	{
		void* Container = &Settings;
		Objects = { &Settings };
		for (int32 Index = 0; Index < Path.Num(); ++Index)
		{
			void* Value = Path[Index]->ContainerPtrToValuePtr<void>(Container);
			if (Index == Path.Num() - 1)
				return Value;
			if (CastField<FStructProperty>(Path[Index]))
				Container = Value;
			else if (const FObjectProperty* Object = CastField<FObjectProperty>(Path[Index]))
			{
				UObject* Inner = Object->GetObjectPropertyValue(Value);
				if (!Inner)
					return nullptr;
				Objects.Add(Inner);
				Container = Inner;
			}
			else
				return nullptr;
		}
		return nullptr;
	}

	/** A property name matches with or without its bool prefix: "bInvertFilter" is "InvertFilter" too. */
	bool PropertyNameMatches(const FString& Name, const FString& Wanted)
	{
		return Normalize(Name) == Wanted || (Name.StartsWith(TEXT("b")) && Name.Len() > 1 && FChar::IsUpper(Name[1]) && Normalize(Name.Mid(1)) == Wanted);
	}

	bool NameMatches(const FProperty& Property, const FString& Wanted)
	{
		return PropertyNameMatches(Property.GetName(), Wanted) || Normalize(Property.GetDisplayNameText().ToString()) == Wanted;
	}

	/** The property of @p Owner a dotted path segment names. */
	FProperty* FindChild(const UStruct* Owner, const FString& Segment)
	{
		const FString Wanted = Normalize(Segment);
		for (TFieldIterator<FProperty> It(Owner); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Edit) && NameMatches(**It, Wanted))
				return *It;
		}
		return nullptr;
	}

	/** The setting @p Name names: a listed one (by its path or its last part), or a dotted path through structs and objects. */
	bool FindSetting(UPCGSettings& Settings, const FString& Name, FSettingPath& Out)
	{
		const FString Wanted = Normalize(Name);
		const TArray<FSettingPath> Listed = ListSettings(Settings);
		TArray<const FSettingPath*> ByLast;
		for (const FSettingPath& Setting : Listed)
		{
			if (Normalize(Setting.Name) == Wanted)
				return (Out = Setting), true;
			if (NameMatches(*Setting.Chain.Last(), Wanted))
				ByLast.Add(&Setting);
		}
		if (ByLast.Num() == 1)
			return (Out = *ByLast[0]), true;
		if (ByLast.Num() > 1)
		{
			TArray<FString> Names;
			for (const FSettingPath* Setting : ByLast)
				Names.Add(Setting->Name);
			return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("'%s' is ambiguous: %s"), *Name, *FString::Join(Names, TEXT(", "))));
		}
		// A deeper dotted path ("SamplerParams.Dimension" is listed; "ActorSelector.ActorFilter" too; anything below walks).
		TArray<FString> Segments;
		Name.ParseIntoArray(Segments, TEXT("."));
		const UStruct* Owner = Settings.GetClass();
		const void* Container = &Settings;
		FSettingPath Walked;
		for (int32 Index = 0; Index < Segments.Num() && Owner; ++Index)
		{
			FProperty* Property = FindChild(Owner, Segments[Index]);
			if (!Property)
			{
				Walked.Chain.Reset();
				break;
			}
			Walked.Chain.Add(Property);
			const void* Value = Property->ContainerPtrToValuePtr<void>(Container);
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
				Owner = Struct->Struct, Container = Value;
			else if (const FObjectProperty* Object = CastField<FObjectProperty>(Property); Object && IsInstancedObject(Property))
			{
				const UObject* Inner = Object->GetObjectPropertyValue(Value);
				Owner = Inner ? Inner->GetClass() : nullptr;
				Container = Inner;
			}
			else
				Owner = Index + 1 < Segments.Num() ? nullptr : Owner;
		}
		if (Walked.Chain.Num() == Segments.Num() && Segments.Num() > 0)
		{
			TArray<FString> Parts;
			for (const FProperty* Property : Walked.Chain)
				Parts.Add(Property->GetName());
			Walked.Name = FString::Join(Parts, TEXT("."));
			return (Out = Walked), true;
		}
		TArray<FString> Names;
		for (const FSettingPath& Setting : Listed)
			Names.Add(Setting.Name);
		return UnrealMcpRql::Failed<bool>(FString::Printf(TEXT("%s has no setting '%s' (settings: %s)"), *UnrealAIPCGCatalog::TypeName(Settings.GetClass()), *Name,
			*FString::Join(Names, TEXT(", "))));
	}

	/** The override pin label of a setting path, when the node has one. */
	FString OverridePinOf(const UPCGSettings& Settings, const TArray<FProperty*>& Chain)
	{
		for (const FPCGSettingsOverridableParam& Param : Settings.OverridableParams())
		{
			if (Param.PropertiesNames.Num() != Chain.Num())
				continue;
			bool bSame = true;
			for (int32 Index = 0; Index < Chain.Num() && bSame; ++Index)
				bSame = Param.PropertiesNames[Index] == Chain[Index]->GetFName();
			if (bSame)
				return Param.Label.ToString();
		}
		return FString();
	}

	const UEnum* EnumOf(const FProperty& Property)
	{
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property))
			return Enum->GetEnum();
		if (const FByteProperty* Byte = CastField<FByteProperty>(&Property))
			return Byte->Enum;
		return nullptr;
	}

	/** The entries of an enum agents may pick (hidden ones and the _MAX entry left out). */
	TArray<int32> EnumEntries(const UEnum& Enum)
	{
		TArray<int32> Out;
		for (int32 Index = 0; Index < Enum.NumEnums(); ++Index)
		{
			if (Enum.HasMetaData(TEXT("Hidden"), Index) || (Index == Enum.NumEnums() - 1 && Enum.GetNameStringByIndex(Index).EndsWith(TEXT("MAX"))))
				continue;
			Out.Add(Index);
		}
		return Out;
	}

	bool ParseNumbers(const FString& Text, TArray<double>& Out)
	{
		FString Inner = Text.TrimStartAndEnd();
		if (Inner.StartsWith(TEXT("(")) && Inner.EndsWith(TEXT(")")))
			Inner = Inner.Mid(1, Inner.Len() - 2);
		else if (Inner.StartsWith(TEXT("[")) && Inner.EndsWith(TEXT("]")))
			Inner = Inner.Mid(1, Inner.Len() - 2);
		TArray<FString> Parts;
		Inner.ParseIntoArray(Parts, TEXT(","));
		for (FString& Part : Parts)
		{
			// "X=1" (UE text) is a number too.
			FString Value = Part.TrimStartAndEnd();
			FString Left, Right;
			if (Value.Split(TEXT("="), &Left, &Right))
				Value = Right.TrimStartAndEnd();
			double Number = 0.0;
			if (!LexTryParseString(Number, *Value))
				return false;
			Out.Add(Number);
		}
		return Out.Num() > 0;
	}

	FString Number(double Value)
	{
		return FString::SanitizeFloat(Value, 0);
	}

	/** "(x, y, z)" (one number fills every component), "#rrggbb" for colours. */
	bool ParseMath(const UScriptStruct* Struct, void* Address, const FString& Text, FString& Error)
	{
		if (Struct == TBaseStructure<FLinearColor>::Get() && Text.TrimStart().StartsWith(TEXT("#")))
		{
			*static_cast<FLinearColor*>(Address) = FLinearColor(FColor::FromHex(Text.TrimStartAndEnd()));
			return true;
		}
		const int32 Count = Struct == TBaseStructure<FVector2D>::Get() || Struct == TBaseStructure<FIntPoint>::Get() ? 2
			: Struct == TBaseStructure<FVector4>::Get() || Struct == TBaseStructure<FLinearColor>::Get() || Struct == TBaseStructure<FQuat>::Get() ? 4 : 3;
		TArray<double> Values;
		if (!ParseNumbers(Text, Values) || (Values.Num() != 1 && Values.Num() != Count && !(Count == 4 && Values.Num() == 3)))
		{
			Error = FString::Printf(TEXT("takes %d numbers \"(%s)\" or one number for all"), Count,
				Struct == TBaseStructure<FRotator>::Get() ? TEXT("pitch, yaw, roll") : Count == 2 ? TEXT("x, y") : Count == 4 ? TEXT("x, y, z, w") : TEXT("x, y, z"));
			return false;
		}
		const auto At = [&Values](int32 Index, double Fallback) { return Values.Num() == 1 ? Values[0] : Values.IsValidIndex(Index) ? Values[Index] : Fallback; };
		if (Struct == TBaseStructure<FVector>::Get())
			*static_cast<FVector*>(Address) = FVector(At(0, 0), At(1, 0), At(2, 0));
		else if (Struct == TBaseStructure<FVector2D>::Get())
			*static_cast<FVector2D*>(Address) = FVector2D(At(0, 0), At(1, 0));
		else if (Struct == TBaseStructure<FVector4>::Get())
			*static_cast<FVector4*>(Address) = FVector4(At(0, 0), At(1, 0), At(2, 0), At(3, 1));
		else if (Struct == TBaseStructure<FRotator>::Get())
			*static_cast<FRotator*>(Address) = FRotator(At(0, 0), At(1, 0), At(2, 0));
		else if (Struct == TBaseStructure<FLinearColor>::Get())
			*static_cast<FLinearColor*>(Address) = FLinearColor(At(0, 0), At(1, 0), At(2, 0), At(3, 1));
		else if (Struct == TBaseStructure<FIntPoint>::Get())
			*static_cast<FIntPoint*>(Address) = FIntPoint(FMath::RoundToInt(At(0, 0)), FMath::RoundToInt(At(1, 0)));
		else if (Struct == TBaseStructure<FIntVector>::Get())
			*static_cast<FIntVector*>(Address) = FIntVector(FMath::RoundToInt(At(0, 0)), FMath::RoundToInt(At(1, 0)), FMath::RoundToInt(At(2, 0)));
		else if (Struct == TBaseStructure<FQuat>::Get())
			*static_cast<FQuat*>(Address) = Values.Num() == 3 ? FQuat(FRotator(At(0, 0), At(1, 0), At(2, 0))) : FQuat(At(0, 0), At(1, 0), At(2, 0), At(3, 1));
		return true;
	}

	FString FormatMath(const UScriptStruct* Struct, const void* Address)
	{
		TArray<double> Values;
		if (Struct == TBaseStructure<FVector>::Get())
		{
			const FVector& V = *static_cast<const FVector*>(Address);
			Values = { V.X, V.Y, V.Z };
		}
		else if (Struct == TBaseStructure<FVector2D>::Get())
		{
			const FVector2D& V = *static_cast<const FVector2D*>(Address);
			Values = { V.X, V.Y };
		}
		else if (Struct == TBaseStructure<FVector4>::Get())
		{
			const FVector4& V = *static_cast<const FVector4*>(Address);
			Values = { V.X, V.Y, V.Z, V.W };
		}
		else if (Struct == TBaseStructure<FRotator>::Get())
		{
			const FRotator& V = *static_cast<const FRotator*>(Address);
			Values = { V.Pitch, V.Yaw, V.Roll };
		}
		else if (Struct == TBaseStructure<FLinearColor>::Get())
		{
			const FLinearColor& V = *static_cast<const FLinearColor*>(Address);
			Values = { V.R, V.G, V.B, V.A };
		}
		else if (Struct == TBaseStructure<FIntPoint>::Get())
		{
			const FIntPoint& V = *static_cast<const FIntPoint*>(Address);
			Values = { double(V.X), double(V.Y) };
		}
		else if (Struct == TBaseStructure<FIntVector>::Get())
		{
			const FIntVector& V = *static_cast<const FIntVector*>(Address);
			Values = { double(V.X), double(V.Y), double(V.Z) };
		}
		else if (Struct == TBaseStructure<FQuat>::Get())
		{
			const FRotator R = static_cast<const FQuat*>(Address)->Rotator();
			Values = { R.Pitch, R.Yaw, R.Roll };
		}
		TArray<FString> Parts;
		for (const double Value : Values)
			Parts.Add(Number(Value));
		return TEXT("(") + FString::Join(Parts, TEXT(", ")) + TEXT(")");
	}

	/** A constant: true / false, a number (keeps an integer or float type), "(x, y[, z[, w]])", or text. */
	bool ParseConstant(FPCGMetadataTypesConstantStruct& Constant, const FString& InText, FString& Error)
	{
		const FString Text = InText.TrimStartAndEnd().TrimQuotes();
		const FString Lower = Text.ToLower();
		const bool bFree = Constant.bAllowsTypeChange;
		const auto Want = [&](EPCGMetadataTypes Type) -> bool
		{
			if (Constant.Type == Type)
				return true;
			if (!bFree)
			{
				Error = FString::Printf(TEXT("this constant is a %s"), *Constant.TypeToString());
				return false;
			}
			Constant.Type = Type;
			return true;
		};
		double Value = 0.0;
		TArray<double> Values;
		if (Lower == TEXT("true") || Lower == TEXT("false"))
			return Want(EPCGMetadataTypes::Boolean) && ((Constant.BoolValue = Lower == TEXT("true")), true);
		if (LexTryParseString(Value, *Text))
		{
			switch (Constant.Type)
			{
			case EPCGMetadataTypes::Float: Constant.FloatValue = static_cast<float>(Value); return true;
			case EPCGMetadataTypes::Integer32: Constant.Int32Value = static_cast<int32>(Value); return true;
			case EPCGMetadataTypes::Integer64: Constant.IntValue = static_cast<int64>(Value); return true;
			default: return Want(EPCGMetadataTypes::Double) && ((Constant.DoubleValue = Value), true);
			}
		}
		if (Text.StartsWith(TEXT("(")) && ParseNumbers(Text, Values))
		{
			if (Values.Num() == 2)
				return Want(EPCGMetadataTypes::Vector2) && ((Constant.Vector2Value = FVector2D(Values[0], Values[1])), true);
			if (Values.Num() == 3 && Constant.Type == EPCGMetadataTypes::Rotator)
				return (Constant.RotatorValue = FRotator(Values[0], Values[1], Values[2])), true;
			if (Values.Num() == 3)
				return Want(EPCGMetadataTypes::Vector) && ((Constant.VectorValue = FVector(Values[0], Values[1], Values[2])), true);
			if (Values.Num() == 4)
				return Want(EPCGMetadataTypes::Vector4) && ((Constant.Vector4Value = FVector4(Values[0], Values[1], Values[2], Values[3])), true);
		}
		if (Constant.Type == EPCGMetadataTypes::Name)
			return (Constant.NameValue = FName(*Text)), true;
		return Want(EPCGMetadataTypes::String) && ((Constant.StringValue = Text), true);
	}

	/** Whether text names an attribute ("$Position.Z", "Density", "@Last") rather than a constant. */
	bool IsAttributeText(const FString& Text)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		double Number = 0.0;
		if (Trimmed.IsEmpty() || LexTryParseString(Number, *Trimmed) || Trimmed.StartsWith(TEXT("(")) || Trimmed.StartsWith(TEXT("\"")))
			return false;
		const FString Lower = Trimmed.ToLower();
		return Lower != TEXT("true") && Lower != TEXT("false");
	}

	FString FormatThreshold(const FPCGAttributeFilterThresholdSettings& Threshold)
	{
		return Threshold.bUseConstantThreshold ? Threshold.AttributeTypes.ToString() : Threshold.ThresholdAttribute.ToString();
	}

	bool IsWeightedEntries(const FProperty& Property)
	{
		const FArrayProperty* Array = CastField<FArrayProperty>(&Property);
		const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
		return Inner && Inner->Struct == FPCGMeshSelectorWeightedEntry::StaticStruct();
	}

	/** "/Game/Trees/SM_Pine: 3, /Game/Trees/SM_Birch" (weight 1 when left out). */
	bool ParseMeshEntries(TArray<FPCGMeshSelectorWeightedEntry>& Entries, const FString& Text, FString& Error)
	{
		TArray<FString> Items;
		Text.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(Items, TEXT(","));
		TArray<FPCGMeshSelectorWeightedEntry> Out;
		for (const FString& Item : Items)
		{
			FString Path = Item.TrimStartAndEnd().TrimQuotes(), WeightText;
			int32 Weight = 1;
			// "path: weight" (an object path has no ':' after its package path; "/Game/A.A:Sub" is not a mesh).
			if (Path.Split(TEXT(":"), &Path, &WeightText, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				Path.TrimStartAndEndInline();
				if (!LexTryParseString(Weight, *WeightText.TrimStartAndEnd()) || Weight < 0)
				{
					Error = FString::Printf(TEXT("'%s' is not a weight (a whole number >= 0) in '%s'"), *WeightText.TrimStartAndEnd(), *Item.TrimStartAndEnd());
					return false;
				}
			}
			UStaticMesh* Mesh = Cast<UStaticMesh>(UnrealMcpRql::LoadObject(UStaticMesh::StaticClass(), Path));
			if (!Mesh)
			{
				Error = FString::Printf(TEXT("no StaticMesh at '%s' (unreal.assets(\"StaticMesh\", \"/Game\") lists the project's)"), *Path);
				return false;
			}
			Out.Emplace(TSoftObjectPtr<UStaticMesh>(Mesh), Weight);
		}
		if (Out.Num() == 0)
		{
			Error = TEXT("takes meshes with optional weights: \"/Game/Trees/SM_Pine: 3, /Game/Trees/SM_Birch: 1\"");
			return false;
		}
		Entries = MoveTemp(Out);
		return true;
	}

	FString FormatMeshEntries(const TArray<FPCGMeshSelectorWeightedEntry>& Entries)
	{
		TArray<FString> Parts;
		for (const FPCGMeshSelectorWeightedEntry& Entry : Entries)
			Parts.Add(FString::Printf(TEXT("%s: %d"), *Entry.Descriptor.StaticMesh.ToSoftObjectPath().GetLongPackageName(), Entry.Weight));
		return Parts.Num() ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("(none)"));
	}

	/** The editor graph of an asset the PCG graph editor opened in this session (transient, rebuilt only when its schema changes). */
	UEdGraph* EditorGraphOf(UPCGGraph& Graph)
	{
		UEdGraph* Found = nullptr;
		ForEachObjectWithOuter(&Graph, [&Found](UObject* Object)
		{
			if (UEdGraph* EdGraph = Cast<UEdGraph>(Object); EdGraph && !Found)
				Found = EdGraph;
		}, /*bIncludeNestedObjects*/ false);
		return Found;
	}
}

FString UnrealAIPCG::Normalize(const FString& Name)
{
	FString Out = Name.ToLower();
	Out.ReplaceInline(TEXT(" "), TEXT(""));
	Out.ReplaceInline(TEXT("_"), TEXT(""));
	Out.ReplaceInline(TEXT("-"), TEXT(""));
	return Out;
}

FString UnrealAIPCG::NodeTitle(const UPCGNode& Node)
{
	const UPCGGraph* Graph = Node.GetGraph();
	if (Graph && &Node == Graph->GetInputNode())
		return TEXT("Input");
	if (Graph && &Node == Graph->GetOutputNode())
		return TEXT("Output");
	return Node.GetNodeTitle(EPCGNodeTitleType::ListView).ToString();
}

UPCGNode* UnrealAIPCG::FindNode(UPCGGraph& Graph, const FString& Name)
{
	using namespace UnrealAIPCGGraphEditLocal;
	const FString Wanted = Normalize(Name);
	const TArray<UPCGNode*> Nodes = AllNodesOf(Graph);
	// The node's Name (unique) wins over titles (several nodes can share one).
	for (UPCGNode* Node : Nodes)
	{
		if (Normalize(Node->GetName()) == Wanted)
			return Node;
	}
	TArray<UPCGNode*> Matches;
	for (UPCGNode* Node : Nodes)
	{
		if (Normalize(NodeTitle(*Node)) == Wanted || Normalize(Node->GetDefaultTitle().ToString()) == Wanted
			|| (Node->GetSettings() && Normalize(UnrealAIPCGCatalog::TypeName(Node->GetSettings()->GetClass())) == Wanted))
			Matches.AddUnique(Node);
	}
	if (Matches.Num() == 1)
		return Matches[0];
	TArray<FString> Labels;
	for (const UPCGNode* Node : Matches.Num() ? Matches : Nodes)
		Labels.Add(NodeLabel(*Node));
	return UnrealMcpRql::Failed<UPCGNode*>(Matches.Num()
		? FString::Printf(TEXT("%d nodes of %s match '%s': %s; pass the Name"), Matches.Num(), *Graph.GetName(), *Name, *FString::Join(Labels, TEXT(", ")))
		: FString::Printf(TEXT("%s has no node '%s' (nodes: %s)"), *Graph.GetName(), *Name, *FString::Join(Labels, TEXT(", "))));
}

UPCGPin* UnrealAIPCG::FindPin(UPCGNode& Node, const FString& Label, bool bOutput)
{
	using namespace UnrealAIPCGGraphEditLocal;
	const TArray<TObjectPtr<UPCGPin>>& Pins = bOutput ? Node.GetOutputPins() : Node.GetInputPins();
	const FString Wanted = Normalize(Label);
	for (UPCGPin* Pin : Pins)
	{
		if (Pin && (Wanted.IsEmpty() ? !OverrideOf(*Pin) : Normalize(Pin->Properties.Label.ToString()) == Wanted))
			return Pin;
	}
	// An override pin by the setting it sets ("PointsPerSquaredMeter" for "Points Per Squared Meter").
	if (!bOutput && !Wanted.IsEmpty())
	{
		if (UPCGPin* Pin = FindOverridePin(Node, Label))
			return Pin;
	}
	return UnrealMcpRql::Failed<UPCGPin*>(FString::Printf(TEXT("%s has no %s pin%s (%s pins: %s)"), *NodeLabel(Node), bOutput ? TEXT("output") : TEXT("input"),
		Label.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" '%s'"), *Label), bOutput ? TEXT("output") : TEXT("input"), *PinLabels(Pins)));
}

UPCGPin* UnrealAIPCG::FindOverridePin(UPCGNode& Node, const FString& Setting)
{
	using namespace UnrealAIPCGGraphEditLocal;
	const UPCGSettings* Settings = Node.GetSettings();
	if (!Settings)
		return nullptr;
	const FString Wanted = Normalize(Setting);
	for (const FPCGSettingsOverridableParam& Param : Settings->OverridableParams())
	{
		const FString Last = Param.PropertiesNames.Num() ? Param.PropertiesNames.Last().ToString() : FString();
		const bool bMatch = Normalize(Param.Label.ToString()) == Wanted || PropertyNameMatches(Last, Wanted) || Normalize(SettingPathOf(Param)) == Wanted;
		if (bMatch)
		{
			if (UPCGPin* Pin = Node.GetInputPin(Param.Label))
				return Pin;
		}
	}
	return nullptr;
}

TArray<UPCGNode*> UnrealAIPCG::AllNodes(const UPCGGraph& Graph)
{
	return UnrealAIPCGGraphEditLocal::AllNodesOf(Graph);
}

FString UnrealAIPCG::SettingPath(const FPCGSettingsOverridableParam& Param)
{
	return UnrealAIPCGGraphEditLocal::SettingPathOf(Param);
}

FString UnrealAIPCG::PinType(const UPCGPin& Pin)
{
	return Pin.GetCurrentTypesID().ToDisplayText().ToString();
}

FString UnrealAIPCG::PinPath(const UPCGPin& Pin)
{
	return FString::Printf(TEXT("%s.%s"), Pin.Node ? *UnrealAIPCGGraphEditLocal::NodeName(*Pin.Node) : TEXT("?"), *Pin.Properties.Label.ToString());
}

TSharedPtr<FJsonValue> UnrealAIPCG::PinsJson(const UPCGNode& Node, bool bOutput)
{
	using namespace UnrealAIPCGGraphEditLocal;
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const UPCGPin* Pin : bOutput ? Node.GetOutputPins() : Node.GetInputPins())
	{
		const FPCGSettingsOverridableParam* Override = Pin ? OverrideOf(*Pin) : nullptr;
		// Unconnected override pins are noise (a node has one per overridable setting): the Settings member names them.
		if (!Pin || (Override && !Pin->IsConnected()))
			continue;
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Pin"), Pin->Properties.Label.ToString());
		Row->SetStringField(TEXT("Type"), PinType(*Pin));
		TArray<TSharedPtr<FJsonValue>> Connections;
		for (const UPCGEdge* Edge : Pin->Edges)
		{
			const UPCGPin* Other = Edge ? (bOutput ? Edge->OutputPin.Get() : Edge->InputPin.Get()) : nullptr;
			if (Other)
				Connections.Add(MakeShared<FJsonValueString>(PinPath(*Other)));
		}
		Row->SetArrayField(TEXT("Connections"), Connections);
		if (Override)
			Row->SetStringField(TEXT("Override"), SettingPathOf(*Override));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return MakeShared<FJsonValueArray>(Rows);
}

TSharedPtr<FJsonValue> UnrealAIPCG::SettingsJson(const UPCGNode& Node)
{
	using namespace UnrealAIPCGGraphEditLocal;
	TArray<TSharedPtr<FJsonValue>> Rows;
	UPCGSettings* Settings = Node.GetSettings();
	if (!Settings)
		return MakeShared<FJsonValueArray>(Rows);
	for (const FSettingPath& Setting : ListSettings(*Settings))
	{
		TArray<UObject*> Objects;
		const void* Address = Resolve(*Settings, Setting.Chain, Objects);
		if (!Address)
			continue;
		const FProperty& Leaf = *Setting.Chain.Last();
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), Setting.Name);
		FString Type = Leaf.GetCPPType();
		if (const UEnum* Enum = EnumOf(Leaf))
		{
			TArray<FString> Entries;
			for (const int32 Index : EnumEntries(*Enum))
				Entries.Add(Enum->GetNameStringByIndex(Index));
			Type = FString::Printf(TEXT("%s (%s)"), *Enum->GetName(), *FString::Join(Entries, TEXT(", ")));
		}
		else if (IsWeightedEntries(Leaf))
			Type = TEXT("meshes \"/Game/SM_A: 3, /Game/SM_B: 1\"");
		Row->SetStringField(TEXT("Type"), Type);
		FString Value = FormatValue(Leaf, Address);
		if (Value.Len() > 300)
			Value = Value.Left(297) + TEXT("...");
		Row->SetStringField(TEXT("Value"), Value);
		const FString Pin = OverridePinOf(*Settings, Setting.Chain);
		if (!Pin.IsEmpty())
			Row->SetStringField(TEXT("Pin"), Pin);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return MakeShared<FJsonValueArray>(Rows);
}

bool UnrealAIPCG::ParseValue(const FProperty& Property, void* Address, const FString& InText, FString& Error)
{
	using namespace UnrealAIPCGGraphEditLocal;
	const FString Text = InText.TrimStartAndEnd();
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
	{
		const FString Lower = Text.ToLower();
		if (Lower == TEXT("true") || Lower == TEXT("1") || Lower == TEXT("yes") || Lower == TEXT("on"))
			return Bool->SetPropertyValue(Address, true), true;
		if (Lower == TEXT("false") || Lower == TEXT("0") || Lower == TEXT("no") || Lower == TEXT("off"))
			return Bool->SetPropertyValue(Address, false), true;
		Error = TEXT("takes true or false");
		return false;
	}
	if (const UEnum* Enum = EnumOf(Property))
	{
		const FString Wanted = Normalize(Text.Contains(TEXT("::")) ? Text.RightChop(Text.Find(TEXT("::")) + 2) : Text);
		int64 Value = 0;
		bool bFound = false;
		TArray<FString> Names;
		for (const int32 Index : EnumEntries(*Enum))
		{
			Names.Add(Enum->GetNameStringByIndex(Index));
			if (!bFound && (Normalize(Enum->GetNameStringByIndex(Index)) == Wanted || Normalize(Enum->GetDisplayNameTextByIndex(Index).ToString()) == Wanted))
				Value = Enum->GetValueByIndex(Index), bFound = true;
		}
		if (!bFound && !LexTryParseString(Value, *Text))
		{
			Error = FString::Printf(TEXT("'%s' is not an entry of %s (entries: %s)"), *Text, *Enum->GetName(), *FString::Join(Names, TEXT(", ")));
			return false;
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Address, Value);
		else
			CastField<FByteProperty>(&Property)->SetIntPropertyValue(Address, Value);
		return true;
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
	{
		double Value = 0.0;
		if (!LexTryParseString(Value, *Text))
		{
			Error = FString::Printf(TEXT("takes a number, not '%s'"), *Text);
			return false;
		}
		if (Numeric->IsFloatingPoint())
			Numeric->SetFloatingPointPropertyValue(Address, Value);
		else
			Numeric->SetIntPropertyValue(Address, static_cast<int64>(FMath::RoundToDouble(Value)));
		return true;
	}
	if (const FStrProperty* String = CastField<FStrProperty>(&Property))
		return String->SetPropertyValue(Address, Text.TrimQuotes()), true;
	if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
		return Name->SetPropertyValue(Address, FName(*Text.TrimQuotes())), true;
	if (const FTextProperty* TextProperty = CastField<FTextProperty>(&Property))
		return TextProperty->SetPropertyValue(Address, FText::FromString(Text.TrimQuotes())), true;
	if (const FClassProperty* Class = CastField<FClassProperty>(&Property))
	{
		UClass* Value = Text.IsEmpty() || Normalize(Text) == TEXT("none") ? nullptr : UnrealMcpRql::ResolveClass(Text);
		if (Value ? !Value->IsChildOf(Class->MetaClass) : !(Text.IsEmpty() || Normalize(Text) == TEXT("none")))
		{
			Error = FString::Printf(TEXT("takes a %s class (unreal.classes(\"%s\") lists them)"), *Class->MetaClass->GetName(), *Class->MetaClass->GetName());
			return false;
		}
		Class->SetObjectPropertyValue(Address, Value);
		return true;
	}
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
	{
		if (Property.HasAnyPropertyFlags(CPF_InstancedReference))
		{
			Error = TEXT("is an object the node owns: set its members (Setting.Member)");
			return false;
		}
		const bool bNone = Text.IsEmpty() || Normalize(Text) == TEXT("none");
		UObject* Value = bNone ? nullptr : UnrealMcpRql::LoadObject(Object->PropertyClass, Text.TrimQuotes());
		if (!Value && !bNone)
		{
			Error = FString::Printf(TEXT("no %s at '%s'"), *Object->PropertyClass->GetName(), *Text);
			return false;
		}
		if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
			Soft->SetPropertyValue(Address, FSoftObjectPtr(Value));
		else
			Object->SetObjectPropertyValue(Address, Value);
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		const UScriptStruct* Type = Struct->Struct;
		if (Type->IsChildOf(FPCGAttributePropertySelector::StaticStruct()))
		{
			static_cast<FPCGAttributePropertySelector*>(Address)->Update(Text.TrimQuotes());
			return true;
		}
		if (Type == FPCGMetadataTypesConstantStruct::StaticStruct())
			return ParseConstant(*static_cast<FPCGMetadataTypesConstantStruct*>(Address), Text, Error);
		if (Type == FPCGAttributeFilterThresholdSettings::StaticStruct())
		{
			FPCGAttributeFilterThresholdSettings& Threshold = *static_cast<FPCGAttributeFilterThresholdSettings*>(Address);
			Threshold.bUseConstantThreshold = !IsAttributeText(Text);
			if (!Threshold.bUseConstantThreshold)
				return Threshold.ThresholdAttribute.Update(Text), true;
			return ParseConstant(Threshold.AttributeTypes, Text, Error);
		}
		if (IsMathStruct(Type) && !Text.Contains(TEXT("=")))
			return ParseMath(Type, Address, Text, Error);
	}
	if (IsWeightedEntries(Property))
		return ParseMeshEntries(*static_cast<TArray<FPCGMeshSelectorWeightedEntry>*>(Address), Text, Error);
	// Anything else: UE's text form (what the editor's Copy writes), wholly consumed.
	FStringOutputDevice Errors;
	const TCHAR* End = Property.ImportText_Direct(*Text, Address, nullptr, PPF_None, &Errors);
	if (!End || !FString(End).TrimStartAndEnd().IsEmpty())
	{
		FString Example;
		Property.ExportText_Direct(Example, Address, Address, nullptr, PPF_None);
		Error = FString::Printf(TEXT("'%s' is not a %s value (UE text such as %s)%s"), *Text, *Property.GetCPPType(), *Example.Left(120),
			Errors.IsEmpty() ? TEXT("") : *(TEXT(": ") + Errors));
		return false;
	}
	return true;
}

FString UnrealAIPCG::FormatValue(const FProperty& Property, const void* Address)
{
	using namespace UnrealAIPCGGraphEditLocal;
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		return Bool->GetPropertyValue(Address) ? TEXT("true") : TEXT("false");
	if (const UEnum* Enum = EnumOf(Property))
	{
		const int64 Value = CastField<FEnumProperty>(&Property) ? CastField<FEnumProperty>(&Property)->GetUnderlyingProperty()->GetSignedIntPropertyValue(Address)
			: CastField<FByteProperty>(&Property)->GetPropertyValue(Address);
		return Enum->GetNameStringByValue(Value);
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		return Numeric->IsFloatingPoint() ? Number(Numeric->GetFloatingPointPropertyValue(Address)) : Numeric->GetNumericPropertyValueToString(Address);
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		const UScriptStruct* Type = Struct->Struct;
		if (Type->IsChildOf(FPCGAttributePropertySelector::StaticStruct()))
			return static_cast<const FPCGAttributePropertySelector*>(Address)->ToString();
		if (Type == FPCGMetadataTypesConstantStruct::StaticStruct())
			return static_cast<const FPCGMetadataTypesConstantStruct*>(Address)->ToString();
		if (Type == FPCGAttributeFilterThresholdSettings::StaticStruct())
			return FormatThreshold(*static_cast<const FPCGAttributeFilterThresholdSettings*>(Address));
		if (IsMathStruct(Type))
			return FormatMath(Type, Address);
	}
	if (IsWeightedEntries(Property))
		return FormatMeshEntries(*static_cast<const TArray<FPCGMeshSelectorWeightedEntry>*>(Address));
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property); Object && !CastField<FSoftObjectProperty>(&Property))
	{
		const UObject* Value = Object->GetObjectPropertyValue(Address);
		return Value ? (Property.HasAnyPropertyFlags(CPF_InstancedReference) ? Value->GetClass()->GetName() : Value->GetPathName()) : FString(TEXT("None"));
	}
	FString Text;
	Property.ExportText_Direct(Text, Address, Address, nullptr, PPF_None);
	return Text;
}

FString UnrealAIPCG::SetSetting(UPCGNode& Node, const FString& Path, const FString& Value)
{
	using namespace UnrealAIPCGGraphEditLocal;
	UPCGSettings* Settings = Node.GetSettings();
	if (!Settings)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s has no settings"), *NodeLabel(Node)));
	FString Name = Path;
	UPCGStaticMeshSpawnerSettings* Spawner = Cast<UPCGStaticMeshSpawnerSettings>(Settings);
	if (Spawner && Normalize(Path) == TEXT("meshes"))
	{
		// The weighted mesh list: the spawner's default selector; switch to it when another one is set (the meshes checked first).
		TArray<FPCGMeshSelectorWeightedEntry> Entries;
		FString Error;
		if (!ParseMeshEntries(Entries, Value, Error))
			return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s.Meshes %s"), *NodeTitle(Node), *Error));
		if (!Cast<UPCGMeshSelectorWeighted>(Spawner->MeshSelectorParameters))
		{
			Spawner->Modify();
			Spawner->SetMeshSelectorType(UPCGMeshSelectorWeighted::StaticClass());
		}
		Name = TEXT("MeshSelectorParameters.MeshEntries");
	}
	FSettingPath Setting;
	if (!FindSetting(*Settings, Name, Setting))
		return FString();
	TArray<UObject*> Objects;
	void* Address = Resolve(*Settings, Setting.Chain, Objects);
	if (!Address)
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s: '%s' is inside an object the node does not have"), *NodeLabel(Node), *Setting.Name));
	FProperty& Leaf = *Setting.Chain.Last();
	// Parse into a copy of the current value first: text that does not parse changes nothing.
	void* Scratch = FMemory::Malloc(Leaf.GetSize(), Leaf.GetMinAlignment());
	Leaf.InitializeValue(Scratch);
	ON_SCOPE_EXIT { Leaf.DestroyValue(Scratch); FMemory::Free(Scratch); };
	Leaf.CopyCompleteValue(Scratch, Address);
	FString Error;
	if (!ParseValue(Leaf, Scratch, Value, Error))
		return UnrealMcpRql::Failed<FString>(FString::Printf(TEXT("%s.%s %s"), *NodeTitle(Node), *Setting.Name, *Error));

	// As the Details panel does: the chain's member property changes, the node and its graph hear of it.
	FEditPropertyChain Chain;
	for (FProperty* Property : Setting.Chain)
		Chain.AddTail(Property);
	Chain.SetActiveMemberPropertyNode(Setting.Chain[0]);
	Chain.SetActivePropertyNode(&Leaf);
	for (UObject* Object : Objects)
		Object->Modify();
	Settings->PreEditChange(Chain);
	Leaf.CopyCompleteValue(Address, Scratch);
	FPropertyChangedEvent Event(&Leaf, EPropertyChangeType::ValueSet);
	Event.SetActiveMemberProperty(Setting.Chain[0]);
	if (Objects.Num() > 1)
	{
		// The instanced object (a mesh selector) refreshes itself first (e.g. the mesh entries' display names).
		FPropertyChangedEvent Inner(&Leaf, EPropertyChangeType::ValueSet);
		Objects.Last()->PostEditChangeProperty(Inner);
	}
	FPropertyChangedChainEvent ChainEvent(Chain, Event);
	Settings->PostEditChangeChainProperty(ChainEvent);
	if (UPCGGraph* Graph = Node.GetGraph())
		Changed(*Graph);
	TArray<UObject*> After;
	const void* Written = Resolve(*Settings, Setting.Chain, After);
	return Written ? FormatValue(Leaf, Written) : FString();
}

TSharedPtr<FJsonValue> UnrealAIPCG::ParametersJson(const UPCGGraph& Graph)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	const FInstancedPropertyBag* Bag = Graph.GetUserParametersStruct();
	const UPropertyBag* Struct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
	const uint8* Memory = Bag ? Bag->GetValue().GetMemory() : nullptr;
	if (!Struct || !Memory)
		return MakeShared<FJsonValueArray>(Rows);
	for (const FPropertyBagPropertyDesc& Desc : Struct->GetPropertyDescs())
	{
		if (!Desc.CachedProperty)
			continue;
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), Desc.Name.ToString());
		Row->SetStringField(TEXT("Type"), Desc.CachedProperty->GetCPPType());
		Row->SetStringField(TEXT("Value"), FormatValue(*Desc.CachedProperty, Desc.CachedProperty->ContainerPtrToValuePtr<void>(Memory)));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return MakeShared<FJsonValueArray>(Rows);
}

TSharedPtr<FJsonValue> UnrealAIPCG::EdgesJson(const UPCGGraph& Graph)
{
	using namespace UnrealAIPCGGraphEditLocal;
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const UPCGNode* Node : AllNodesOf(Graph))
	{
		for (const UPCGPin* Pin : Node->GetOutputPins())
		{
			if (!Pin)
				continue;
			for (const UPCGEdge* Edge : Pin->Edges)
			{
				if (Edge && Edge->OutputPin)
					Rows.Add(MakeShared<FJsonValueString>(PinPath(*Pin) + TEXT(" -> ") + PinPath(*Edge->OutputPin)));
			}
		}
	}
	return MakeShared<FJsonValueArray>(Rows);
}

void UnrealAIPCG::Changed(UPCGGraph& Graph)
{
	using namespace UnrealAIPCGGraphEditLocal;
	Graph.MarkPackageDirty();
	if (!EditorGraphOf(Graph))
		return;
	// The PCG graph editor builds its view of a graph once per session and keeps it: after this call (once per graph,
	// however many edits a mutation makes), invalidate it (a schema change makes the editor rebuild the view on its next
	// open) and reopen an open editor so it shows the changes.
	static TSet<TWeakObjectPtr<UPCGGraph>> Pending;
	if (Pending.Num() == 0)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			for (const TWeakObjectPtr<UPCGGraph>& Weak : Pending)
			{
				UPCGGraph* Changed = Weak.Get();
				UEdGraph* EditorGraph = Changed ? EditorGraphOf(*Changed) : nullptr;
				if (!EditorGraph)
					continue;
				const bool bOpen = Editors && Editors->FindEditorsForAsset(Changed).Num() > 0;
				if (bOpen)
					Editors->CloseAllEditorsForAsset(Changed);
				EditorGraph->Schema = UEdGraphSchema::StaticClass();
				if (bOpen)
					Editors->OpenEditorForAsset(Changed);
			}
			Pending.Reset();
			return false;
		}));
	}
	Pending.Add(&Graph);
}

TArray<FString> UnrealAIPCG::FGenerated::MeshLines() const
{
	TArray<TPair<FString, int32>> Sorted = Meshes.Array();
	Sorted.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value != B.Value ? A.Value > B.Value : A.Key < B.Key; });
	TArray<FString> Out;
	for (const TPair<FString, int32>& Mesh : Sorted)
		Out.Add(FString::Printf(TEXT("%s: %d"), *Mesh.Key, Mesh.Value));
	return Out;
}

UnrealAIPCG::FGenerated UnrealAIPCG::CountGenerated(UPCGComponent& Component)
{
	FGenerated Out;
	const auto Count = [&Out](UPCGComponent& One)
	{
		One.ForEachConstManagedResource([&Out](const UPCGManagedResource* Resource)
		{
			if (const UPCGManagedISMComponent* Managed = Cast<UPCGManagedISMComponent>(Resource))
			{
				if (const UInstancedStaticMeshComponent* Instances = Managed->GetComponent())
				{
					const int32 Num = Instances->GetInstanceCount();
					Out.Instances += Num;
					++Out.Components;
					const UStaticMesh* Mesh = Instances->GetStaticMesh();
					Out.Meshes.FindOrAdd(Mesh ? Mesh->GetName() : FString(TEXT("None"))) += Num;
				}
			}
			else if (Cast<UPCGManagedComponent>(Resource))
				++Out.Components;
			else if (const UPCGManagedActors* Actors = Cast<UPCGManagedActors>(Resource))
				Out.Actors += Actors->GetConstGeneratedActors().Num();
		});
		for (const FPCGTaggedData& Data : One.GetGeneratedGraphOutput().TaggedData)
		{
			if (const UPCGBasePointData* Points = Cast<UPCGBasePointData>(Data.Data))
				Out.Points += Points->GetNumPoints();
		}
	};
	Count(Component);
	if (Component.IsPartitioned())
	{
		if (UPCGSubsystem* Subsystem = UPCGSubsystem::GetInstance(Component.GetWorld()))
			Subsystem->ForAllRegisteredLocalComponents(&Component, [&Count](UPCGComponent* Local) { if (Local) Count(*Local); });
	}
	return Out;
}

TSharedPtr<FJsonValue> UnrealAIPCG::GeneratedJson(UPCGComponent& Component)
{
	const FGenerated Generated = CountGenerated(Component);
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("Instances"), Generated.Instances);
	TArray<TSharedPtr<FJsonValue>> Meshes;
	for (const FString& Line : Generated.MeshLines())
		Meshes.Add(MakeShared<FJsonValueString>(Line));
	Row->SetArrayField(TEXT("Meshes"), Meshes);
	Row->SetNumberField(TEXT("Components"), Generated.Components);
	Row->SetNumberField(TEXT("Actors"), Generated.Actors);
	Row->SetNumberField(TEXT("Points"), Generated.Points);
	Row->SetBoolField(TEXT("IsGenerating"), Component.IsGenerating());
	return MakeShared<FJsonValueObject>(Row);
}
