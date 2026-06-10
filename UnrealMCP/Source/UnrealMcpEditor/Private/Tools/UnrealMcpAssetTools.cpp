// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"
#include "Tools/UnrealMcpAssetScopedRead.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "UObject/TopLevelAssetPath.h"
#include "Misc/Paths.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"

#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetImportTask.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"

#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"

/**
 * The asset / Content-Browser tool family (docs/ARCHITECTURE.md §10 "asset family", issue #10).
 *
 * Eleven kebab-case tools over the AssetRegistry, UEditorAssetLibrary, AssetTools and the editor
 * material APIs. Every handler runs ON the game thread (the dispatcher guarantees it, §4) so it
 * calls the editor subsystems directly with no extra marshalling.
 *
 * Object/asset references follow the §3.2 path-or-name convention — an object path
 * ("/Game/Maps/Arena.Arena") or the equivalent package path ("/Game/Maps/Arena"); both forms are
 * accepted by UEditorAssetLibrary's load/exists primitives, which this family leans on for
 * resolution. Read-only tools (`asset-get-data`, `asset-material-get-data`) support §3.2 scoped
 * reads via the `paths` array (post-serialization JSON filtering, see UnrealMcpAssetScopedRead).
 *
 * Per the implement-task brief, asset-path resolution and the scoped-read helper are kept LOCAL to
 * this family rather than touching a shared FUnrealMcpObjectRef the concurrent actor chain may be
 * introducing; consolidation can happen post-merge.
 */
namespace UnrealMcpAssetTools
{
	// --- Local helpers --------------------------------------------------------------------------

