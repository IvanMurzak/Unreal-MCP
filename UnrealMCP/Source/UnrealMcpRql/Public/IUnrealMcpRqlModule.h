// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Features/IModularFeature.h"
#include "Features/IModularFeatures.h"
#include "Modules/ModuleInterface.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

#include <type_traits>

class AActor;
class UActorComponent;
class UClass;
class UObject;
class UStruct;
class UWorld;

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

	/**
	 * A source whose value is one object of @p Class, e.g. an operations object: `niagara | call SpawnSystem(...)`.
	 * @p Class types the source in rql-schema (the getter is not run for it).
	 */
	virtual void AddObjectSource(const FString& Name, const FString& Description, UClass* Class, TFunction<UObject*()> Getter) = 0;

	/** A source whose value is a list of @p ElementClass objects, e.g. the Landscape actors of the editor world. */
	virtual void AddObjectListSource(const FString& Name, const FString& Description, UClass* ElementClass,
		TFunction<TArray<UObject*>()> Getter) = 0;

	/** A source listing the project assets of @p AssetClass (and subclasses), loaded lazily, at most 200 per query. */
	virtual void AddAssetSource(const FString& Name, const FString& Description, UClass* AssetClass) = 0;

	/**
	 * A source listing the class default objects of the subclasses of @p BaseClass: the loaded classes (abstract ones left
	 * out) and the project's Blueprint classes, sorted by class name, at most 200. Blueprint classes that are not loaded
	 * yet load when the query reads them (`gasAbilities | first 3` loads three).
	 */
	virtual void AddClassDefaultsSource(const FString& Name, const FString& Description, UClass* BaseClass) = 0;

	/**
	 * Make every UFUNCTION declared on @p Class callable with `call` (an operations class — its parameters and return value
	 * are converted like members). Usually the class default object is a source (AddObjectSource). @p TypeName renames
	 * the type for agents (default: the class name). A function reports a failure with UnrealMcpRql::Fail.
	 * A function that changes no saved data (a ground read, a lookup) declares `meta = (RqlReadOnly)` on its UFUNCTION: rql-query
	 * may then call it (every other call needs rql-mutate), it opens no undo step, marks nothing unsaved, may run on the
	 * same arguments more than once, and rql-schema marks it read-only.
	 */
	virtual void AddApiClass(UClass* Class, const FString& TypeName = FString()) = 0;

	/** AddApiClass(@p ApiClass, @p TypeName) plus a source whose value is its class default object: `niagara | call ...`. */
	void AddApiSource(const FString& Name, const FString& Description, UClass* ApiClass, const FString& TypeName = FString());

	/** Expose properties that reflection hides (not EditAnywhere / BlueprintVisible) as members of @p Owner. */
	virtual void ExposeProperties(UStruct* Owner, const TArray<FName>& PropertyNames, bool bWritable = false) = 0;

	/** A read-only member of @p Class whose value is JSON (numbers, strings, lists, objects) computed from the instance. */
	virtual void AddJsonMember(UClass* Class, const FString& Name, const FString& Description,
		TFunction<TSharedPtr<FJsonValue>(UObject*)> Getter) = 0;

	/** A read-only member of @p Class holding one @p ObjectClass object (null when none), e.g. a component's bound asset. */
	virtual void AddObjectMember(UClass* Class, const FString& Name, const FString& Description, UClass* ObjectClass,
		TFunction<UObject*(UObject*)> Getter) = 0;

	/** A read-only member of @p Class listing objects (an owning list: `recurse` and `get` follow it). */
	virtual void AddObjectListMember(UClass* Class, const FString& Name, const FString& Description, UClass* ElementClass,
		TFunction<TArray<UObject*>(UObject*)> Getter) = 0;

	/** Markdown rendered into the `unreal-rql` skill under the heading @p Topic. */
	virtual void AddGuide(const FString& Topic, const FString& Markdown) = 0;
};

