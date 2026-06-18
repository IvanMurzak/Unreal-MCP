// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"
// The resource registry reuses the §5 extension-registration result struct (FUnrealMcpExtensionRegistrationResult)
// from the tool registry verbatim — include that header rather than re-declaring it (mirror, don't fork).
#include "UnrealMcpToolRegistry.h"

/**
 * Core resource-registration types for the Unreal-MCP plugin (docs/ARCHITECTURE.md §A.1 / §A.2). The exact
 * resource sibling of UnrealMcpToolRegistry.h / UnrealMcpPromptRegistry.h: the registry is the plugin-owned
 * source of truth for the resource set; each family declares its resources through the fluent
 * FUnrealMcpResourceBuilder; the registry compiles each declaration into a JSON descriptor (the
 * resource-manifest shape) and dispatches incoming resource-read requests to the declared handler. The
 * sidecar mirrors the manifest into MCP ProxyResources.
 *
 * MVP scope (§A.1): static fixed-URI resources. A resource's URI is its identity (the registry key) AND the
 * MCP route. Templated / parameterized URIs are deferred (upstream McpPlugin has the template types). A
 * resource read returns one or more content blocks: each is Text(string) XOR Blob(base64) + mimeType, so
 * binary content rides as base64 like screenshot images.
 */

/** One content block of a resource read, mirroring the IPC resource-response contents[] shape (§A.1). */
struct UNREALMCPRUNTIME_API FUnrealMcpResourceContent
{
	FString Uri;        // the resource uri this block belongs to (echoed back; usually the requested uri)
	FString MimeType;   // optional MIME type of this block
	FString Text;       // text content (XOR Blob)
	FString Blob;       // base64-encoded binary content (XOR Text)
	bool bIsBlob = false;

	static FUnrealMcpResourceContent MakeText(const FString& InUri, const FString& InText, const FString& InMimeType = FString())
	{
		FUnrealMcpResourceContent Content;
		Content.Uri = InUri;
		Content.Text = InText;
		Content.MimeType = InMimeType;
		Content.bIsBlob = false;
		return Content;
	}

	static FUnrealMcpResourceContent MakeBlob(const FString& InUri, const FString& InBase64, const FString& InMimeType = FString())
	{
		FUnrealMcpResourceContent Content;
		Content.Uri = InUri;
		Content.Blob = InBase64;
		Content.MimeType = InMimeType;
		Content.bIsBlob = true;
		return Content;
	}
};

/** Terminal result of a resource-read, mirroring the IPC resource-response shape (§A.1). */
struct UNREALMCPRUNTIME_API FUnrealMcpResourceResult
{
	bool bSuccess = true;
	FString Error;                                   // human-readable reason on failure (never a secret)
	TArray<FUnrealMcpResourceContent> Contents;      // the returned content blocks

	/** A single-text-block success — the common JSON/string resource (mimeType e.g. "application/json"). */
	static FUnrealMcpResourceResult Text(const FString& Uri, const FString& InText, const FString& MimeType = FString())
	{
		FUnrealMcpResourceResult Result;
		Result.bSuccess = true;
		Result.Contents.Add(FUnrealMcpResourceContent::MakeText(Uri, InText, MimeType));
		return Result;
	}

	/** A single-blob-block success — binary content carried as base64 (mimeType e.g. "image/png"). */
	static FUnrealMcpResourceResult Blob(const FString& Uri, const FString& InBase64, const FString& MimeType = FString())
	{
		FUnrealMcpResourceResult Result;
		Result.bSuccess = true;
		Result.Contents.Add(FUnrealMcpResourceContent::MakeBlob(Uri, InBase64, MimeType));
		return Result;
	}

	/** A multi-block success (the caller assembled the content list). */
	static FUnrealMcpResourceResult Success(const TArray<FUnrealMcpResourceContent>& InContents)
	{
		FUnrealMcpResourceResult Result;
		Result.bSuccess = true;
		Result.Contents = InContents;
		return Result;
	}

