// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpPromptRegistry.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpDescriptorHash.h"
#include "UnrealMcpSchema.h"
#include "UnrealMcpValidation.h"

// --- Schema building ------------------------------------------------------------------------------
// The §3.2 scalar + vector schema builders are the shared FUnrealMcpSchema TU (UnrealMcpSchema.h) — the same
// externally-linked definition the tool registry + editor tool families use (no more Prompt*-prefixed clones).

namespace
{
	/** The wire string for an EUnrealMcpPromptRole ("user" | "assistant"). */
	const TCHAR* PromptRoleToString(EUnrealMcpPromptRole Role)
	{
		return Role == EUnrealMcpPromptRole::Assistant ? TEXT("assistant") : TEXT("user");
	}
}

TSharedPtr<FJsonObject> FUnrealMcpRegisteredPrompt::BuildInputSchema() const
{
	// Identical to FUnrealMcpRegisteredTool::BuildInputSchema (§3.2): object / properties / required.
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Required;

	for (const FUnrealMcpParamSpec& Param : Params)
	{
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

TSharedPtr<FJsonObject> FUnrealMcpRegisteredPrompt::ToDescriptorJson() const
{
	// Mirrors the tool descriptor minus the tool-only hint/skill fields, PLUS the `role` string field (§A.1).
	TSharedPtr<FJsonObject> Desc = MakeShared<FJsonObject>();
	Desc->SetStringField(TEXT("name"), Name);
	Desc->SetStringField(TEXT("title"), Title);
	Desc->SetStringField(TEXT("description"), Description);
	Desc->SetStringField(TEXT("role"), PromptRoleToString(Role));
	Desc->SetObjectField(TEXT("inputSchema"), BuildInputSchema());
	Desc->SetBoolField(TEXT("enabled"), bEnabled);
	Desc->SetStringField(TEXT("extensionId"), ExtensionId);
	Desc->SetStringField(TEXT("schemaHash"), SchemaHash);
	return Desc;
}

// --- FUnrealMcpPromptBuilder ----------------------------------------------------------------------

FUnrealMcpPromptBuilder::FUnrealMcpPromptBuilder(FUnrealMcpPromptRegistry& InRegistry, const FString& InName)
	: Registry(InRegistry)
{
	Prompt.Name = InName;
}

FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::Title(const FString& InTitle) { Prompt.Title = InTitle; return *this; }
FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::Description(const FString& InDescription) { Prompt.Description = InDescription; return *this; }
FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::Role(EUnrealMcpPromptRole InRole) { Prompt.Role = InRole; return *this; }

FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::ParamString(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	Prompt.Params.Add(FUnrealMcpParamSpec{ Name, TEXT("string"), Desc, Req, nullptr });
	return *this;
}
FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::ParamInt(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	Prompt.Params.Add(FUnrealMcpParamSpec{ Name, TEXT("integer"), Desc, Req, nullptr });
	return *this;
}
FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::ParamNumber(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	Prompt.Params.Add(FUnrealMcpParamSpec{ Name, TEXT("number"), Desc, Req, nullptr });
	return *this;
}
FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::ParamBool(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	Prompt.Params.Add(FUnrealMcpParamSpec{ Name, TEXT("boolean"), Desc, Req, nullptr });
	return *this;
}
FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::ParamVector(const FString& Name, const FString& Desc, EUnrealMcpParamRequirement Req)
{
	FUnrealMcpParamSpec Spec{ Name, TEXT("object"), Desc, Req, FUnrealMcpSchema::VectorSchema(Desc) };
	Prompt.Params.Add(Spec);
	return *this;
}
FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::Param(const FString& Name, const FString& JsonType, const FString& Desc, EUnrealMcpParamRequirement Req, TSharedPtr<FJsonObject> CustomSchema)
{
	Prompt.Params.Add(FUnrealMcpParamSpec{ Name, JsonType, Desc, Req, CustomSchema });
	return *this;
}
FUnrealMcpPromptBuilder& FUnrealMcpPromptBuilder::ExtensionId(const FString& InExtensionId) { Prompt.ExtensionId = InExtensionId; return *this; }

void FUnrealMcpPromptBuilder::Handle(FUnrealMcpPromptHandler InHandler)
{
	Prompt.Handler = MoveTemp(InHandler);
	Registry.Commit(MoveTemp(Prompt));
}

// --- FUnrealMcpPromptRegistryTraits (kind-specific policy for the generic template) ---------------

bool FUnrealMcpPromptRegistryTraits::IsValidKey(const FString& Name)
{
	// Same shared kebab rule as the tool registry (no leading/trailing/doubled hyphen; lowercase letters/digits).
	return FUnrealMcpValidation::IsValidKebabName(Name);
}

bool FUnrealMcpPromptRegistryTraits::Validate(const FUnrealMcpRegisteredPrompt& InPrompt, FString& OutError)
{
	if (!IsValidKey(InPrompt.Name))
	{
		OutError = FString::Printf(
			TEXT("invalid prompt name '%s' (must be non-empty kebab-case: lowercase letters, digits, single internal hyphens)"),
			*InPrompt.Name);
		return false;
	}

	if (!InPrompt.Handler)
	{
		OutError = FString::Printf(TEXT("prompt '%s' has no bound handler"), *InPrompt.Name);
		return false;
	}

	return FUnrealMcpValidation::ValidateParamSpecs(InPrompt.Params, TEXT("prompt"), InPrompt.Name, OutError);
}

// --- FUnrealMcpPromptRegistry (only the prompt-specific surface; the rest is inherited from the template) ---

FUnrealMcpPromptBuilder FUnrealMcpPromptRegistry::Prompt(const FString& Name)
{
	return FUnrealMcpPromptBuilder(*this, Name);
}

FUnrealMcpPromptResult FUnrealMcpPromptRegistry::Execute(const FString& Name, const FUnrealMcpToolCall& Call) const
{
	// FindForDispatch gates disabled prompts at the execution boundary (a disabled prompt is dropped from
	// BuildManifestJson, but a stale prompts/list could still dispatch it by name) and yields the SAME
	// "Unknown prompt '%s'." / "Prompt '%s' is disabled." messages.
	FString Error;
	const FUnrealMcpRegisteredPrompt* Found = FindForDispatch(Name, Error);
	if (Found == nullptr)
		return FUnrealMcpPromptResult::Error(Error);

	return Found->Handler(Call);
}
