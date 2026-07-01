// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpResourceRegistry.h"
#include "UnrealMcpLog.h"
#include "UnrealMcpDescriptorHash.h"

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

// --- FUnrealMcpResourceRegistryTraits (kind-specific policy for the generic template) -------------

bool FUnrealMcpResourceRegistryTraits::IsValidKey(const FString& Uri)
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

bool FUnrealMcpResourceRegistryTraits::Validate(const FUnrealMcpRegisteredResource& InResource, FString& OutError)
{
	if (!IsValidKey(InResource.Uri))
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

// --- FUnrealMcpResourceRegistry (only the resource-specific surface; the rest is inherited from the template) ---

FUnrealMcpResourceBuilder FUnrealMcpResourceRegistry::Resource(const FString& Uri)
{
	return FUnrealMcpResourceBuilder(*this, Uri);
}

FUnrealMcpResourceResult FUnrealMcpResourceRegistry::Read(const FString& Uri) const
{
	// FindForDispatch gates disabled resources at the read boundary (a disabled resource is dropped from
	// BuildManifestJson, but a stale resources/list could still dispatch it by uri) and yields the SAME
	// "Unknown resource '%s'." / "Resource '%s' is disabled." messages.
	FString Error;
	const FUnrealMcpRegisteredResource* Found = FindForDispatch(Uri, Error);
	if (Found == nullptr)
		return FUnrealMcpResourceResult::MakeError(Error);

	return Found->Handler(Uri);
}
