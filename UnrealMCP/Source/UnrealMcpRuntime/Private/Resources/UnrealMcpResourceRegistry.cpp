// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpResourceRegistry.h"
#include "UnrealMcpLog.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

// --- Stable serialization helper (family-UNIQUE name) ---------------------------------------------
// The tool/prompt registries each have their own SerializeStable in an anonymous namespace; a unity build
// concatenates every .cpp into one TU, so an anonymous-namespace helper is NOT file-private. Give the
// resource copy a family-unique name (Resource*-prefixed) to dodge the §ODR collision (conventions.md).

namespace
{
	FString ResourceSerializeStable(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}
}

TSharedPtr<FJsonObject> FUnrealMcpRegisteredResource::ToDescriptorJson() const
{
	// Mirrors the ResourceDescriptor IPC shape (§A.1): uri (the route + identity), name, description, mimeType.
	TSharedPtr<FJsonObject> Desc = MakeShared<FJsonObject>();
	Desc->SetStringField(TEXT("uri"), Uri);
	Desc->SetStringField(TEXT("name"), Name);
	Desc->SetStringField(TEXT("description"), Description);
	Desc->SetStringField(TEXT("mimeType"), MimeType);
	Desc->SetBoolField(TEXT("enabled"), bEnabled);
	Desc->SetStringField(TEXT("extensionId"), ExtensionId);
	Desc->SetStringField(TEXT("schemaHash"), SchemaHash);
	return Desc;
}

// --- FUnrealMcpResourceBuilder --------------------------------------------------------------------

FUnrealMcpResourceBuilder::FUnrealMcpResourceBuilder(FUnrealMcpResourceRegistry& InRegistry, const FString& InUri)
	: Registry(InRegistry)
{
	Resource.Uri = InUri;
}

FUnrealMcpResourceBuilder& FUnrealMcpResourceBuilder::Name(const FString& InName) { Resource.Name = InName; return *this; }
FUnrealMcpResourceBuilder& FUnrealMcpResourceBuilder::Description(const FString& InDescription) { Resource.Description = InDescription; return *this; }
FUnrealMcpResourceBuilder& FUnrealMcpResourceBuilder::MimeType(const FString& InMimeType) { Resource.MimeType = InMimeType; return *this; }
FUnrealMcpResourceBuilder& FUnrealMcpResourceBuilder::ExtensionId(const FString& InExtensionId) { Resource.ExtensionId = InExtensionId; return *this; }

void FUnrealMcpResourceBuilder::Read(FUnrealMcpResourceHandler InHandler)
{
	Resource.Handler = MoveTemp(InHandler);
	Registry.Commit(MoveTemp(Resource));
}

// --- FUnrealMcpResourceRegistry -------------------------------------------------------------------

FUnrealMcpResourceBuilder FUnrealMcpResourceRegistry::Resource(const FString& Uri)
{
	return FUnrealMcpResourceBuilder(*this, Uri);
}

FString FUnrealMcpResourceRegistry::ComputeSchemaHash(const FUnrealMcpRegisteredResource& InResource)
{
	// Hash the canonicalized descriptor MINUS the enabled flag (mirror the tool/prompt registries). Reuse
	// ToDescriptorJson and drop "enabled" + "schemaHash" so the hash is stable regardless of those fields.
	FUnrealMcpRegisteredResource Copy = InResource;
	Copy.SchemaHash.Empty();
	TSharedPtr<FJsonObject> Desc = Copy.ToDescriptorJson();
	Desc->RemoveField(TEXT("enabled"));
	Desc->RemoveField(TEXT("schemaHash"));

	const FString Canonical = ResourceSerializeStable(Desc);
	FSHA1 Sha;
	const FTCHARToUTF8 Utf8(*Canonical);
	Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Sha.Final();
	uint8 Digest[20];
	Sha.GetHash(Digest);
	return FString(TEXT("sha1:")) + BytesToHex(Digest, 20).ToLower();
}

