// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpObjectRef.h"
#include "UnrealMcpPropertyJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"                  // GEditor->NewMap() — headless-safe in-memory world (AutomationEditorCommon uses it)
#include "FileHelpers.h"            // UEditorLoadingAndSavingUtils — NewMapFromTemplate / LoadMap / SaveMap / SaveCurrentLevel (UnrealEd)
#include "EditorLevelUtils.h"       // UEditorLevelUtils::MakeLevelCurrent / RemoveLevelFromWorld (UnrealEd)
#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

/**
 * The level / map tool family (docs/ARCHITECTURE.md §10 "level family", the Unity Scene.* analog),
 * declared via §3.3. Seven native C++ tools over the world / map editor surface:
 *   level-create, level-open, level-save, level-get-data, level-list-loaded, level-set-current,
 *   level-unload-sublevel.
 *
 * Headless-safety (the binding lesson carried from the actor family, issue #16):
 *   - UEditorActorSubsystem AND ULevelEditorSubsystem (LevelEditor module) are avoided. We touch the
 *     editor UWorld directly (GEditor->NewMap, UEditorLoadingAndSavingUtils, UEditorLevelUtils — all
 *     Engine/UnrealEd, already linked), so every body below is verified under -nullrhi automation.
 *   - level-create's in-memory path uses GEditor->NewMap() (exactly what FAutomationEditorCommonUtils
 *     does) so specs create worlds without writing a .umap to the testbed (working-tree stays clean).
 *     Saving to disk happens only when the caller passes 'path' (save-as) and in the live bridge e2e.
 *   - World-partition CREATION needs ULevelEditorSubsystem::NewLevel(path, true) from the LevelEditor
 *     module; that module dependency (and its headless behaviour) is out of this family's MVP scope, so
 *     level-create produces standard levels and World-Partition is surfaced READ-ONLY by
 *     level-list-loaded per §10 ("World Partition read-only awareness").
 *
 * Every Handle() body runs ON the game thread (the dispatcher marshals Registry.Execute, §4), so the
 * bodies touch the editor world / UObject graph directly. Reuses FUnrealMcpObjectRef (§3.2 path-or-name
 * refs) and FUnrealMcpPropertyJson (scoped reads) from the actor family for level-get-data.
 */
