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
		// A custom schema (object/array/exotic types built via the generic Param()) is used verbatim;
		// the simple scalar types fall back to a {type,description} schema.
		TSharedPtr<FJsonObject> ParamSchema = Param.ObjectSchema.IsValid()
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
FUnrealMcpToolBuilder& FUnrealMcpToolBuilder::Param(const FString& Name, const FString& JsonType, const FString& Desc, EUnrealMcpParamRequirement Req, TSharedPtr<FJsonObject> CustomSchema)
{
	Tool.Params.Add(FUnrealMcpParamSpec{ Name, JsonType, Desc, Req, CustomSchema });
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

bool FUnrealMcpToolRegistry::IsValidToolName(const FString& Name)
{
	if (Name.IsEmpty())
		return false;

	bool bPrevHyphen = false;
	const int32 Len = Name.Len();
	for (int32 i = 0; i < Len; ++i)
	{
		const TCHAR C = Name[i];
		const bool bLower = (C >= TEXT('a') && C <= TEXT('z'));
		const bool bDigit = (C >= TEXT('0') && C <= TEXT('9'));
		const bool bHyphen = (C == TEXT('-'));
		if (!bLower && !bDigit && !bHyphen)
			return false;
		if (bHyphen && (i == 0 || i == Len - 1 || bPrevHyphen))
			return false; // no leading, trailing, or doubled hyphen
		bPrevHyphen = bHyphen;
	}
	return true;
}

bool FUnrealMcpToolRegistry::ValidateTool(const FUnrealMcpRegisteredTool& InTool, FString& OutError)
{
	if (!IsValidToolName(InTool.Name))
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

	static const TCHAR* const KnownTypes[] = { TEXT("string"), TEXT("integer"), TEXT("number"), TEXT("boolean"), TEXT("object"), TEXT("array") };
	TSet<FString> SeenParams;
	for (const FUnrealMcpParamSpec& Param : InTool.Params)
	{
		if (Param.Name.IsEmpty())
		{
			OutError = FString::Printf(TEXT("tool '%s' has a parameter with an empty name (malformed schema)"), *InTool.Name);
			return false;
		}

		bool bKnown = false;
		for (const TCHAR* const Known : KnownTypes)
		{
			if (Param.JsonType == Known) { bKnown = true; break; }
		}
		if (!bKnown)
		{
			OutError = FString::Printf(TEXT("tool '%s' parameter '%s' has unknown JSON type '%s' (malformed schema)"),
				*InTool.Name, *Param.Name, *Param.JsonType);
			return false;
		}

		if ((Param.JsonType == TEXT("object") || Param.JsonType == TEXT("array")) && !Param.ObjectSchema.IsValid())
		{
			OutError = FString::Printf(TEXT("tool '%s' %s parameter '%s' has no schema (malformed schema)"),
				*InTool.Name, *Param.JsonType, *Param.Name);
			return false;
		}

		bool bAlreadyPresent = false;
		SeenParams.Add(Param.Name, &bAlreadyPresent);
		if (bAlreadyPresent)
		{
			OutError = FString::Printf(TEXT("tool '%s' declares duplicate parameter '%s' (malformed schema)"),
				*InTool.Name, *Param.Name);
			return false;
		}
	}
	return true;
}

void FUnrealMcpToolRegistry::Commit(FUnrealMcpRegisteredTool&& InTool)
{
	if (bExtensionScope)
	{
		// Extensions are untrusted: stamp the owning id, validate, and dedup (§5). A dropped/rejected
		// entry never touches the committed set, so other tools and extensions are unaffected.
		InTool.ExtensionId = ScopeExtensionId;

		FString Error;
		if (!ValidateTool(InTool, Error))
		{
			ScopeErrors.Add(Error);
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension '%s': %s — entry dropped."), *ScopeExtensionId, *Error);
			return;
		}

		if (const FUnrealMcpRegisteredTool* Existing = Tools.Find(InTool.Name))
		{
			// Tailor the reason to the kind of collision so the recorded error is not misleading:
			// a core-tool clash, a clash with a different extension (first-wins by ExtensionId sort),
			// or two tools sharing a name WITHIN this same provider (an authoring error, no sort involved).
			FString Rejection;
			if (Existing->ExtensionId == TEXT("core"))
			{
				Rejection = FString::Printf(
					TEXT("tool '%s' rejected: name collides with a built-in core tool (extensions may not shadow core)"),
					*InTool.Name);
			}
			else if (Existing->ExtensionId == ScopeExtensionId)
			{
				Rejection = FString::Printf(
					TEXT("tool '%s' rejected: this extension already declared a tool with that name (duplicate within the extension)"),
					*InTool.Name);
			}
			else
			{
				Rejection = FString::Printf(
					TEXT("tool '%s' rejected: name already registered by extension '%s' (first-wins by ExtensionId sort)"),
					*InTool.Name, *Existing->ExtensionId);
			}
			ScopeErrors.Add(Rejection);
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension '%s': %s"), *ScopeExtensionId, *Rejection);
			return;
		}

		++ScopeToolsRegistered;
	}

	InTool.SchemaHash = ComputeSchemaHash(InTool);
	const FString Name = InTool.Name;
	// §7/§8 retention: a (re-)registered tool inherits the retained whitelist/blocklist immediately, so an
	// extension hot-reload (RebuildFromProviders removes then re-registers every extension tool FRESH, with
	// bEnabled defaulting true) cannot resurrect a tool the user disabled, and a non-empty §8 whitelist still
	// hides a non-whitelisted tool a rebuild re-adds. The schema hash above excludes the enabled flag (§2.2),
	// so flipping bEnabled here does not perturb it.
	InTool.bEnabled = ShouldToolBeEnabled(Name);
	Tools.Add(Name, MoveTemp(InTool));
	++Revision;
	UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] registered tool '%s' (revision %d)."), *Name, Revision);
}