bool FUnrealMcpResourceRegistry::IsValidResourceUri(const FString& Uri)
{
	// Static MVP: a resource URI must be non-empty and free of control characters / whitespace (a malformed
	// uri would leak into the manifest as the manager's route). We deliberately do NOT enforce a scheme so a
	// game can use any opaque "scheme://path" form (e.g. "unreal://project/levels"); templated URIs are
	// deferred (§A.1) so a brace/placeholder is not special here.
	if (Uri.IsEmpty())
		return false;

	for (const TCHAR C : Uri)
	{
		if (C <= 0x20 || C == 0x7F) // control chars + space
			return false;
	}
	return true;
}

bool FUnrealMcpResourceRegistry::ValidateResource(const FUnrealMcpRegisteredResource& InResource, FString& OutError)
{
	if (!IsValidResourceUri(InResource.Uri))
	{
		OutError = FString::Printf(
			TEXT("invalid resource uri '%s' (must be non-empty and contain no whitespace/control characters)"),
			*InResource.Uri);
		return false;
	}

	if (!InResource.Handler)
	{
		OutError = FString::Printf(TEXT("resource '%s' has no bound read handler"), *InResource.Uri);
		return false;
	}
	return true;
}

void FUnrealMcpResourceRegistry::Commit(FUnrealMcpRegisteredResource&& InResource)
{
	if (bExtensionScope)
	{
		// Extensions are untrusted: stamp the owning id, validate, and dedup (§5).
		InResource.ExtensionId = ScopeExtensionId;

		FString Error;
		if (!ValidateResource(InResource, Error))
		{
			ScopeErrors.Add(Error);
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension '%s': %s — entry dropped."), *ScopeExtensionId, *Error);
			return;
		}

		if (const FUnrealMcpRegisteredResource* Existing = Resources.Find(InResource.Uri))
		{
			FString Rejection;
			if (Existing->ExtensionId == TEXT("core"))
			{
				Rejection = FString::Printf(
					TEXT("resource '%s' rejected: uri collides with a built-in core resource (extensions may not shadow core)"),
					*InResource.Uri);
			}
			else if (Existing->ExtensionId == ScopeExtensionId)
			{
				Rejection = FString::Printf(
					TEXT("resource '%s' rejected: this extension already declared a resource with that uri (duplicate within the extension)"),
					*InResource.Uri);
			}
			else
			{
				Rejection = FString::Printf(
					TEXT("resource '%s' rejected: uri already registered by extension '%s' (first-wins by ExtensionId sort)"),
					*InResource.Uri, *Existing->ExtensionId);
			}
			ScopeErrors.Add(Rejection);
			UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension '%s': %s"), *ScopeExtensionId, *Rejection);
			return;
		}

		++ScopeResourcesRegistered;
	}

	InResource.SchemaHash = ComputeSchemaHash(InResource);
	const FString Uri = InResource.Uri;
	// §7/§8 retention: a (re-)registered resource inherits the retained whitelist/blocklist immediately.
	InResource.bEnabled = ShouldResourceBeEnabled(Uri);
	Resources.Add(Uri, MoveTemp(InResource));
	++Revision;
	UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] registered resource '%s' (revision %d)."), *Uri, Revision);
}

FUnrealMcpExtensionRegistrationResult FUnrealMcpResourceRegistry::RegisterExtension(
	const FString& ExtensionId, TFunctionRef<void(FUnrealMcpResourceRegistry&)> RegisterFn)
{
	checkf(!bExtensionScope, TEXT("FUnrealMcpResourceRegistry::RegisterExtension does not support nested scopes."));

	bExtensionScope = true;
	ScopeExtensionId = ExtensionId;
	ScopeResourcesRegistered = 0;
	ScopeErrors.Reset();

	RegisterFn(*this);

	FUnrealMcpExtensionRegistrationResult Result;
	Result.ToolsRegistered = ScopeResourcesRegistered; // reused struct: count means "entries registered"
	Result.Errors = MoveTemp(ScopeErrors);

	bExtensionScope = false;
	ScopeExtensionId.Empty();
	ScopeResourcesRegistered = 0;
	ScopeErrors.Reset();
	return Result;
}

int32 FUnrealMcpResourceRegistry::RemoveResourcesForExtension(const FString& ExtensionId)
{
	TArray<FString> ToRemove;
	for (const TPair<FString, FUnrealMcpRegisteredResource>& Pair : Resources)
	{
		if (Pair.Value.ExtensionId == ExtensionId)
			ToRemove.Add(Pair.Key);
	}
	for (const FString& Uri : ToRemove)
		Resources.Remove(Uri);
	if (ToRemove.Num() > 0)
		++Revision;
	return ToRemove.Num();
}

