// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

// --- FUnrealMcpToolCall accessors -----------------------------------------------------------------

FString FUnrealMcpToolCall::GetString(const FString& Key, const FString& Default) const
{
	FString Value;
	return Arguments->TryGetStringField(Key, Value) ? Value : Default;
}

int64 FUnrealMcpToolCall::GetInt(const FString& Key, int64 Default) const
{
	double Value;
	if (Arguments->TryGetNumberField(Key, Value))
		return static_cast<int64>(Value);
	return Default;
}

double FUnrealMcpToolCall::GetNumber(const FString& Key, double Default) const
{
	double Value;
	return Arguments->TryGetNumberField(Key, Value) ? Value : Default;
}

bool FUnrealMcpToolCall::GetBool(const FString& Key, bool Default) const
{
	bool Value;
	return Arguments->TryGetBoolField(Key, Value) ? Value : Default;
}

FVector FUnrealMcpToolCall::GetVector(const FString& Key, const FVector& Default) const
{
	const TSharedPtr<FJsonObject>* Obj;
	if (!Arguments->TryGetObjectField(Key, Obj) || !Obj->IsValid())
		return Default;

	FVector Result = Default;
	double Component;
	if ((*Obj)->TryGetNumberField(TEXT("x"), Component)) Result.X = Component;
	if ((*Obj)->TryGetNumberField(TEXT("y"), Component)) Result.Y = Component;
	if ((*Obj)->TryGetNumberField(TEXT("z"), Component)) Result.Z = Component;
	return Result;
}

FRotator FUnrealMcpToolCall::GetRotator(const FString& Key, const FRotator& Default) const
{
	const TSharedPtr<FJsonObject>* Obj;
	if (!Arguments->TryGetObjectField(Key, Obj) || !Obj->IsValid())
		return Default;

	FRotator Result = Default;
	double Component;
	if ((*Obj)->TryGetNumberField(TEXT("pitch"), Component)) Result.Pitch = Component;
	if ((*Obj)->TryGetNumberField(TEXT("yaw"), Component)) Result.Yaw = Component;
	if ((*Obj)->TryGetNumberField(TEXT("roll"), Component)) Result.Roll = Component;
	return Result;
}

// --- Schema building ------------------------------------------------------------------------------

namespace
{
	TSharedPtr<FJsonObject> MakeTypedSchema(const FString& JsonType, const FString& Description)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), JsonType);
		if (!Description.IsEmpty())
			Schema->SetStringField(TEXT("description"), Description);
		return Schema;
	}

	/** A {x,y,z: number} object schema (the §3.2 FVector mapping). */
	TSharedPtr<FJsonObject> MakeVectorSchema(const FString& Description)
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetObjectField(TEXT("x"), MakeTypedSchema(TEXT("number"), FString()));
		Props->SetObjectField(TEXT("y"), MakeTypedSchema(TEXT("number"), FString()));
		Props->SetObjectField(TEXT("z"), MakeTypedSchema(TEXT("number"), FString()));

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		if (!Description.IsEmpty())
			Schema->SetStringField(TEXT("description"), Description);
		Schema->SetObjectField(TEXT("properties"), Props);
		return Schema;
	}

	FString SerializeStable(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}
}

