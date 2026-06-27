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

// --- FUnrealMcpPromptRegistry ---------------------------------------------------------------------

FUnrealMcpPromptBuilder FUnrealMcpPromptRegistry::Prompt(const FString& Name)
{
	return FUnrealMcpPromptBuilder(*this, Name);
}

FString FUnrealMcpPromptRegistry::ComputeSchemaHash(const FUnrealMcpRegisteredPrompt& InPrompt)
{
	// Hash the canonicalized descriptor MINUS the enabled flag (mirror the tool registry): the shared helper
	// builds the hash off the descriptor JSON directly (it strips enabled/schemaHash itself), no struct copy needed.
	return UnrealMcpComputeDescriptorHash(InPrompt.ToDescriptorJson());
}

bool FUnrealMcpPromptRegistry::IsValidPromptName(const FString& Name)
{
	// Same shared kebab rule as the tool registry (no leading/trailing/doubled hyphen; lowercase letters/digits).
	return FUnrealMcpValidation::IsValidKebabName(Name);
}

bool FUnrealMcpPromptRegistry::ValidatePrompt(const FUnrealMcpRegisteredPrompt& InPrompt, FString& OutError)
{
	if (!IsValidPromptName(InPrompt.Name))
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

void FUnrealMcpPromptRegistry::Commit(FUnrealMcpRegisteredPrompt&& InPrompt)
{
	if (bExtensionScope)
	{
		// Extensions are untrusted: stamp the owning id, validate, and dedup (§5).
		InPrompt.ExtensionId = ScopeExtensionId;

		FString Error;
		if (!ValidatePrompt(InPrompt, Error))
		{
			ScopeErrors.Add(Error);
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension '%s': %s — entry dropped."), *ScopeExtensionId, *Error);
			return;
		}

		if (const FUnrealMcpRegisteredPrompt* Existing = Prompts.Find(InPrompt.Name))
		{
			FString Rejection;
			if (Existing->ExtensionId == TEXT("core"))
			{
				Rejection = FString::Printf(
					TEXT("prompt '%s' rejected: name collides with a built-in core prompt (extensions may not shadow core)"),
					*InPrompt.Name);
			}
			else if (Existing->ExtensionId == ScopeExtensionId)
			{
				Rejection = FString::Printf(
					TEXT("prompt '%s' rejected: this extension already declared a prompt with that name (duplicate within the extension)"),
					*InPrompt.Name);
			}
			else
			{
				Rejection = FString::Printf(
					TEXT("prompt '%s' rejected: name already registered by extension '%s' (first-wins by ExtensionId sort)"),
					*InPrompt.Name, *Existing->ExtensionId);
			}
			ScopeErrors.Add(Rejection);
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension '%s': %s"), *ScopeExtensionId, *Rejection);
			return;
		}

		++ScopePromptsRegistered;
	}

	InPrompt.SchemaHash = ComputeSchemaHash(InPrompt);
	const FString Name = InPrompt.Name;
	// §7/§8 retention: a (re-)registered prompt inherits the retained whitelist/blocklist immediately.
	InPrompt.bEnabled = ShouldPromptBeEnabled(Name);
	Prompts.Add(Name, MoveTemp(InPrompt));
	++Revision;
	UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] registered prompt '%s' (revision %d)."), *Name, Revision);
}

FUnrealMcpExtensionRegistrationResult FUnrealMcpPromptRegistry::RegisterExtension(
	const FString& ExtensionId, TFunctionRef<void(FUnrealMcpPromptRegistry&)> RegisterFn)
{
	checkf(!bExtensionScope, TEXT("FUnrealMcpPromptRegistry::RegisterExtension does not support nested scopes."));

	bExtensionScope = true;
	ScopeExtensionId = ExtensionId;
	ScopePromptsRegistered = 0;
	ScopeErrors.Reset();

	RegisterFn(*this);

	FUnrealMcpExtensionRegistrationResult Result;
	Result.ToolsRegistered = ScopePromptsRegistered; // reused struct: count means "entries registered"
	Result.Errors = MoveTemp(ScopeErrors);

	bExtensionScope = false;
	ScopeExtensionId.Empty();
	ScopePromptsRegistered = 0;
	ScopeErrors.Reset();
	return Result;
}