FUnrealMcpExtensionRegistrationResult FUnrealMcpToolRegistry::RegisterExtension(
	const FString& ExtensionId, TFunctionRef<void(FUnrealMcpToolRegistry&)> RegisterFn)
{
	checkf(!bExtensionScope, TEXT("FUnrealMcpToolRegistry::RegisterExtension does not support nested scopes."));

	bExtensionScope = true;
	ScopeExtensionId = ExtensionId;
	ScopeToolsRegistered = 0;
	ScopeErrors.Reset();

	RegisterFn(*this);

	FUnrealMcpExtensionRegistrationResult Result;
	Result.ToolsRegistered = ScopeToolsRegistered;
	Result.Errors = MoveTemp(ScopeErrors);

	bExtensionScope = false;
	ScopeExtensionId.Empty();
	ScopeToolsRegistered = 0;
	ScopeErrors.Reset();
	return Result;
}

int32 FUnrealMcpToolRegistry::RemoveToolsForExtension(const FString& ExtensionId)
{
	TArray<FString> ToRemove;
	for (const TPair<FString, FUnrealMcpRegisteredTool>& Pair : Tools)
	{
		if (Pair.Value.ExtensionId == ExtensionId)
			ToRemove.Add(Pair.Key);
	}
	for (const FString& Name : ToRemove)
		Tools.Remove(Name);
	if (ToRemove.Num() > 0)
		++Revision;
	return ToRemove.Num();
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
	{
		// §7 per-tool enable-map: a disabled tool is EXCLUDED from the served manifest entirely (not merely
		// flagged), so the sidecar mirrors no ProxyTool for it and it never appears in tools/list (§2.2).
		const FUnrealMcpRegisteredTool& Tool = Tools[Name];
		if (!Tool.bEnabled)
			continue;
		ToolArray.Add(MakeShared<FJsonValueObject>(Tool.ToDescriptorJson()));
	}

	Manifest->SetArrayField(TEXT("tools"), ToolArray);
	return Manifest;
}

TArray<FString> FUnrealMcpToolRegistry::GetToolNamesSorted() const
{
	TArray<FString> Names;
	Tools.GetKeys(Names);
	Names.Sort();
	return Names;
}

int32 FUnrealMcpToolRegistry::NumEnabled() const
{
	int32 Count = 0;
	for (const TPair<FString, FUnrealMcpRegisteredTool>& Pair : Tools)
	{
		if (Pair.Value.bEnabled)
			++Count;
	}
	return Count;
}

bool FUnrealMcpToolRegistry::SetToolEnabled(const FString& Name, bool bEnabled)
{
	FUnrealMcpRegisteredTool* Found = Tools.Find(Name);
	if (Found == nullptr || Found->bEnabled == bEnabled)
		return false;
	Found->bEnabled = bEnabled;
	++Revision;
	UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] tool '%s' %s (revision %d)."),
		*Name, bEnabled ? TEXT("enabled") : TEXT("disabled"), Revision);
	return true;
}

bool FUnrealMcpToolRegistry::ShouldToolBeEnabled(const FString& Name) const
{
	// §8 effective-served rule: served iff (whitelist empty — no filter — OR the name is whitelisted) AND the
	// name is NOT in the §7 blocklist. Both sets are retained members, so this is the single source of truth
	// shared by Commit (per-registration) and RecomputeEnablement (per-filter-change) — neither can clobber the
	// other's intent.
	const bool bPassesWhitelist = EnabledToolsWhitelist.IsEmpty() || EnabledToolsWhitelist.Contains(Name);
	return bPassesWhitelist && !DisabledToolNames.Contains(Name);
}

void FUnrealMcpToolRegistry::RecomputeEnablement()
{
	bool bAnyChanged = false;
	for (TPair<FString, FUnrealMcpRegisteredTool>& Pair : Tools)
	{
		const bool bShouldEnable = ShouldToolBeEnabled(Pair.Key);
		if (Pair.Value.bEnabled != bShouldEnable)
		{
			Pair.Value.bEnabled = bShouldEnable;
			bAnyChanged = true;
		}
	}
	if (bAnyChanged)
		++Revision;
}

void FUnrealMcpToolRegistry::SetEnabledToolsFilter(const TArray<FString>& EnabledTools)
{
	EnabledToolsWhitelist = TSet<FString>(EnabledTools);
	RecomputeEnablement();
}

void FUnrealMcpToolRegistry::ApplyDisabledTools(const TArray<FString>& DisabledNames)
{
	DisabledToolNames = TSet<FString>(DisabledNames);
	RecomputeEnablement();
}

FUnrealMcpToolResult FUnrealMcpToolRegistry::Execute(const FString& Name, const FUnrealMcpToolCall& Call) const
{
	const FUnrealMcpRegisteredTool* Found = Tools.Find(Name);
	if (Found == nullptr || !Found->Handler)
		return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Unknown tool '%s'."), *Name));

	return Found->Handler(Call);
}