TSharedPtr<FJsonObject> FUnrealMcpRegisteredTool::BuildInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Required;

	for (const FUnrealMcpParamSpec& Param : Params)
	{
		TSharedPtr<FJsonObject> ParamSchema = Param.JsonType == TEXT("object") && Param.ObjectSchema.IsValid()
			? Param.ObjectSchema
			: MakeTypedSchema(Param.JsonType, Param.Description);

		Properties->SetObjectField(Param.Name, ParamSchema);
		if (Param.Requirement == EUnrealMcpParamRequirement::Required)
			Required.Add(MakeShared<FJsonValueString>(Param.Name));
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	if (Required.Num() > 0)
		Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

TSharedPtr<FJsonObject> FUnrealMcpRegisteredTool::ToDescriptorJson() const
{
	TSharedPtr<FJsonObject> Desc = MakeShared<FJsonObject>();
	Desc->SetStringField(TEXT("name"), Name);
	Desc->SetStringField(TEXT("title"), Title);
	Desc->SetStringField(TEXT("description"), Description);
	Desc->SetObjectField(TEXT("inputSchema"), BuildInputSchema());
	if (OutputSchema.IsValid())
		Desc->SetObjectField(TEXT("outputSchema"), OutputSchema);
	Desc->SetBoolField(TEXT("readOnlyHint"), bReadOnlyHint);
	Desc->SetBoolField(TEXT("destructiveHint"), bDestructiveHint);
	Desc->SetBoolField(TEXT("idempotentHint"), bIdempotentHint);
	Desc->SetBoolField(TEXT("openWorldHint"), bOpenWorldHint);
	Desc->SetBoolField(TEXT("enabled"), bEnabled);
	Desc->SetStringField(TEXT("extensionId"), ExtensionId);
	Desc->SetStringField(TEXT("schemaHash"), SchemaHash);
	return Desc;
}

// --- FUnrealMcpToolBuilder ------------------------------------------------------------------------

FUnrealMcpToolBuilder::FUnrealMcpToolBuilder(FUnrealMcpToolRegistry& InRegistry, const FString& InName)
	: Registry(InRegistry)
{
	Tool.Name = InName;
}

FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::Title(const FString& InTitle) { Tool.Title = InTitle; return *this; }
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::Description(const FString& InDescription) { Tool.Description = InDescription; return *this; }

FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::ParamString(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	Tool.Params.Add(FUnrealMcpParamSpec{ Name, TEXT("string"), Desc, Req, nullptr });
	return *this;
}
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::ParamInt(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	Tool.Params.Add(FUnrealMcpParamSpec{ Name, TEXT("integer"), Desc, Req, nullptr });
	return *this;
}
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::ParamNumber(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	Tool.Params.Add(FUnrealMcpParamSpec{ Name, TEXT("number"), Desc, Req, nullptr });
	return *this;
}
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::ParamBool(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	Tool.Params.Add(FUnrealMcpParamSpec{ Name, TEXT("boolean"), Desc, Req, nullptr });
	return *this;
}
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::ParamVector(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	FUnrealMcpParamSpec Spec{ Name, TEXT("object"), Desc, Req, MakeVectorSchema(Desc) };
	Tool.Params.Add(Spec);
	return *this;
}
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::ReadOnlyHint(bool bValue) { Tool.bReadOnlyHint = bValue; return *this; }
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::DestructiveHint(bool bValue) { Tool.bDestructiveHint = bValue; return *this; }
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::IdempotentHint(bool bValue) { Tool.bIdempotentHint = bValue; return *this; }
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::OpenWorldHint(bool bValue) { Tool.bOpenWorldHint = bValue; return *this; }
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::ExtensionId(const FString& InExtensionId) { Tool.ExtensionId = InExtensionId; return *this; }

void FUnrealMcpToolBuilder::Handle(FUnrealMcpToolHandler InHandler)
{
	Tool.Handler = MoveTemp(InHandler);
	Registry.Commit(MoveTemp(Tool));
}

// --- FUnrealMcpToolRegistry -----------------------------------------------------------------------

FUnrealMcpToolBuilder FUnrealMcpToolRegistry::Tool(const FString& Name)
{
	return FUnrealMcpToolBuilder(*this, Name);
}

FString FUnrealMcpToolRegistry::ComputeSchemaHash(const FUnrealMcpRegisteredTool& InTool)
{
	// Hash the canonicalized descriptor MINUS the enabled flag (§2.2). Reuse ToDescriptorJson and drop
	// "enabled" + "schemaHash" so the hash is stable regardless of those mutable fields.
	FUnrealMcpRegisteredTool Copy = InTool;
	Copy.SchemaHash.Empty();
	TSharedPtr<FJsonObject> Desc = Copy.ToDescriptorJson();
	Desc->RemoveField(TEXT("enabled"));
	Desc->RemoveField(TEXT("schemaHash"));

	const FString Canonical = SerializeStable(Desc);
	FSHA1 Sha;
	const FTCHARToUTF8 Utf8(*Canonical);
	Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Sha.Final();
	uint8 Digest[20];
	Sha.GetHash(Digest);
	return FString(TEXT("sha1:")) + BytesToHex(Digest, 20).ToLower();
}

void FUnrealMcpToolRegistry::Commit(FUnrealMcpRegisteredTool&& InTool)
{
	InTool.SchemaHash = ComputeSchemaHash(InTool);
	const FString Name = InTool.Name;
	Tools.Add(Name, MoveTemp(InTool));
	++Revision;
	UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] registered tool '%s' (revision %d)."), *Name, Revision);
}

TSharedPtr<FJsonObject> FUnrealMcpToolRegistry::BuildManifestJson() const
{
	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("type"), TEXT("tool-manifest"));
	Manifest->SetNumberField(TEXT("revision"), Revision);

	TArray<TSharedPtr<FJsonValue>> ToolArray;
	// Deterministic ordering by name keeps the manifest stable across runs (§5 ordering principle).
	TArray<FString> Names;
	Tools.GetKeys(Names);
	Names.Sort();
	for (const FString& Name : Names)
		ToolArray.Add(MakeShared<FJsonValueObject>(Tools[Name].ToDescriptorJson()));

	Manifest->SetArrayField(TEXT("tools"), ToolArray);
	return Manifest;
}

FUnrealMcpToolResult FUnrealMcpToolRegistry::Execute(const FString& Name, const FUnrealMcpToolCall& Call) const
{
	const FUnrealMcpRegisteredTool* Found = Tools.Find(Name);
	if (Found == nullptr || !Found->Handler)
		return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Unknown tool '%s'."), *Name));

	return Found->Handler(Call);
}