namespace
{
	/** An `array` of strings — the §3.2 scoped-read `paths` filter (mirrors the actor family's local schema).
	 *  Uniquely named (Level-prefixed): the UE unity build concatenates this TU with the other tool
	 *  families' TUs, so a bare `MakeStringArraySchema` would ODR-collide with the actor family's copy. */
	TSharedPtr<FJsonObject> LevelMakeStringArraySchema(const FString& Desc)
	{
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("array"));
		if (!Desc.IsEmpty())
			Schema->SetStringField(TEXT("description"), Desc);
		Schema->SetObjectField(TEXT("items"), Items);
		return Schema;
	}

	/**
	 * Read a string array argument (the scoped-read `paths` filter); empty when absent or not an array.
	 * A non-string entry is reported via OutError (and the array is left empty) rather than silently
	 * dropped — a dropped entry would otherwise flip the scoped read to "identity only" (the opposite of
	 * the requested scope) with no signal to the caller. Same contract as the actor family's helper.
	 * Uniquely named (Level-prefixed) to avoid a unity-build ODR collision with the actor family's copy.
	 */
	TArray<FString> LevelGetStringArray(const FUnrealMcpToolCall& Call, const FString& Key, FString& OutError)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Call.Arguments->TryGetArrayField(Key, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					Out.Add(S);
				}
				else
				{
					OutError = FString::Printf(TEXT("'%s' must be an array of strings; a non-string entry was provided."), *Key);
					Out.Reset();
					return Out;
				}
			}
		}
		return Out;
	}

	/** Long package name of a level's outermost package (e.g. /Game/Maps/Arena); empty when null. */
	FString LevelPackageName(const ULevel* Level)
	{
		if (!Level)
			return FString();
		const UPackage* Package = Level->GetOutermost();
		return Package ? Package->GetName() : FString();
	}

	/** Short, content-browser-style name of a level (e.g. "Arena"); empty when null. */
	FString LevelShortName(const ULevel* Level)
	{
		const FString PackageName = LevelPackageName(Level);
		return PackageName.IsEmpty() ? FString() : FPackageName::GetShortName(PackageName);
	}

	/** A `{ name, package, isPersistent, isCurrent }` identity block for a level loaded in @p World. */
	TSharedPtr<FJsonObject> LevelIdentity(const ULevel* Level, const UWorld* World)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), LevelShortName(Level));
		Out->SetStringField(TEXT("package"), LevelPackageName(Level));
		const bool bPersistent = World && Level == World->PersistentLevel;
		Out->SetBoolField(TEXT("isPersistent"), bPersistent);
		Out->SetBoolField(TEXT("isCurrent"), World && Level == World->GetCurrentLevel());
		return Out;
	}

	/** The editor world for level tools; null outside the editor (mirrors FUnrealMcpObjectRef::GetEditorWorld). */
	UWorld* GetWorldOrNull()
	{
		return FUnrealMcpObjectRef::GetEditorWorld();
	}

	/**
	 * Resolve a level asset path (object path like '/Game/Maps/Arena.Arena' or a plain package name like
	 * '/Game/Maps/Arena') to the on-disk .umap filename, returning false + a reason when the path is
	 * malformed or no package exists. Used to turn an expected-miss (typo'd path / template) into a clean
	 * structured error BEFORE calling an engine API that would otherwise log an Error (an Automation
	 * failure) or — for NewMapFromTemplate — silently fall back to a blank map.
	 */
	bool ResolveLevelFilename(const FString& AssetPath, FString& OutFilename, FString& OutError)
	{
		FString PackageName = AssetPath;
		if (FPackageName::IsValidObjectPath(AssetPath))
			PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid level asset path."), *AssetPath);
			return false;
		}
		if (!FPackageName::DoesPackageExist(PackageName, &OutFilename))
		{
			OutError = FString::Printf(TEXT("No level package exists at '%s'."), *PackageName);
			return false;
		}
		// DoesPackageExist is content-agnostic: a non-level asset (material, blueprint, …) also passes. Without
		// this guard NewMapFromTemplate -> LoadMap would silently fall back to a blank map (and level-open would
		// log an engine Error). Require the resolved package to be a .umap.
		if (!OutFilename.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("'%s' is not a level/map asset."), *PackageName);
			OutFilename.Reset();
			return false;
		}
		return true;
	}

	/** Names of dirty (unsaved) map packages that a New/Open Level would silently discard — empty when none.
	 *  Surfaced as a structured `discardedDirtyLevels` note so an unattended/headless caller (where the engine's
	 *  own save-prompt is suppressed) still learns that unsaved level edits were dropped. */
	TArray<TSharedPtr<FJsonValue>> DirtyMapPackageNames()
	{
		TArray<UPackage*> DirtyPackages;
		UEditorLoadingAndSavingUtils::GetDirtyMapPackages(DirtyPackages);
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const UPackage* Package : DirtyPackages)
			if (Package)
				Out.Add(MakeShared<FJsonValueString>(Package->GetName()));
		return Out;
	}

	/** Normalise a save-as asset path (object-path or package-name form) to a long package name, validating it;
	 *  returns false + a reason when malformed. Sets bOutExists when a package already exists at the target, so
	 *  the caller can surface an overwrite (SaveMap force-overwrites silently otherwise). */
	bool ResolveSaveTargetPackage(const FString& AssetPath, FString& OutPackageName, bool& bOutExists, FString& OutError)
	{
		OutPackageName = AssetPath;
		if (FPackageName::IsValidObjectPath(AssetPath))
			OutPackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FPackageName::IsValidLongPackageName(OutPackageName))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid level asset path (expected e.g. '/Game/Maps/Arena')."), *AssetPath);
			return false;
		}
		// Probe for a pre-existing package AND verify it is a .umap (content-aware, like ResolveLevelFilename).
		// SaveMap writes <name>.umap; if a NON-map asset (material, blueprint, …) already claims this long
		// package name, saving would leave two on-disk files (Arena.umap + Arena.uasset) under one package name
		// — an asset-registry/cooker ambiguity — while we'd wrongly report overwrote:true. Reject that collision.
		FString ExistingFilename;
		bOutExists = FPackageName::DoesPackageExist(OutPackageName, &ExistingFilename);
		if (bOutExists && !ExistingFilename.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("'%s' already exists and is not a level/map asset; choose a different path."), *OutPackageName);
			return false;
		}
		return true;
	}
}

