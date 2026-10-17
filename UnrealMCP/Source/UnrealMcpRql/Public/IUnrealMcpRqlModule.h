// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Features/IModularFeature.h"
#include "Templates/Function.h"

class UClass;
class UObject;
class UStruct;

/**
 * What an RQL module contributes to the editor's RQL engine (docs/rql.md "Extensions"). UE types only: the RQL
 * runtime is private to the UnrealMcpRql module.
 *
 * Every reflected type is already queryable without a module: UClass / UScriptStruct / UEnum members come from
 * FProperty (EditAnywhere / BlueprintVisible), so a module adds what reflection does not show — sources (where agents
 * start a query), operations (`call`), extra read-only members and the agent guide rendered into the `unreal-rql` skill.
 */
class UNREALMCPRQL_API FUnrealMcpRqlModuleContext
{
public:
	virtual ~FUnrealMcpRqlModuleContext() = default;

	/** A source whose value is one object, e.g. an operations object: `niagara | call SpawnSystem(...)`. */
	virtual void AddObjectSource(const FString& Name, const FString& Description, TFunction<UObject*()> Getter) = 0;

	/** A source whose value is a list of objects, e.g. the Landscape actors of the editor world. */
	virtual void AddObjectListSource(const FString& Name, const FString& Description, TFunction<TArray<UObject*>()> Getter) = 0;

	/** A source listing the project assets of @p AssetClass (and subclasses), loaded lazily, at most 200 per query. */
	virtual void AddAssetSource(const FString& Name, const FString& Description, UClass* AssetClass) = 0;

	/**
	 * Make every UFUNCTION declared on @p Class callable with `call` (an operations class — its parameters and return value
	 * are converted like members). Usually the class default object is a source (AddObjectSource). @p TypeName renames
	 * the type for agents (default: the class name). A function reports a failure with UnrealMcpRql::Fail.
	 */
	virtual void AddApiClass(UClass* Class, const FString& TypeName = FString()) = 0;

	/** Expose properties that reflection hides (not EditAnywhere / BlueprintVisible) as members of @p Owner. */
	virtual void ExposeProperties(UStruct* Owner, const TArray<FName>& PropertyNames, bool bWritable = false) = 0;

	/** A read-only member of @p Class whose value is JSON (numbers, strings, lists, objects) computed from the instance. */
	virtual void AddJsonMember(UClass* Class, const FString& Name, const FString& Description,
		TFunction<TSharedPtr<FJsonValue>(UObject*)> Getter) = 0;

	/** A read-only member of @p Class listing objects (an owning list: `recurse` and `get` follow it). */
	virtual void AddObjectListMember(UClass* Class, const FString& Name, const FString& Description, UClass* ElementClass,
		TFunction<TArray<UObject*>(UObject*)> Getter) = 0;

	/** Markdown rendered into the `unreal-rql` skill under the heading @p Topic. */
	virtual void AddGuide(const FString& Topic, const FString& Markdown) = 0;
};

/**
 * The extension seam of the RQL data layer: an extension registers an implementation as a modular feature under
 * GetModularFeatureName() (IModularFeatures::Get().RegisterModularFeature) and unregisters it on shutdown. The editor
 * rebuilds its RQL engine and republishes the rql-* tools whenever the set of modules changes.
 */
class UNREALMCPRQL_API IUnrealMcpRqlModule : public IModularFeature
{
public:
	static FName GetModularFeatureName();

	virtual ~IUnrealMcpRqlModule() = default;

	/** Stable module name shown in the `unreal-rql` skill, e.g. "Niagara". */
	virtual FString GetRqlModuleName() const = 0;

	/** The extension that ships the module (reverse-DNS id, e.g. "com.ivanmurzak.unreal-ai-niagara"); empty for none. */
	virtual FString GetExtensionId() const { return FString(); }

	/** The extension's version, kept equal to its .uplugin VersionName (scripts/extension_release.py checks it). */
	virtual FString GetExtensionVersion() const { return FString(); }

	/** Register sources, operations, members and guides. Runs on the game thread whenever the engine is (re)built. */
	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) = 0;
};

namespace UnrealMcpRql
{
	/** A class by short name ("PointLight", "AStaticMeshActor"), class path or Blueprint asset path; nullptr when none. */
	UNREALMCPRQL_API UClass* ResolveClass(const FString& Name);

	/** An object of @p Class by path ("/Game/Materials/M_Red" or its object path), loaded quietly; nullptr when none. */
	UNREALMCPRQL_API UObject* LoadObject(UClass* Class, const FString& Path);

	/**
	 * Fail the UFUNCTION that RQL is calling right now with @p Message (UFUNCTIONs cannot throw). The `call` stage reports
	 * it as an error. Changes the function already made stay (as with any failing call); validate before changing.
	 */
	UNREALMCPRQL_API void Fail(const FString& Message);
}
