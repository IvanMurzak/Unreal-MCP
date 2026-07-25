// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpDescriptorHash.h"
#include "UnrealMcpSchema.h"
#include "UnrealMcpValidation.h"

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
// The §3.2 scalar + vector schema builders now live in the shared FUnrealMcpSchema TU (UnrealMcpSchema.h),
// externally linked so the prompt registry + the editor tool families reuse the SAME definition.

TSharedPtr<FJsonObject> FUnrealMcpRegisteredTool::BuildInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Required;

	for (const FUnrealMcpParamSpec& Param : Params)
	{
		// A custom schema (object/array/exotic types built via the generic Param()) is used verbatim;
		// the simple scalar types fall back to a {type,description} schema.
		TSharedPtr<FJsonObject> ParamSchema = Param.ObjectSchema.IsValid()
			? Param.ObjectSchema
			: FUnrealMcpSchema::TypedSchema(Param.JsonType, Param.Description);

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
	// Dedicated SHORT skill description for the generated SKILL.md front-matter (§7). When a tool did not declare
	// one via the builder, fall back to the Title — a concise human label, NOT a truncation of the full
	// Description (the sidecar generator uses this verbatim for the YAML `description:`; the full Description still
	// becomes the SKILL.md body). Never empty so the front-matter always has a meaningful short value.
	Desc->SetStringField(TEXT("skillDescription"), SkillDescription.IsEmpty() ? Title : SkillDescription);
	Desc->SetObjectField(TEXT("inputSchema"), BuildInputSchema());
	if (OutputSchema.IsValid())
		Desc->SetObjectField(TEXT("outputSchema"), OutputSchema);
	Desc->SetBoolField(TEXT("readOnlyHint"), bReadOnlyHint);
	Desc->SetBoolField(TEXT("destructiveHint"), bDestructiveHint);
	Desc->SetBoolField(TEXT("idempotentHint"), bIdempotentHint);
	Desc->SetBoolField(TEXT("openWorldHint"), bOpenWorldHint);
	Desc->SetBoolField(TEXT("enabled"), bEnabled);
	// §2.4 served surface. Always emitted (never omitted for the Standard default) so the sidecar never has
	// to infer it and an older descriptor can be told apart from a deliberately-standard one.
	Desc->SetStringField(TEXT("toolType"), UnrealMcpToolTypeToString(ToolType));
	Desc->SetStringField(TEXT("extensionId"), ExtensionId);
	Desc->SetStringField(TEXT("schemaHash"), SchemaHash);
	return Desc;
}

// --- Tool type (§2.4) -----------------------------------------------------------------------------

const TCHAR* UnrealMcpToolTypeToString(EUnrealMcpToolType ToolType)
{
	return ToolType == EUnrealMcpToolType::System ? TEXT("system") : TEXT("standard");
}

EUnrealMcpToolType UnrealMcpToolTypeFromString(const FString& Token)
{
	// Lenient by design: an unknown/absent token means "the sender predates §2.4", and the safe reading of
	// silence is the pre-§2.4 behaviour — a standard tool. Never surfaces an unknown token as System.
	return Token.Equals(TEXT("system"), ESearchCase::IgnoreCase)
		? EUnrealMcpToolType::System
		: EUnrealMcpToolType::Standard;
}

// --- FUnrealMcpToolBuilder ------------------------------------------------------------------------

FUnrealMcpToolBuilder::FUnrealMcpToolBuilder(FUnrealMcpToolRegistry& InRegistry, const FString& InName)
	: Registry(InRegistry)
{
	Tool.Name = InName;
}

FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::Title(const FString& InTitle) { Tool.Title = InTitle; return *this; }
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::Description(const FString& InDescription) { Tool.Description = InDescription; return *this; }
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::SkillDescription(const FString& InSkillDescription) { Tool.SkillDescription = InSkillDescription; return *this; }

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
	FUnrealMcpParamSpec Spec{ Name, TEXT("object"), Desc, Req, FUnrealMcpSchema::VectorSchema(Desc) };
	Tool.Params.Add(Spec);
	return *this;
}
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::Param(const FString& Name, const FString& JsonType, const FString& Desc, EUnrealMcpParamRequirement Req, TSharedPtr<FJsonObject> CustomSchema)
{
	Tool.Params.Add(FUnrealMcpParamSpec{ Name, JsonType, Desc, Req, CustomSchema });
	return *this;
}
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::ToolType(EUnrealMcpToolType InToolType) { Tool.ToolType = InToolType; return *this; }
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

// --- FUnrealMcpToolRegistryTraits (kind-specific policy for the generic template) -----------------

bool FUnrealMcpToolRegistryTraits::IsValidKey(const FString& Name)
{
	return FUnrealMcpValidation::IsValidKebabName(Name);
}

bool FUnrealMcpToolRegistryTraits::Validate(const FUnrealMcpRegisteredTool& InTool, FString& OutError)
{
	if (!IsValidKey(InTool.Name))
	{
		OutError = FString::Printf(
			TEXT("invalid tool name '%s' (must be non-empty kebab-case: lowercase letters, digits, single internal hyphens)"),
			*InTool.Name);
		return false;
	}

	if (!InTool.Handler)
	{
		OutError = FString::Printf(TEXT("tool '%s' has no bound handler"), *InTool.Name);
		return false;
	}

	return FUnrealMcpValidation::ValidateParamSpecs(InTool.Params, TEXT("tool"), InTool.Name, OutError);
}

// --- FUnrealMcpToolRegistry (only the tool-specific surface; the rest is inherited from the template) ---

FUnrealMcpToolBuilder FUnrealMcpToolRegistry::Tool(const FString& Name)
{
	return FUnrealMcpToolBuilder(*this, Name);
}

FUnrealMcpToolResult FUnrealMcpToolRegistry::Execute(const FString& Name, const FUnrealMcpToolCall& Call) const
{
	// FindForDispatch enforces the §7 blocklist / §8 whitelist at the execution boundary (a disabled tool is
	// dropped from BuildManifestJson, but a sidecar on a stale manifest — toggle → push race — could still
	// dispatch it by name) and yields the SAME "Unknown tool '%s'." / "Tool '%s' is disabled." messages.
	FString Error;
	const FUnrealMcpRegisteredTool* Found = FindForDispatch(Name, Error);
	if (Found == nullptr)
		return FUnrealMcpToolResult::Error(Error);

	return Found->Handler(Call);
}