	static IAssetRegistry& GetAssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	static IAssetTools& GetAssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	}

	/** Split "/Game/Foo/Bar" (or the object form "/Game/Foo/Bar.Bar") into "/Game/Foo" + "Bar". */
	static bool SplitObjectPath(const FString& InPath, FString& OutPackagePath, FString& OutAssetName)
	{
		FString Work = InPath;
		// Drop a trailing object suffix (".AssetName") if present — we want the package path form.
		int32 DotIndex;
		if (Work.FindChar(TEXT('.'), DotIndex))
		{
			Work.LeftInline(DotIndex);
		}
		Work.TrimStartAndEndInline();
		while (Work.EndsWith(TEXT("/")))
		{
			Work.LeftChopInline(1);
		}

		int32 SlashIndex;
		if (!Work.FindLastChar(TEXT('/'), SlashIndex) || SlashIndex == 0 || SlashIndex == Work.Len() - 1)
		{
			return false;
		}
		OutPackagePath = Work.Left(SlashIndex);
		OutAssetName = Work.Mid(SlashIndex + 1);
		return !OutAssetName.IsEmpty() && !OutPackagePath.IsEmpty();
	}

	/** Read a string-array argument (returns empty when absent or not an array). */
	static TArray<FString> GetStringArray(const FUnrealMcpToolCall& Call, const FString& Key)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Array;
		if (Call.Arguments->TryGetArrayField(Key, Array))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				FString Str;
				if (Value.IsValid() && Value->TryGetString(Str))
				{
					Result.Add(Str);
				}
			}
		}
		return Result;
	}

	/** The §3.2 scoped-read field list (the `paths` argument) for read-only get-data tools. */
	static TArray<FString> GetScopedReadPaths(const FUnrealMcpToolCall& Call)
	{
		return GetStringArray(Call, TEXT("paths"));
	}

	static TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& Data)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Data.AssetName.ToString());
		Obj->SetStringField(TEXT("path"), Data.GetObjectPathString());
		Obj->SetStringField(TEXT("packageName"), Data.PackageName.ToString());
		Obj->SetStringField(TEXT("packagePath"), Data.PackagePath.ToString());
		Obj->SetStringField(TEXT("class"), Data.AssetClassPath.ToString());
		return Obj;
	}

	static TSharedPtr<FJsonValue> LinearColorToJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return MakeShared<FJsonValueObject>(Obj);
	}

	// --- Tool registration ----------------------------------------------------------------------

	void Register(FUnrealMcpToolRegistry& Registry)
	{
		// asset-find -------------------------------------------------------------------------------
		Registry.Tool(TEXT("asset-find"))
			.Title(TEXT("Find Assets"))
			.Description(TEXT("Query the Content Browser AssetRegistry with optional filters: 'name' "
			                  "(case-insensitive substring of the asset name), 'classPath' (full class "
			                  "path e.g. '/Script/Engine.Material', subclasses included), 'path' (package "
			                  "path to search under, e.g. '/Game/Materials'), 'tagKey'/'tagValue' (asset "
			                  "registry tag filter). Paginated via 'offset'/'limit'."))
			.ParamString(TEXT("name"), TEXT("Case-insensitive substring filter on the asset name."))
			.ParamString(TEXT("classPath"), TEXT("Full class path filter, e.g. '/Script/Engine.Material'."))
			.ParamString(TEXT("path"), TEXT("Package path to search under, e.g. '/Game/Materials'."))
			.ParamString(TEXT("tagKey"), TEXT("Asset-registry tag name to filter on."))
			.ParamString(TEXT("tagValue"), TEXT("Required value for 'tagKey' (omit to match any value)."))
			.ParamBool(TEXT("recursive"), TEXT("Recurse sub-paths of 'path'. Default true."))
			.ParamInt(TEXT("offset"), TEXT("Pagination offset into the result set. Default 0."))
			.ParamInt(TEXT("limit"), TEXT("Maximum results to return. Default 100."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString NameFilter = Call.GetString(TEXT("name"));
				const FString ClassPath = Call.GetString(TEXT("classPath"));
				const FString PackagePath = Call.GetString(TEXT("path"));
				const FString TagKey = Call.GetString(TEXT("tagKey"));
				const FString TagValue = Call.GetString(TEXT("tagValue"));
				const bool bRecursive = Call.GetBool(TEXT("recursive"), true);
				const int32 Offset = FMath::Max(0, (int32)Call.GetInt(TEXT("offset"), 0));
				const int32 Limit = FMath::Max(1, (int32)Call.GetInt(TEXT("limit"), 100));

				FARFilter Filter;
				if (!PackagePath.IsEmpty())
				{
					Filter.PackagePaths.Add(FName(*PackagePath));
					Filter.bRecursivePaths = bRecursive;
				}
				if (!ClassPath.IsEmpty())
				{
					// A valid top-level class path is "/Script/Module.Class" (or "/Game/Path/Asset.Asset"):
					// it starts with '/' and contains a '.' separating package and asset. Constructing an
					// FTopLevelAssetPath from a "short" (dotless) name fires an engine ensure, so validate
					// the shape BEFORE construction and return a clean, LLM-actionable error instead.
					if (!ClassPath.StartsWith(TEXT("/")) || !ClassPath.Contains(TEXT(".")))
					{
						return FUnrealMcpToolResult::Error(
							FString::Printf(TEXT("'%s' is not a valid class path (expected '/Script/Module.Class')."), *ClassPath));
					}
					FTopLevelAssetPath ClassTopLevel(ClassPath);
					if (!ClassTopLevel.IsValid())
					{
						return FUnrealMcpToolResult::Error(
							FString::Printf(TEXT("'%s' is not a valid class path (expected '/Script/Module.Class')."), *ClassPath));
					}
					Filter.ClassPaths.Add(ClassTopLevel);
					Filter.bRecursiveClasses = true;
				}
				if (!TagKey.IsEmpty())
				{
					Filter.TagsAndValues.Add(FName(*TagKey),
						TagValue.IsEmpty() ? TOptional<FString>() : TOptional<FString>(TagValue));
				}

				TArray<FAssetData> Assets;
				IAssetRegistry& Registry = GetAssetRegistry();
				if (Filter.IsEmpty())
				{
					Registry.GetAllAssets(Assets);
				}
				else
				{
					Registry.GetAssets(Filter, Assets);
				}

				// Post-filter by name substring (the registry has no substring filter primitive).
				if (!NameFilter.IsEmpty())
				{
					Assets.RemoveAll([&NameFilter](const FAssetData& Data)
					{
						return !Data.AssetName.ToString().Contains(NameFilter, ESearchCase::IgnoreCase);
					});
				}

				// Deterministic ordering so pagination is stable across calls.
				Assets.Sort([](const FAssetData& A, const FAssetData& B)
				{
					return A.GetObjectPathString() < B.GetObjectPathString();
				});

				const int32 Total = Assets.Num();
				TArray<TSharedPtr<FJsonValue>> Page;
				for (int32 Index = Offset; Index < Total && Page.Num() < Limit; ++Index)
				{
					Page.Add(MakeShared<FJsonValueObject>(AssetDataToJson(Assets[Index])));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("total"), Total);
				Structured->SetNumberField(TEXT("offset"), Offset);
				Structured->SetNumberField(TEXT("count"), Page.Num());
				Structured->SetArrayField(TEXT("assets"), Page);

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d asset(s); returning %d (offset %d)."), Total, Page.Num(), Offset),
					Structured);
			});

		// asset-get-data ---------------------------------------------------------------------------
		Registry.Tool(TEXT("asset-get-data"))
			.Title(TEXT("Get Asset Data"))
			.Description(TEXT("Read AssetRegistry metadata for one asset by path-or-name (§3.2): name, "
			                  "object path, package, class, and the asset's registry tag map. Supports "
			                  "§3.2 scoped reads — pass 'paths' (dot-separated field names) to return only "
			                  "those branches of the result and save tokens."))
			.ParamString(TEXT("path"), TEXT("Asset path-or-name, e.g. '/Game/Mat/M_Foo' or '/Game/Mat/M_Foo.M_Foo'."), EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));
				}
				if (!UEditorAssetLibrary::DoesAssetExist(Path))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No asset at '%s'."), *Path));
				}

				const FAssetData Data = UEditorAssetLibrary::FindAssetData(Path);
				TSharedPtr<FJsonObject> Full = AssetDataToJson(Data);

				TSharedPtr<FJsonObject> Tags = MakeShared<FJsonObject>();
				Data.TagsAndValues.ForEach([&Tags](const TPair<FName, FAssetTagValueRef>& Tag)
				{
					Tags->SetStringField(Tag.Key.ToString(), Tag.Value.AsString());
				});
				Full->SetObjectField(TEXT("tags"), Tags);

				const TSharedPtr<FJsonObject> Structured = UnrealMcpAssetScopedRead::Apply(Full, GetScopedReadPaths(Call));
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Asset data for '%s'."), *Data.AssetName.ToString()), Structured);
			});

		// asset-create-folder ----------------------------------------------------------------------
		Registry.Tool(TEXT("asset-create-folder"))
			.Title(TEXT("Create Content Folder"))
			.Description(TEXT("Create a Content Browser folder (directory) at the given package path, "
			                  "e.g. '/Game/MyNewFolder'. Idempotent — succeeds if the folder already exists."))
			.ParamString(TEXT("path"), TEXT("Package path of the folder to create, e.g. '/Game/MyFolder'."), EUnrealMcpParamRequirement::Required)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));
				}
				if (UEditorAssetLibrary::DoesDirectoryExist(Path))
				{
					TSharedPtr<FJsonObject> Existing = MakeShared<FJsonObject>();
					Existing->SetStringField(TEXT("path"), Path);
					Existing->SetBoolField(TEXT("created"), false);
					return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Folder '%s' already exists."), *Path), Existing);
				}
				if (!UEditorAssetLibrary::MakeDirectory(Path))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to create folder '%s'."), *Path));
				}
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Path);
				Structured->SetBoolField(TEXT("created"), true);
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Created folder '%s'."), *Path), Structured);
			});

		// asset-copy -------------------------------------------------------------------------------
		Registry.Tool(TEXT("asset-copy"))
			.Title(TEXT("Copy Asset"))
			.Description(TEXT("Duplicate an asset to a new package path. 'source' and 'destination' use the "
			                  "§3.2 path-or-name convention; 'destination' is the full package path of the copy "
			                  "(e.g. '/Game/Mat/M_Foo_Copy')."))
			.ParamString(TEXT("source"), TEXT("Source asset path-or-name."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("destination"), TEXT("Destination package path for the duplicate."), EUnrealMcpParamRequirement::Required)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Source = Call.GetString(TEXT("source"));
				const FString Destination = Call.GetString(TEXT("destination"));
				if (Source.IsEmpty() || Destination.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'source' and 'destination' are required."));
				}
				if (!UEditorAssetLibrary::DoesAssetExist(Source))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No source asset at '%s'."), *Source));
				}
				if (!UEditorAssetLibrary::DuplicateAsset(Source, Destination))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Failed to copy '%s' -> '%s'."), *Source, *Destination));
				}
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("source"), Source);
				Structured->SetStringField(TEXT("destination"), Destination);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Copied '%s' to '%s'."), *Source, *Destination), Structured);
			});

		// asset-move -------------------------------------------------------------------------------
		Registry.Tool(TEXT("asset-move"))
			.Title(TEXT("Move / Rename Asset"))
			.Description(TEXT("Move or rename an asset. 'source' and 'destination' use the §3.2 path-or-name "
			                  "convention; 'destination' is the full target package path."))
			.ParamString(TEXT("source"), TEXT("Source asset path-or-name."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("destination"), TEXT("Destination package path."), EUnrealMcpParamRequirement::Required)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Source = Call.GetString(TEXT("source"));
				const FString Destination = Call.GetString(TEXT("destination"));
				if (Source.IsEmpty() || Destination.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'source' and 'destination' are required."));
				}
				if (!UEditorAssetLibrary::DoesAssetExist(Source))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No source asset at '%s'."), *Source));
				}
				if (!UEditorAssetLibrary::RenameAsset(Source, Destination))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Failed to move '%s' -> '%s'."), *Source, *Destination));
				}
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("source"), Source);
				Structured->SetStringField(TEXT("destination"), Destination);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Moved '%s' to '%s'."), *Source, *Destination), Structured);
			});

		// asset-delete -----------------------------------------------------------------------------
		Registry.Tool(TEXT("asset-delete"))
			.Title(TEXT("Delete Asset"))
			.Description(TEXT("Delete an asset by path-or-name (§3.2). Destructive and not undoable from MCP."))
			.ParamString(TEXT("path"), TEXT("Asset path-or-name to delete."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));
				}
				if (!UEditorAssetLibrary::DoesAssetExist(Path))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No asset at '%s'."), *Path));
				}
				if (!UEditorAssetLibrary::DeleteAsset(Path))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to delete '%s'."), *Path));
				}
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Path);
				Structured->SetBoolField(TEXT("deleted"), true);
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Deleted '%s'."), *Path), Structured);
			});

		// asset-refresh ----------------------------------------------------------------------------
		Registry.Tool(TEXT("asset-refresh"))
			.Title(TEXT("Refresh / Rescan Assets"))
			.Description(TEXT("Force the AssetRegistry to rescan one or more package paths so newly added "
			                  "files on disk become visible. Pass 'paths' (array of package paths); defaults "
			                  "to ['/Game'] when omitted."))
			.ParamString(TEXT("path"), TEXT("Single package path to rescan (alternative to 'paths')."))
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				TArray<FString> Paths = GetStringArray(Call, TEXT("paths"));
				const FString SinglePath = Call.GetString(TEXT("path"));
				if (!SinglePath.IsEmpty())
				{
					Paths.AddUnique(SinglePath);
				}
				if (Paths.Num() == 0)
				{
					Paths.Add(TEXT("/Game"));
				}

				GetAssetRegistry().ScanPathsSynchronous(Paths, /*bForceRescan*/ true);

				TArray<TSharedPtr<FJsonValue>> Scanned;
				for (const FString& Path : Paths)
				{
					Scanned.Add(MakeShared<FJsonValueString>(Path));
				}
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetArrayField(TEXT("paths"), Scanned);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Rescanned %d path(s)."), Paths.Num()), Structured);
			});

		// asset-material-create --------------------------------------------------------------------
		Registry.Tool(TEXT("asset-material-create"))
			.Title(TEXT("Create Material Instance"))
			.Description(TEXT("Create a UMaterialInstanceConstant from a parent material. 'parent' is the "
			                  "parent material/material-interface path-or-name; 'destination' is the full "
			                  "package path of the new instance (e.g. '/Game/Mat/MI_Foo')."))
			.ParamString(TEXT("parent"), TEXT("Parent material path-or-name."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("destination"), TEXT("Destination package path for the new material instance."), EUnrealMcpParamRequirement::Required)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ParentPath = Call.GetString(TEXT("parent"));
				const FString Destination = Call.GetString(TEXT("destination"));
				if (ParentPath.IsEmpty() || Destination.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'parent' and 'destination' are required."));
				}
				// Guard before LoadAsset: loading a missing asset logs an engine Error (which the
				// Automation harness counts as a failure) — DoesAssetExist is the clean pre-check.
				if (!UEditorAssetLibrary::DoesAssetExist(ParentPath))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Parent material '%s' does not exist."), *ParentPath));
				}

				UObject* ParentObject = UEditorAssetLibrary::LoadAsset(ParentPath);
				UMaterialInterface* Parent = Cast<UMaterialInterface>(ParentObject);
				if (!Parent)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("'%s' is not a material/material-interface."), *ParentPath));
				}

				FString PackagePath, AssetName;
				if (!SplitObjectPath(Destination, PackagePath, AssetName))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("'%s' is not a valid destination package path."), *Destination));
				}
				if (UEditorAssetLibrary::DoesAssetExist(Destination))
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("An asset already exists at '%s'."), *Destination));
				}

				UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
				Factory->InitialParent = Parent;

				UObject* Created = GetAssetTools().CreateAsset(
					AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
				if (!Created)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Failed to create material instance at '%s'."), *Destination));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Created->GetPathName());
				Structured->SetStringField(TEXT("parent"), Parent->GetPathName());
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Created material instance '%s' (parent '%s')."), *AssetName, *Parent->GetName()),
					Structured);
			});

		// asset-material-modify --------------------------------------------------------------------
		Registry.Tool(TEXT("asset-material-modify"))
			.Title(TEXT("Modify Material Instance"))
			.Description(TEXT("Set parameters on a UMaterialInstanceConstant. Pass 'scalars' (object of "
			                  "name->number), 'vectors' (object of name->{r,g,b,a}), and/or 'textures' "
			                  "(object of name->texture path-or-name). The instance is updated and marked dirty."))
			.ParamString(TEXT("path"), TEXT("Material instance path-or-name."), EUnrealMcpParamRequirement::Required)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));
				}
				if (!UEditorAssetLibrary::DoesAssetExist(Path))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No asset at '%s'."), *Path));
				}
				UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(UEditorAssetLibrary::LoadAsset(Path));
				if (!Instance)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("'%s' is not a UMaterialInstanceConstant."), *Path));
				}

				// The engine's UMaterialEditingLibrary setters ALWAYS return false (the editor-only
				// override is applied as a side effect, but the return value is never set). So we
				// determine applied-vs-failed by validating each requested name against the parent
				// material's known parameter set, NOT by the setter's return value.
				TArray<FName> ScalarNameList, VectorNameList, TextureNameList;
				UMaterialEditingLibrary::GetScalarParameterNames(Instance, ScalarNameList);
				UMaterialEditingLibrary::GetVectorParameterNames(Instance, VectorNameList);
				UMaterialEditingLibrary::GetTextureParameterNames(Instance, TextureNameList);
				const TSet<FName> KnownScalars(ScalarNameList);
				const TSet<FName> KnownVectors(VectorNameList);
				const TSet<FName> KnownTextures(TextureNameList);

				TArray<FString> Applied;
				TArray<FString> Failed;

				const TSharedPtr<FJsonObject>* Scalars;
				if (Call.Arguments->TryGetObjectField(TEXT("scalars"), Scalars))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Scalars)->Values)
					{
						const FName ParamName(*Pair.Key);
						double Value;
						if (KnownScalars.Contains(ParamName) && Pair.Value.IsValid() && Pair.Value->TryGetNumber(Value))
						{
							UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, ParamName, (float)Value);
							Applied.Add(FString::Printf(TEXT("scalar:%s"), *Pair.Key));
						}
						else
						{
							Failed.Add(FString::Printf(TEXT("scalar:%s"), *Pair.Key));
						}
					}
				}

				const TSharedPtr<FJsonObject>* Vectors;
				if (Call.Arguments->TryGetObjectField(TEXT("vectors"), Vectors))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Vectors)->Values)
					{
						const FName ParamName(*Pair.Key);
						const TSharedPtr<FJsonObject>* ColorObj;
						if (KnownVectors.Contains(ParamName) && Pair.Value.IsValid() && Pair.Value->TryGetObject(ColorObj))
						{
							FLinearColor Color(FLinearColor::Black);
							double Component;
							if ((*ColorObj)->TryGetNumberField(TEXT("r"), Component)) Color.R = (float)Component;
							if ((*ColorObj)->TryGetNumberField(TEXT("g"), Component)) Color.G = (float)Component;
							if ((*ColorObj)->TryGetNumberField(TEXT("b"), Component)) Color.B = (float)Component;
							if ((*ColorObj)->TryGetNumberField(TEXT("a"), Component)) Color.A = (float)Component;
							UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, ParamName, Color);
							Applied.Add(FString::Printf(TEXT("vector:%s"), *Pair.Key));
						}
						else
						{
							Failed.Add(FString::Printf(TEXT("vector:%s"), *Pair.Key));
						}
					}
				}

				const TSharedPtr<FJsonObject>* Textures;
				if (Call.Arguments->TryGetObjectField(TEXT("textures"), Textures))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Textures)->Values)
					{
						const FName ParamName(*Pair.Key);
						FString TexturePath;
						UTexture* Texture = nullptr;
						if (Pair.Value.IsValid() && Pair.Value->TryGetString(TexturePath) && UEditorAssetLibrary::DoesAssetExist(TexturePath))
						{
							Texture = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexturePath));
						}
						if (KnownTextures.Contains(ParamName) && Texture)
						{
							UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, ParamName, Texture);
							Applied.Add(FString::Printf(TEXT("texture:%s"), *Pair.Key));
						}
						else
						{
							Failed.Add(FString::Printf(TEXT("texture:%s"), *Pair.Key));
						}
					}
				}

				UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
				Instance->MarkPackageDirty();

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), Instance->GetPathName());
				TArray<TSharedPtr<FJsonValue>> AppliedJson;
				for (const FString& Item : Applied) AppliedJson.Add(MakeShared<FJsonValueString>(Item));
				Structured->SetArrayField(TEXT("applied"), AppliedJson);
				TArray<TSharedPtr<FJsonValue>> FailedJson;
				for (const FString& Item : Failed) FailedJson.Add(MakeShared<FJsonValueString>(Item));
				Structured->SetArrayField(TEXT("failed"), FailedJson);

				if (Applied.Num() == 0 && Failed.Num() > 0)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("No parameters applied to '%s' (%d failed — unknown parameter names?)."),
							*Instance->GetName(), Failed.Num()));
				}
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Applied %d parameter(s) to '%s' (%d failed)."), Applied.Num(), *Instance->GetName(), Failed.Num()),
					Structured);
			});

		// asset-material-get-data ------------------------------------------------------------------
		Registry.Tool(TEXT("asset-material-get-data"))
			.Title(TEXT("Get Material Parameters"))
			.Description(TEXT("Read parameter info for a material or material instance (the 'shader' analog): "
			                  "scalar/vector/texture parameter names, plus current values when the asset is a "
			                  "material instance, plus the parent. Supports §3.2 scoped reads via 'paths'."))
			.ParamString(TEXT("path"), TEXT("Material or material-instance path-or-name."), EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path"));
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'path' is required."));
				}
				if (!UEditorAssetLibrary::DoesAssetExist(Path))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No asset at '%s'."), *Path));
				}
				UMaterialInterface* Material = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(Path));
				if (!Material)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("'%s' is not a material/material-interface."), *Path));
				}
				UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);

				TArray<FName> ScalarNames, VectorNames, TextureNames;
				UMaterialEditingLibrary::GetScalarParameterNames(Material, ScalarNames);
				UMaterialEditingLibrary::GetVectorParameterNames(Material, VectorNames);
				UMaterialEditingLibrary::GetTextureParameterNames(Material, TextureNames);

				TSharedPtr<FJsonObject> Scalars = MakeShared<FJsonObject>();
				for (const FName& Name : ScalarNames)
				{
					if (Instance)
						Scalars->SetNumberField(Name.ToString(), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, Name));
					else
						Scalars->SetField(Name.ToString(), MakeShared<FJsonValueNull>());
				}
				TSharedPtr<FJsonObject> Vectors = MakeShared<FJsonObject>();
				for (const FName& Name : VectorNames)
				{
					if (Instance)
						Vectors->SetField(Name.ToString(), LinearColorToJson(UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, Name)));
					else
						Vectors->SetField(Name.ToString(), MakeShared<FJsonValueNull>());
				}
				TSharedPtr<FJsonObject> Textures = MakeShared<FJsonObject>();
				for (const FName& Name : TextureNames)
				{
					UTexture* Texture = Instance ? UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, Name) : nullptr;
					Textures->SetStringField(Name.ToString(), Texture ? Texture->GetPathName() : FString());
				}

				TSharedPtr<FJsonObject> Full = MakeShared<FJsonObject>();
				Full->SetStringField(TEXT("path"), Material->GetPathName());
				Full->SetBoolField(TEXT("isInstance"), Instance != nullptr);
				if (Instance && Instance->Parent)
				{
					Full->SetStringField(TEXT("parent"), Instance->Parent->GetPathName());
				}
				Full->SetObjectField(TEXT("scalars"), Scalars);
				Full->SetObjectField(TEXT("vectors"), Vectors);
				Full->SetObjectField(TEXT("textures"), Textures);

				const TSharedPtr<FJsonObject> Structured = UnrealMcpAssetScopedRead::Apply(Full, GetScopedReadPaths(Call));
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Material parameters for '%s'."), *Material->GetName()), Structured);
			});

		// asset-import -----------------------------------------------------------------------------
		Registry.Tool(TEXT("asset-import"))
			.Title(TEXT("Import Asset"))
			.Description(TEXT("Import a source file (FBX, texture/PNG, etc.) from the local filesystem into "
			                  "the Content Browser via an AssetImportTask. 'file' is an absolute filesystem "
			                  "path; 'destination' is the target package path (e.g. '/Game/Imported'); 'name' "
			                  "optionally overrides the imported asset name."))
			.ParamString(TEXT("file"), TEXT("Absolute filesystem path of the source file to import."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("destination"), TEXT("Destination package path, e.g. '/Game/Imported'."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Optional imported asset name override."))
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString File = Call.GetString(TEXT("file"));
				const FString Destination = Call.GetString(TEXT("destination"));
				const FString NameOverride = Call.GetString(TEXT("name"));
				if (File.IsEmpty() || Destination.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("'file' and 'destination' are required."));
				}
				if (!FPaths::FileExists(File))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Source file does not exist: '%s'."), *File));
				}

				UAssetImportTask* Task = NewObject<UAssetImportTask>();
				Task->Filename = File;
				Task->DestinationPath = Destination;
				if (!NameOverride.IsEmpty())
				{
					Task->DestinationName = NameOverride;
				}
				Task->bAutomated = true;
				Task->bReplaceExisting = true;
				Task->bSave = false;
				Task->bAsync = false;

				GetAssetTools().ImportAssetTasks({ Task });

				const TArray<UObject*>& Imported = Task->GetObjects();
				if (Imported.Num() == 0)
				{
					return FUnrealMcpToolResult::Error(
						FString::Printf(TEXT("Import produced no assets from '%s'."), *File));
				}

				TArray<TSharedPtr<FJsonValue>> Paths;
				for (UObject* Object : Imported)
				{
					if (Object)
					{
						Paths.Add(MakeShared<FJsonValueString>(Object->GetPathName()));
					}
				}
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("file"), File);
				Structured->SetArrayField(TEXT("imported"), Paths);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Imported %d asset(s) from '%s'."), Imported.Num(), *File), Structured);
			});
	}
}