int32 FUnrealMcpPromptRegistry::RemovePromptsForExtension(const FString& ExtensionId)
{
	TArray<FString> ToRemove;
	for (const TPair<FString, FUnrealMcpRegisteredPrompt>& Pair : Prompts)
	{
		if (Pair.Value.ExtensionId == ExtensionId)
			ToRemove.Add(Pair.Key);
	}
	for (const FString& Name : ToRemove)
		Prompts.Remove(Name);
	if (ToRemove.Num() > 0)
		++Revision;
	return ToRemove.Num();
}

TSharedPtr<FJsonObject> FUnrealMcpPromptRegistry::BuildManifestJson() const
{
	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("type"), TEXT("prompt-manifest"));
	Manifest->SetNumberField(TEXT("revision"), Revision);

	TArray<TSharedPtr<FJsonValue>> PromptArray;
	const TArray<FString> Names = GetPromptNamesSorted();
	for (const FString& Name : Names)
	{
		// A disabled prompt is EXCLUDED from the served manifest entirely (mirror the tool registry).
		const FUnrealMcpRegisteredPrompt& P = Prompts[Name];
		if (!P.bEnabled)
			continue;
		PromptArray.Add(MakeShared<FJsonValueObject>(P.ToDescriptorJson()));
	}

	Manifest->SetArrayField(TEXT("prompts"), PromptArray);
	return Manifest;
}

TArray<FString> FUnrealMcpPromptRegistry::GetPromptNamesSorted() const
{
	TArray<FString> Names;
	Prompts.GetKeys(Names);
	Names.Sort();
	return Names;
}

int32 FUnrealMcpPromptRegistry::NumEnabled() const
{
	int32 Count = 0;
	for (const TPair<FString, FUnrealMcpRegisteredPrompt>& Pair : Prompts)
	{
		if (Pair.Value.bEnabled)
			++Count;
	}
	return Count;
}

bool FUnrealMcpPromptRegistry::SetPromptEnabled(const FString& Name, bool bEnabled)
{
	FUnrealMcpRegisteredPrompt* Found = Prompts.Find(Name);
	if (Found == nullptr || Found->bEnabled == bEnabled)
		return false;
	Found->bEnabled = bEnabled;
	++Revision;
	UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] prompt '%s' %s (revision %d)."),
		*Name, bEnabled ? TEXT("enabled") : TEXT("disabled"), Revision);
	return true;
}

bool FUnrealMcpPromptRegistry::ShouldPromptBeEnabled(const FString& Name) const
{
	return Enablement.ShouldBeEnabled(Name);
}

bool FUnrealMcpPromptRegistry::PassesEnabledPromptsWhitelist(const FString& Name) const
{
	return Enablement.PassesWhitelist(Name);
}

void FUnrealMcpPromptRegistry::RecomputeEnablement()
{
	bool bAnyChanged = false;
	for (TPair<FString, FUnrealMcpRegisteredPrompt>& Pair : Prompts)
	{
		const bool bShouldEnable = ShouldPromptBeEnabled(Pair.Key);
		if (Pair.Value.bEnabled != bShouldEnable)
		{
			Pair.Value.bEnabled = bShouldEnable;
			bAnyChanged = true;
		}
	}
	if (bAnyChanged)
		++Revision;
}

void FUnrealMcpPromptRegistry::SetEnabledPromptsFilter(const TArray<FString>& EnabledPrompts)
{
	Enablement.SetWhitelist(EnabledPrompts);
	RecomputeEnablement();
}

void FUnrealMcpPromptRegistry::ApplyDisabledPrompts(const TArray<FString>& DisabledNames)
{
	Enablement.SetBlocklist(DisabledNames);
	RecomputeEnablement();
}

FUnrealMcpPromptResult FUnrealMcpPromptRegistry::Execute(const FString& Name, const FUnrealMcpToolCall& Call) const
{
	const FUnrealMcpRegisteredPrompt* Found = Prompts.Find(Name);
	if (Found == nullptr || !Found->Handler)
		return FUnrealMcpPromptResult::Error(FString::Printf(TEXT("Unknown prompt '%s'."), *Name));

	// Gate disabled prompts at the execution boundary (mirror the tool registry): a disabled prompt is
	// dropped from BuildManifestJson, but a stale prompts/list could still dispatch it by name.
	if (!Found->bEnabled)
		return FUnrealMcpPromptResult::Error(FString::Printf(TEXT("Prompt '%s' is disabled."), *Name));

	return Found->Handler(Call);
}