/**
 * The extension seam of the RQL data layer: an extension registers an implementation as a modular feature under
 * GetModularFeatureName() (IModularFeatures::Get().RegisterModularFeature) and unregisters it on shutdown. The editor's
 * extension manager republishes the rql-* tools whenever the set of modules changes; an extension disabled in the
 * Extensions window contributes nothing (GetExtensionId names it there).
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

	/**
	 * The extension's version: return UnrealMcpRql::PluginVersion(TEXT("<plugin>")), its .uplugin VersionName
	 * (scripts/extension_release.py checks that the first-party extensions do).
	 */
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

	/**
	 * Add a warning to the answer of the UFUNCTION that RQL is calling right now (the call still succeeds and returns its
	 * value): something the caller should know about what it did, e.g. "the river's bed cuts the road 'Road' at ...". The
	 * answer lists it under `diagnostics` (code `call-warning`), once per message. Outside an RQL call it does nothing.
	 */
	UNREALMCPRQL_API void Warn(const FString& Message);

	/** Fail(@p Message) and return the "nothing" value of the function: `return UnrealMcpRql::Failed<AActor*>(TEXT("..."));`. */
	template <typename T = void>
	T Failed(const FString& Message)
	{
		Fail(Message);
		if constexpr (!std::is_void_v<T>)
			return T();
	}

	/** The editor world (null outside the editor). */
	UNREALMCPRQL_API UWorld* EditorWorld();

	/** The VersionName of the plugin @p PluginName (its .uplugin): GetExtensionVersion() returns it. Empty when unknown. */
	UNREALMCPRQL_API FString PluginVersion(const FString& PluginName);

	/**
	 * Spawn an actor of @p Class in the current level of the editor world (undoable within the RQL mutation), labelled
	 * @p Label when given and attached to @p Parent when given. nullptr after Fail() when it cannot.
	 */
	UNREALMCPRQL_API AActor* SpawnActor(UClass* Class, const FVector& Location, const FRotator& Rotation,
		const FString& Label = FString(), AActor* Parent = nullptr);

	/**
	 * Add an instance component of @p Class to @p Actor (named @p Name, or a unique name when empty; scene components
	 * attach to the root), as `actors | call AddComponent(...)` does. @p Configure runs before the component registers.
	 * nullptr after Fail() when it cannot.
	 */
	UNREALMCPRQL_API UActorComponent* AddComponent(AActor* Actor, UClass* Class, const FString& Name = FString(),
		TFunction<void(UActorComponent*)> Configure = nullptr);

	/**
	 * World Partition: load actors of the editor world that are not loaded, by actor GUID (a descriptor's GetGuid()), as
	 * `editor | call LoadActors` does: through the editor's actor-list loader (UWorldPartitionBlueprintLibrary::LoadActors,
	 * what Python's load_actors calls). Loading changes no saved data: no package is marked unsaved and no undo step is
	 * added, so a read-only operation (meta = (RqlReadOnly)) may call it. The actors stay loaded until the world goes.
	 * Synchronous, and only what the editor allows: GUIDs of loaded actors, of actors deleted in this session (their
	 * descriptor stays until save), of runtime-only descriptors and unknown GUIDs are skipped, and an actor in a data layer
	 * the editor has not loaded stays unloaded; check IsLoaded() of the descriptor (or the region's OutLoaded) when it
	 * matters. Nothing happens without World Partition or while a play world is the active world (the loader loads into
	 * the editor world only). True then too; false with @p OutError only when this engine has no such loader.
	 */
	UNREALMCPRQL_API bool LoadWorldPartitionActors(const TArray<FGuid>& Guids, FString* OutError = nullptr);

	/**
	 * World Partition: load every actor of the editor world that is not loaded and whose editor bounds overlap @p Region in
	 * XY (cm; Z ignored, as region loading in the World Partition window), as `editor | call LoadRegion` does; same loader,
	 * skips and guarantees as LoadWorldPartitionActors. @p OutLoaded, when given, is reset and receives only the actors this
	 * call loaded (actors already loaded in the region are not listed). False with @p OutError for an invalid or inverted box.
	 */
	UNREALMCPRQL_API bool LoadWorldPartitionRegion(const FBox2D& Region, TArray<AActor*>* OutLoaded = nullptr, FString* OutError = nullptr);

	/**
	 * Create an asset of @p Class at @p Path ("/Game/Input/IA_Jump", project or plugin content, not taken yet), with the
	 * Content Browser's "New" factory when one creates the class, as `editor | call CreateAsset(...)` does. Unsaved.
	 * nullptr after Fail() when it cannot.
	 */
	UNREALMCPRQL_API UObject* CreateAsset(UClass* Class, const FString& Path);
}

/**
 * The editor module of an extension whose only job is its RQL module: owns a @p TRqlModule and registers it as the
 * IUnrealMcpRqlModule modular feature for the module's lifetime.
 *   IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FMyRqlModule>, MyExtension)
 */
template <typename TRqlModule>
class TUnrealMcpRqlEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RqlModule = MakeUnique<TRqlModule>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
	}

	virtual void ShutdownModule() override
	{
		if (RqlModule.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
			RqlModule.Reset();
		}
	}

private:
	TUniquePtr<TRqlModule> RqlModule;
};