	static FUnrealMcpResourceResult MakeError(const FString& InError)
	{
		FUnrealMcpResourceResult Result;
		Result.bSuccess = false;
		Result.Error = InError;
		return Result;
	}
};

/**
 * Signature of a resource handler: runs on the game thread (the dispatcher guarantees it, §4), returns a
 * terminal result synchronously. The requested URI is passed in (for a static MVP resource it equals the
 * descriptor's uri; the parameter keeps the signature forward-compatible with templated URIs, §A.1).
 */
using FUnrealMcpResourceHandler = TFunction<FUnrealMcpResourceResult(const FString& /*Uri*/)>;

/** A fully compiled resource: descriptor (manifest shape) + handler. */
struct UNREALMCPRUNTIME_API FUnrealMcpRegisteredResource
{
	FString Uri;        // the resource's MCP uri — its identity (registry key) and route
	FString Name;
	FString Description;
	FString MimeType;
	bool bEnabled = true;
	FString ExtensionId = TEXT("core");
	FString SchemaHash;
	FUnrealMcpResourceHandler Handler;

	/** The resource-manifest descriptor object (one entry in resource-manifest.resources[]). */
	TSharedPtr<FJsonObject> ToDescriptorJson() const;
};

class FUnrealMcpResourceRegistry;

/** Fluent declaration builder (§3.3 resource analog). One per resource; commits on Read(). */
class UNREALMCPRUNTIME_API FUnrealMcpResourceBuilder
{
public:
	FUnrealMcpResourceBuilder(FUnrealMcpResourceRegistry& InRegistry, const FString& InUri);

	FUnrealMcpResourceBuilder& Name(const FString& InName);
	FUnrealMcpResourceBuilder& Description(const FString& InDescription);
	FUnrealMcpResourceBuilder& MimeType(const FString& InMimeType);
	FUnrealMcpResourceBuilder& ExtensionId(const FString& InExtensionId);

	/** Bind the read handler and commit the resource into the registry. */
	void Read(FUnrealMcpResourceHandler InHandler);

private:
	FUnrealMcpResourceRegistry& Registry;
	FUnrealMcpRegisteredResource Resource;
};

/**
 * The plugin-owned resource registry (docs/ARCHITECTURE.md §A.1) — the exact resource sibling of
 * FUnrealMcpToolRegistry / FUnrealMcpPromptRegistry. Holds the compiled resource set, produces the JSON
 * manifest snapshot for the bridge, and dispatches resource-read requests by URI. A monotonic Revision
 * bumps on every change so the sidecar's manifest diff can reason about ordering. Not thread-safe (same
 * contract as the tool/prompt registries): all mutation happens at startup before the bridge accepts; any
 * DYNAMIC re-registration MUST marshal both the mutation and the manifest read through the game-thread
 * dispatcher (§4).
 */
class UNREALMCPRUNTIME_API FUnrealMcpResourceRegistry
{
public:
	/** Begin declaring a resource (the §3.3 fluent API, resource analog). @p Uri is the resource's identity. */
	FUnrealMcpResourceBuilder Resource(const FString& Uri);

	/**
	 * Commit a fully-built resource (called by the builder's Read()).
	 * - Core path (default): replaces any same-uri resource — core families are trusted.
	 * - Extension scope (inside RegisterExtension): the resource is stamped with the scope's ExtensionId,
	 *   validated (§5), and deduped. An invalid entry is DROPPED and a duplicate is REJECTED (first-wins).
	 */
	void Commit(FUnrealMcpRegisteredResource&& InResource);

	/**
	 * Register an extension provider's resources (§5). Opens an "extension scope": every Commit during
	 * @p RegisterFn is stamped with @p ExtensionId, validated, and deduped. Reuses the tool registry's
	 * FUnrealMcpExtensionRegistrationResult (its `ToolsRegistered` count here means "entries registered").
	 * Scopes never nest.
	 */
	FUnrealMcpExtensionRegistrationResult RegisterExtension(const FString& ExtensionId, TFunctionRef<void(FUnrealMcpResourceRegistry&)> RegisterFn);