TSharedPtr<FJsonObject> FUnrealMcpResourceRegistry::BuildManifestJson() const
{
	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("type"), TEXT("resource-manifest"));
	Manifest->SetNumberField(TEXT("revision"), Revision);

	TArray<TSharedPtr<FJsonValue>> ResourceArray;
	TArray<FString> Uris;
	Resources.GetKeys(Uris);
	Uris.Sort();
	for (const FString& Uri : Uris)
	{
		// A disabled resource is EXCLUDED from the served manifest entirely (mirror the tool/prompt registries).
		const FUnrealMcpRegisteredResource& R = Resources[Uri];
		if (!R.bEnabled)
			continue;
		ResourceArray.Add(MakeShared<FJsonValueObject>(R.ToDescriptorJson()));
	}

	Manifest->SetArrayField(TEXT("resources"), ResourceArray);
	return Manifest;
}

TArray<FString> FUnrealMcpResourceRegistry::GetResourceUrisSorted() const
{
	TArray<FString> Uris;
	Resources.GetKeys(Uris);
	Uris.Sort();
	return Uris;
}

int32 FUnrealMcpResourceRegistry::NumEnabled() const
{
	int32 Count = 0;
	for (const TPair<FString, FUnrealMcpRegisteredResource>& Pair : Resources)
	{
		if (Pair.Value.bEnabled)
			++Count;
	}
	return Count;
}

bool FUnrealMcpResourceRegistry::SetResourceEnabled(const FString& Uri, bool bEnabled)
{
	FUnrealMcpRegisteredResource* Found = Resources.Find(Uri);
	if (Found == nullptr || Found->bEnabled == bEnabled)
		return false;
	Found->bEnabled = bEnabled;
	++Revision;
	UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] resource '%s' %s (revision %d)."),
		*Uri, bEnabled ? TEXT("enabled") : TEXT("disabled"), Revision);
	return true;
}

bool FUnrealMcpResourceRegistry::ShouldResourceBeEnabled(const FString& Uri) const
{
	return PassesEnabledResourcesWhitelist(Uri) && !DisabledResourceUris.Contains(Uri);
}

bool FUnrealMcpResourceRegistry::PassesEnabledResourcesWhitelist(const FString& Uri) const
{
	return EnabledResourcesWhitelist.IsEmpty() || EnabledResourcesWhitelist.Contains(Uri);
}

void FUnrealMcpResourceRegistry::RecomputeEnablement()
{
	bool bAnyChanged = false;
	for (TPair<FString, FUnrealMcpRegisteredResource>& Pair : Resources)
	{
		const bool bShouldEnable = ShouldResourceBeEnabled(Pair.Key);
		if (Pair.Value.bEnabled != bShouldEnable)
		{
			Pair.Value.bEnabled = bShouldEnable;
			bAnyChanged = true;
		}
	}
	if (bAnyChanged)
		++Revision;
}

void FUnrealMcpResourceRegistry::SetEnabledResourcesFilter(const TArray<FString>& EnabledResources)
{
	EnabledResourcesWhitelist = TSet<FString>(EnabledResources);
	RecomputeEnablement();
}

void FUnrealMcpResourceRegistry::ApplyDisabledResources(const TArray<FString>& DisabledUris)
{
	DisabledResourceUris = TSet<FString>(DisabledUris);
	RecomputeEnablement();
}

FUnrealMcpResourceResult FUnrealMcpResourceRegistry::Read(const FString& Uri) const
{
	const FUnrealMcpRegisteredResource* Found = Resources.Find(Uri);
	if (Found == nullptr || !Found->Handler)
		return FUnrealMcpResourceResult::MakeError(FString::Printf(TEXT("Unknown resource '%s'."), *Uri));

	// Gate disabled resources at the read boundary (mirror the tool/prompt registries): a disabled resource is
	// dropped from BuildManifestJson, but a stale resources/list could still dispatch it by uri.
	if (!Found->bEnabled)
		return FUnrealMcpResourceResult::MakeError(FString::Printf(TEXT("Resource '%s' is disabled."), *Uri));

	return Found->Handler(Uri);
}