namespace UnrealMcpLevelTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		// ----------------------------------------------------------------------------------------
		// level-create — new (in-memory) level, optionally from a template, optionally saved to a path.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("level-create"))
			.Title(TEXT("Create Level"))
			.Description(TEXT("Create a new, empty level and make it the active editor world. Optionally seed it "
			                  "from an existing level via 'template' (asset path), and optionally persist it to "
			                  "disk via 'path' (asset path, e.g. '/Game/Maps/Arena') — omit 'path' to leave the "
			                  "new level transient/in-memory. Replaces the current editor world (its undo buffer "
			                  "is reset, as with any New/Open Level)."))
			.ParamString(TEXT("path"), TEXT("Asset path to save the new level to (e.g. '/Game/Maps/Arena'). Omit to leave it transient/in-memory."))
			.ParamString(TEXT("template"), TEXT("Asset path of an existing level to seed the new one from. Omit for a blank level."))
			// Destructive: replacing the editor world discards the current world's unsaved edits, and a
			// create-with-path silently overwrites an existing .umap (hence `discardedDirtyLevels`/`overwrote`).
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				if (!GEditor)
					return FUnrealMcpToolResult::Error(TEXT("No editor available (not running in the editor)."));

				const FString Template = Call.GetString(TEXT("template"));
				const FString SavePath = Call.GetString(TEXT("path"));

				// Capture unsaved map edits BEFORE replacing the world — New/Open Level discards them and the
				// engine's confirm dialog is suppressed under -unattended, so this note is the only signal.
				TArray<TSharedPtr<FJsonValue>> DiscardedDirty = DirtyMapPackageNames();

				// Validate the save-as target BEFORE replacing the world. ResolveSaveTargetPackage is pure
				// validation (no side effects); doing it up front means a malformed 'path' fails fast instead of
				// destroying the caller's open world (and its unsaved edits) only to then reject the bad path.
				bool bOverwrote = false;
				FString SavedPackage;
				if (!SavePath.IsEmpty())
				{
					FString SaveError;
					if (!ResolveSaveTargetPackage(SavePath, SavedPackage, bOverwrote, SaveError))
						return FUnrealMcpToolResult::Error(SaveError);
				}

				UWorld* NewWorld = nullptr;
				if (!Template.IsEmpty())
				{
					// Validate the template package exists FIRST: NewMapFromTemplate silently falls back to
					// a blank map when the template is missing (it does NOT return null), so a typo'd template
					// would otherwise produce a misleading "success" with an empty world. Probe up front and
					// fail with a structured error instead.
					FString TemplateFilename, TemplateError;
					if (!ResolveLevelFilename(Template, TemplateFilename, TemplateError))
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("Could not create a level from template '%s': %s"), *Template, *TemplateError));

					// Pass the resolved on-disk filename (not the raw asset path): NewMapFromTemplate forwards it
					// to FEditorFileUtils::LoadMap, exactly as level-open does with its resolved filename.
					NewWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(TemplateFilename, /*bSaveExistingMap*/ false);
					if (!NewWorld)
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("Could not create a level from template '%s' (template found but invalid)."), *Template));
				}
				else
				{
					// GEditor->NewMap() is the headless-safe in-memory world creation the engine's own
					// automation harness uses (FAutomationEditorCommonUtils::CreateNewMap); no disk write.
					NewWorld = GEditor->NewMap();
					if (!NewWorld)
						return FUnrealMcpToolResult::Error(TEXT("Failed to create a new level (GEditor->NewMap returned null)."));
				}

				// Save target was validated above; now that the world exists, write it to disk. SaveMap force-
				// overwrites an existing .umap silently — the collision was already detected into `bOverwrote`.
				bool bSaved = false;
				if (!SavePath.IsEmpty())
				{
					bSaved = UEditorLoadingAndSavingUtils::SaveMap(NewWorld, SavedPackage);
					if (!bSaved)
					{
						// The world was already replaced (and the previous world's unsaved edits discarded) before this
						// save attempt; an Error result carries no structured payload, so name the discarded packages in
						// the message text rather than dropping that signal silently.
						FString DiscardedSuffix;
						if (DiscardedDirty.Num() > 0)
						{
							TArray<FString> DiscardedNames;
							for (const TSharedPtr<FJsonValue>& V : DiscardedDirty)
								if (V.IsValid())
									DiscardedNames.Add(V->AsString());
							DiscardedSuffix = FString::Printf(
								TEXT(" Unsaved edits from the previous world were already discarded: %s."),
								*FString::Join(DiscardedNames, TEXT(", ")));
						}
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("Created the level but failed to save it to '%s' (invalid path or save was declined).%s"),
							*SavedPackage, *DiscardedSuffix));
					}
				}

				ULevel* Persistent = NewWorld->PersistentLevel;
				TSharedPtr<FJsonObject> Structured = LevelIdentity(Persistent, NewWorld);
				Structured->SetBoolField(TEXT("saved"), bSaved);
				if (bSaved)
				{
					Structured->SetStringField(TEXT("savedPath"), SavedPackage);
					Structured->SetBoolField(TEXT("overwrote"), bOverwrote);
				}
				if (DiscardedDirty.Num() > 0)
					Structured->SetArrayField(TEXT("discardedDirtyLevels"), DiscardedDirty);

				const FString What = Template.IsEmpty() ? TEXT("blank level") : FString::Printf(TEXT("level from template '%s'"), *Template);
				const FString Where = bSaved
					? FString::Printf(TEXT(" and saved to '%s'%s"), *SavedPackage, bOverwrote ? TEXT(" (overwrote an existing level)") : TEXT(""))
					: TEXT(" (transient — pass 'path' to save)");
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Created a %s%s."), *What, *Where), Structured);
			});

		// ----------------------------------------------------------------------------------------
		// level-open — open an existing level by asset path.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("level-open"))
			.Title(TEXT("Open Level"))
			.Description(TEXT("Open an existing level as the active editor world, replacing the current one. "
			                  "Identify it by asset path (e.g. '/Game/Maps/Arena')."))
			.ParamString(TEXT("path"), TEXT("Asset path of the level to open (e.g. '/Game/Maps/Arena')."), EUnrealMcpParamRequirement::Required)
			// Destructive: opening a level replaces the editor world and discards the current world's
			// unsaved edits (hence `discardedDirtyLevels`).
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// UEditorLoadingAndSavingUtils::LoadMap dereferences GEditor internally; guard it the same way
				// level-create guards GEditor->NewMap. The other tools reach the editor via GetWorldOrNull, but
				// level-open REPLACES the world, so there is no current world to probe — check GEditor directly.
				if (!GEditor)
					return FUnrealMcpToolResult::Error(TEXT("No editor available (not running in the editor)."));

				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (asset path of the level to open)."));

				// Normalise to a long package name and resolve the .umap on disk. Probing existence up
				// front turns an expected-miss into a clean structured error instead of an engine Error
				// (which Automation would count as a failure) or a load of the wrong file.
				FString Filename, ResolveError;
				if (!ResolveLevelFilename(Path, Filename, ResolveError))
					return FUnrealMcpToolResult::Error(ResolveError);

				// Capture unsaved map edits BEFORE the load replaces the world (see level-create).
				TArray<TSharedPtr<FJsonValue>> DiscardedDirty = DirtyMapPackageNames();

				UWorld* Opened = UEditorLoadingAndSavingUtils::LoadMap(Filename);
				if (!Opened)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to open level '%s'."), *Path));

				TSharedPtr<FJsonObject> Structured = LevelIdentity(Opened->PersistentLevel, Opened);
				if (DiscardedDirty.Num() > 0)
					Structured->SetArrayField(TEXT("discardedDirtyLevels"), DiscardedDirty);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Opened level '%s'."), *LevelShortName(Opened->PersistentLevel)),
					Structured);
			});

		// ----------------------------------------------------------------------------------------
		// level-save — save the current level; save-as via optional 'path'.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("level-save"))
			.Title(TEXT("Save Level"))
			.Description(TEXT("Save the current editor level. Pass 'path' (asset path) to save-as a copy of the "
			                  "PERSISTENT level to a new location; omit it to save the current level (which may be a "
			                  "streaming sublevel) in place. A transient/never-saved level with no 'path' returns an "
			                  "error (provide 'path' to give it a location)."))
			.ParamString(TEXT("path"), TEXT("Asset path to save-as the PERSISTENT level to (e.g. '/Game/Maps/Arena'). Omit to save the current level in place."))
			// Destructive: save-as silently overwrites an existing .umap at the target (hence `overwrote`).
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = GetWorldOrNull();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No editor world available (not running in the editor)."));

				const FString SavePath = Call.GetString(TEXT("path"));
				if (!SavePath.IsEmpty())
				{
					// Normalise + validate the target and probe for a pre-existing level: SaveMap force-
					// overwrites an existing .umap silently, so detect the collision and surface `overwrote`.
					FString SavedPackage, SaveError;
					bool bOverwrote = false;
					if (!ResolveSaveTargetPackage(SavePath, SavedPackage, bOverwrote, SaveError))
						return FUnrealMcpToolResult::Error(SaveError);
					if (!UEditorLoadingAndSavingUtils::SaveMap(World, SavedPackage))
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("Failed to save the level to '%s' (invalid path or save was declined)."), *SavedPackage));
					TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
					Structured->SetStringField(TEXT("savedPath"), SavedPackage);
					Structured->SetBoolField(TEXT("overwrote"), bOverwrote);
					return FUnrealMcpToolResult::Success(
						FString::Printf(TEXT("Saved the current level to '%s'%s."), *SavedPackage,
							bOverwrote ? TEXT(" (overwrote an existing level)") : TEXT("")), Structured);
				}

				// Save-in-place. Guard the transient/never-saved case BEFORE calling SaveCurrentLevel: for a level
				// with no on-disk package, FEditorFileUtils::SaveLevel falls into the interactive Save-As FILE
				// DIALOG (and PromptToCheckoutLevels can raise a checkout dialog) — modal UI that would block the
				// game thread / bridge, violating the handler contract (no modal UI in tool bodies). Probing the
				// current level's package existence keeps this headless-safe in an interactive editor too.
				const FString CurrentPackage = LevelPackageName(World->GetCurrentLevel());
				if (CurrentPackage.IsEmpty() || !FPackageName::DoesPackageExist(CurrentPackage))
					return FUnrealMcpToolResult::Error(
						TEXT("The current level has never been saved (it has no on-disk location yet). "
						     "Pass 'path' to save it to a new location."));

				// The level has a real package on disk; an in-place save will not prompt for a location. A false
				// return now indicates a checkout or write failure rather than the transient case.
				if (!UEditorLoadingAndSavingUtils::SaveCurrentLevel())
					return FUnrealMcpToolResult::Error(
						TEXT("The current level could not be saved in place (a checkout or write failure)."));

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Saved the current level '%s'."), *LevelShortName(World->GetCurrentLevel())));
			});

		// ----------------------------------------------------------------------------------------
		// level-get-data — actor-tree snapshot of the loaded world; scoped reads via 'paths' (§3.2).
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("level-get-data"))
			.Title(TEXT("Get Level Data"))
			.Description(TEXT("Read an actor-tree snapshot of the loaded editor world. Returns each actor's "
			                  "identity, optionally including reflected data scoped to the dotted 'paths' filter "
			                  "(§3.2) to save tokens. Pass 'actor' to scope the read to a single actor instead of "
			                  "the whole world."))
			.ParamString(TEXT("actor"), TEXT("Label / name / path of a single actor to read. Omit to snapshot the whole world."))
			.Param(TEXT("paths"), TEXT("array"), TEXT("Dotted property paths to include per actor (scoped read). Identity only when omitted."), EUnrealMcpParamRequirement::Optional, LevelMakeStringArraySchema(TEXT("Dotted property paths to include per actor (scoped read).")))
			.ParamInt(TEXT("limit"), TEXT("Maximum number of actors to return for a world snapshot. Defaults to 200; pass 0 or a negative value for no limit."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = GetWorldOrNull();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No editor world available."));

				FString PathsError;
				const TArray<FString> Paths = LevelGetStringArray(Call, TEXT("paths"), PathsError);
				if (!PathsError.IsEmpty())
					return FUnrealMcpToolResult::Error(PathsError);

				// Single-actor scope.
				const FString ActorRef = Call.GetString(TEXT("actor"));
				if (!ActorRef.IsEmpty())
				{
					AActor* Actor = FUnrealMcpObjectRef::ResolveActor(ActorRef, World);
					if (!Actor)
						return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No actor matched '%s' (by label/name/path)."), *ActorRef));

					TSharedPtr<FJsonObject> Entry = FUnrealMcpObjectRef::ActorIdentity(Actor);
					// Attach reflected data only when a scoped 'paths' filter is given — matching the whole-world
					// branch and the "Identity only when omitted" contract (an empty Paths returns the full dump).
					if (Paths.Num() > 0)
						Entry->SetObjectField(TEXT("data"), FUnrealMcpPropertyJson::SerializeObject(Actor, Paths));
					return FUnrealMcpToolResult::Success(
						FString::Printf(TEXT("Read actor '%s'."), *Actor->GetActorLabel()), Entry);
				}

				// Whole-world snapshot.
				const int32 Limit = static_cast<int32>(Call.GetInt(TEXT("limit"), 200));
				TArray<TSharedPtr<FJsonValue>> Actors;
				int32 Total = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor)
						continue;
					++Total;
					if (Limit > 0 && Actors.Num() >= Limit)
						continue; // keep counting the total, stop materializing entries

					TSharedPtr<FJsonObject> Entry = FUnrealMcpObjectRef::ActorIdentity(Actor);
					if (Paths.Num() > 0)
						Entry->SetObjectField(TEXT("data"), FUnrealMcpPropertyJson::SerializeObject(Actor, Paths));
					Actors.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = LevelIdentity(World->GetCurrentLevel(), World);
				Structured->SetStringField(TEXT("world"), World->GetName());
				Structured->SetBoolField(TEXT("isPartitionedWorld"), World->IsPartitionedWorld());
				Structured->SetNumberField(TEXT("levelCount"), World->GetLevels().Num());
				Structured->SetNumberField(TEXT("count"), Actors.Num());
				Structured->SetNumberField(TEXT("total"), Total);
				Structured->SetArrayField(TEXT("actors"), Actors);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Snapshot of world '%s': %d actor(s) (returned %d)."), *World->GetName(), Total, Actors.Num()),
					Structured);
			});

		// ----------------------------------------------------------------------------------------
		// level-list-loaded — persistent + streaming sublevels, World-Partition aware (read-only).
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("level-list-loaded"))
			.Title(TEXT("List Loaded Levels"))
			.Description(TEXT("List the levels loaded in the current editor world — the persistent level plus any "
			                  "streaming sublevels — with their load/visibility/current state, and whether the "
			                  "world is a World-Partition world. Read-only."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = GetWorldOrNull();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No editor world available."));

				TArray<TSharedPtr<FJsonValue>> Levels;

				// Persistent level first.
				if (World->PersistentLevel)
				{
					TSharedPtr<FJsonObject> Entry = LevelIdentity(World->PersistentLevel, World);
					Entry->SetBoolField(TEXT("isLoaded"), true);
					Entry->SetBoolField(TEXT("isVisible"), true);
					Levels.Add(MakeShared<FJsonValueObject>(Entry));
				}

				// Streaming sublevels (the §10 "sublevels" awareness). A sublevel that is not currently
				// loaded has a null loaded-level pointer; report its requested/visible flags regardless.
				for (ULevelStreaming* Streaming : World->GetStreamingLevels())
				{
					if (!Streaming)
						continue;
					ULevel* Loaded = Streaming->GetLoadedLevel();
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), FPackageName::GetShortName(Streaming->GetWorldAssetPackageName()));
					Entry->SetStringField(TEXT("package"), Streaming->GetWorldAssetPackageName());
					Entry->SetBoolField(TEXT("isPersistent"), false);
					Entry->SetBoolField(TEXT("isCurrent"), Loaded && Loaded == World->GetCurrentLevel());
					Entry->SetBoolField(TEXT("isLoaded"), Loaded != nullptr);
					Entry->SetBoolField(TEXT("isVisible"), Streaming->GetShouldBeVisibleInEditor());
					Levels.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("world"), World->GetName());
				Structured->SetBoolField(TEXT("isPartitionedWorld"), World->IsPartitionedWorld());
				Structured->SetNumberField(TEXT("count"), Levels.Num());
				Structured->SetArrayField(TEXT("levels"), Levels);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("World '%s' has %d loaded level(s)%s."), *World->GetName(), Levels.Num(),
						World->IsPartitionedWorld() ? TEXT(" (World-Partition)") : TEXT("")),
					Structured);
			});

		// ----------------------------------------------------------------------------------------
		// level-set-current — set the active editing level among the loaded levels.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("level-set-current"))
			.Title(TEXT("Set Current Level"))
			.Description(TEXT("Set the current editing level (the one new actors are added to) by name — the "
			                  "content-browser short name of a loaded level (persistent or streaming sublevel). "
			                  "Use level-list-loaded to discover the available names."))
			.ParamString(TEXT("name"), TEXT("Short name — or full package path, to disambiguate — of the loaded level to make current."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = GetWorldOrNull();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No editor world available."));

				const FString Name = Call.GetString(TEXT("name"));
				if (Name.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'name' (short name of a loaded level)."));

				// Match by short name OR full package path. The package path is unambiguous; a short name that
				// matches more than one loaded level is an error (the caller must pass the package path instead).
				ULevel* Target = nullptr;
				int32 ShortNameMatches = 0;
				for (ULevel* Level : World->GetLevels())
				{
					if (!Level)
						continue;
					if (LevelPackageName(Level).Equals(Name, ESearchCase::IgnoreCase))
					{
						Target = Level;
						ShortNameMatches = 1; // an exact package-path hit is decisive
						break;
					}
					if (LevelShortName(Level).Equals(Name, ESearchCase::IgnoreCase))
					{
						Target = Level;
						++ShortNameMatches;
					}
				}
				if (ShortNameMatches > 1)
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("'%s' matches multiple loaded levels; pass the full package path (e.g. '/Game/Maps/Arena') to disambiguate."), *Name));
				if (!Target)
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("No loaded level named '%s' (use level-list-loaded to see the available names)."), *Name));

				if (World->GetCurrentLevel() == Target)
					return FUnrealMcpToolResult::Success(
						FString::Printf(TEXT("Level '%s' is already the current level."), *LevelShortName(Target)),
						LevelIdentity(Target, World));

				// MakeLevelCurrent updates the editor's current-level state (and the actor-editor context);
				// it is a no-op for a locked level unless forced — we do not force here.
				UEditorLevelUtils::MakeLevelCurrent(Target, /*bEvenIfLocked*/ false);
				if (World->GetCurrentLevel() != Target)
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Could not make '%s' the current level (it may be locked)."), *LevelShortName(Target)));

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Made '%s' the current level."), *LevelShortName(Target)),
					LevelIdentity(Target, World));
			});

		// ----------------------------------------------------------------------------------------
		// level-unload-sublevel — remove a loaded streaming sublevel from the world.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("level-unload-sublevel"))
			.Title(TEXT("Unload Sublevel"))
			.Description(TEXT("Unload (remove from the world) a loaded streaming sublevel by its short name. The "
			                  "persistent level cannot be unloaded. Use level-list-loaded to discover the sublevel "
			                  "names."))
			.ParamString(TEXT("name"), TEXT("Short name — or full package path, to disambiguate — of the streaming sublevel to unload."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = GetWorldOrNull();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No editor world available."));

				const FString Name = Call.GetString(TEXT("name"));
				if (Name.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'name' (short name of a streaming sublevel)."));

				// Search the streaming sublevels FIRST (by short name OR full package path). Doing this before
				// the persistent-level guard keeps a sublevel that happens to share the persistent level's short
				// name reachable via its package path. A short name matching multiple sublevels is an error.
				ULevelStreaming* Match = nullptr;
				int32 ShortNameMatches = 0;
				for (ULevelStreaming* Streaming : World->GetStreamingLevels())
				{
					if (!Streaming)
						continue;
					const FString Package = Streaming->GetWorldAssetPackageName();
					if (Package.Equals(Name, ESearchCase::IgnoreCase))
					{
						Match = Streaming;
						ShortNameMatches = 1; // an exact package-path hit is decisive
						break;
					}
					if (FPackageName::GetShortName(Package).Equals(Name, ESearchCase::IgnoreCase))
					{
						Match = Streaming;
						++ShortNameMatches;
					}
				}
				if (ShortNameMatches > 1)
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("'%s' matches multiple streaming sublevels; pass the full package path to disambiguate."), *Name));
				if (!Match)
				{
					// Not a sublevel: explain the persistent-level case specifically, else a plain not-found.
					if (World->PersistentLevel &&
						(LevelShortName(World->PersistentLevel).Equals(Name, ESearchCase::IgnoreCase) ||
						 LevelPackageName(World->PersistentLevel).Equals(Name, ESearchCase::IgnoreCase)))
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("'%s' is the persistent level and cannot be unloaded."), *Name));
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("No streaming sublevel named '%s' (use level-list-loaded to see the available names)."), *Name));
				}

				ULevel* Loaded = Match->GetLoadedLevel();
				if (!Loaded)
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Sublevel '%s' is not currently loaded; nothing to unload."), *Name));

				// Capture the sublevel's unsaved state BEFORE removing it: RemoveLevelFromWorld discards a dirty
				// sublevel's edits with no prompt under -unattended, so surface `wasDirty` to complete the family's
				// discarded-edits convention (mirrors `discardedDirtyLevels` on level-create / level-open).
				const UPackage* LoadedPackage = Loaded->GetOutermost();
				const bool bWasDirty = LoadedPackage && LoadedPackage->IsDirty();

				if (!UEditorLevelUtils::RemoveLevelFromWorld(Loaded))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to unload sublevel '%s'."), *Name));

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetBoolField(TEXT("wasDirty"), bWasDirty);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Unloaded sublevel '%s'%s."), *Name,
						bWasDirty ? TEXT(" (discarded its unsaved edits)") : TEXT("")),
					Structured);
			});
	}
}