	/** Remove every resource stamped with @p ExtensionId (hot-unload / rebuild, §5). Bumps revision if any removed. Returns count removed. */
	int32 RemoveResourcesForExtension(const FString& ExtensionId);

	/** True iff @p Uri is a valid resource URI (non-empty, no control/whitespace chars). */
	static bool IsValidResourceUri(const FString& Uri);

	/** Validate a resource descriptor for the §5 isolation contract. Returns false + a reason on the first problem. */
	static bool ValidateResource(const FUnrealMcpRegisteredResource& InResource, FString& OutError);

	bool HasResource(const FString& Uri) const { return Resources.Contains(Uri); }
	int32 Num() const { return Resources.Num(); }
	const FUnrealMcpRegisteredResource* Find(const FString& Uri) const { return Resources.Find(Uri); }

	/** Every registered resource URI, sorted. */
	TArray<FString> GetResourceUrisSorted() const;

	/** Count of resources whose bEnabled flag is set. */
	int32 NumEnabled() const;

	/** True iff @p Uri passes the §8 EnabledResources whitelist gate (empty whitelist, or the uri is listed). */
	bool PassesEnabledResourcesWhitelist(const FString& Uri) const;

	/**
	 * Set one resource's enabled flag directly — a GRANULAR helper, NOT a production §7 toggle. Does NOT
	 * update the retained whitelist/blocklist. Disabled resources are EXCLUDED from BuildManifestJson.
	 * Bumps the revision on a real change. Unknown uri / no-op change: returns false, no bump. Game-thread only.
	 */
	bool SetResourceEnabled(const FString& Uri, bool bEnabled);

	/** Set the §8 env whitelist. Mirrors the tool registry's SetEnabledToolsFilter exactly. Game-thread only. */
	void SetEnabledResourcesFilter(const TArray<FString>& EnabledResources);

	/** Apply the persisted §7 per-resource enable-map (blocklist). Mirrors the tool registry's ApplyDisabledTools. Game-thread only. */
	void ApplyDisabledResources(const TArray<FString>& DisabledUris);

	/** Current manifest revision (bumps on every registry mutation). */
	int32 GetRevision() const { return Revision; }

	/** Build the full resource-manifest message body (revision + resources[]) for the bridge (§A.1). Disabled resources excluded, sorted by uri. */
	TSharedPtr<FJsonObject> BuildManifestJson() const;

	/**
	 * Read a resource by URI on the CURRENT thread (the dispatcher has already marshalled to the game
	 * thread). Returns an error result for an unknown / disabled resource. The handler owns content production.
	 */
	FUnrealMcpResourceResult Read(const FString& Uri) const;

	/** Compute the stable schema hash for a resource descriptor (excludes the enabled flag). */
	static FString ComputeSchemaHash(const FUnrealMcpRegisteredResource& InResource);

private:
	TMap<FString, FUnrealMcpRegisteredResource> Resources;
	int32 Revision = 0;

	// --- Retained §7/§8 enablement filters (mirror the tool/prompt registries exactly). ---
	TSet<FString> EnabledResourcesWhitelist; // §8 whitelist; empty = no filter
	TSet<FString> DisabledResourceUris;      // §7 persisted blocklist

	/** The combined §8 effective-served predicate: enabled iff (whitelist empty OR member) AND NOT blocklisted. */
	bool ShouldResourceBeEnabled(const FString& Uri) const;

	/** Re-evaluate every registered resource's bEnabled against ShouldResourceBeEnabled; bumps the revision once if any changed. */
	void RecomputeEnablement();

	// --- Extension-scope state (active only inside RegisterExtension; scopes never nest) ----------
	bool bExtensionScope = false;
	FString ScopeExtensionId;
	int32 ScopeResourcesRegistered = 0;
	TArray<FString> ScopeErrors;
};
