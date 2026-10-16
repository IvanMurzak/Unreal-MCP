// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

// --- Enhanced Input asset types the tools author / inspect -----------------------------------
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputCoreTypes.h"

// --- Editor / Asset-Registry APIs the tools call ---------------------------------------------
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIEnhancedInput, Log, All);

namespace
{
	// The module is unity-built (every .cpp concatenated into one TU), so file-local helpers must be
	// MODULE-UNIQUE (the Unreal-MCP UnrealMCP/AGENTS.md ODR rule) — prefix with the module name. These are shared
	// by the create handlers below (path splitting + write-root guard) and the read handlers (value-type
	// stringification), mirroring the core UnrealMcpAssetTools idiom.

	/** Split "/Game/Foo/Bar" (or the object form "/Game/Foo/Bar.Bar") into "/Game/Foo" + "Bar". */
	bool UnrealAIEnhancedInput_SplitObjectPath(const FString& InPath, FString& OutPackagePath, FString& OutAssetName)
	{
		FString Work = InPath;
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

	/** Reject the engine/script content roots for WRITE operations — constrain creation to project roots. */
	bool UnrealAIEnhancedInput_IsWritableContentRoot(const FString& InPath)
	{
		auto IsReservedRoot = [&InPath](const TCHAR* Root)
		{
			return InPath.Equals(Root) || InPath.StartsWith(FString(Root) + TEXT("/"));
		};
		return !(IsReservedRoot(TEXT("/Engine")) || IsReservedRoot(TEXT("/Script")) || IsReservedRoot(TEXT("/Temp")));
	}

	/** EInputActionValueType -> the canonical token the action-create tool accepts (round-trips). */
	FString UnrealAIEnhancedInput_ValueTypeToString(EInputActionValueType ValueType)
	{
		switch (ValueType)
		{
		case EInputActionValueType::Boolean: return TEXT("Boolean");
		case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
		case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
		case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
		default:                             return TEXT("Boolean");
		}
	}

	/** Parse a value-type token (case-insensitive). Returns false on an unknown token. */
	bool UnrealAIEnhancedInput_ParseValueType(const FString& In, EInputActionValueType& Out)
	{
		if (In.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase)) { Out = EInputActionValueType::Boolean; return true; }
		if (In.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase))  { Out = EInputActionValueType::Axis1D;  return true; }
		if (In.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase))  { Out = EInputActionValueType::Axis2D;  return true; }
		if (In.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase))  { Out = EInputActionValueType::Axis3D;  return true; }
		return false;
	}
}

/**
 * The extension's tool provider — an implementation of the Unreal-MCP extension contract
 * (IUnrealMcpToolProvider). It declares this extension's tools through the fluent
 * FUnrealMcpToolRegistry builder. See https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/EXTENSIONS.md.
 *
 * Enhanced Input is a built-in, plugin-gated input system. This extension stays deliberately THIN: a
 * proof of capability over Enhanced Input's asset-authoring surface (Input Actions, Mapping Contexts,
 * key bindings), not exhaustive coverage. Every tool is a handler lambda over game-thread-safe asset /
 * AssetRegistry / editor APIs, with no async work and no owned UI. Handlers are DEFENSIVE — UE builds
 * without C++ exceptions, so a crash inside a handler is an editor crash; every tool validates its
 * inputs and the engine state it touches and returns FUnrealMcpToolResult::Error(...) instead of
 * dereferencing a null.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName — `commands/bump-version.ps1`
 * updates both atomically.
 */
class FUnrealAIEnhancedInputProvider : public IUnrealMcpToolProvider
{
public:
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-enhanced-input"); }
	virtual FText GetDisplayName() const override { return NSLOCTEXT("UnrealAIEnhancedInput", "DisplayName", "Unreal AI EnhancedInput"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.0"); }

	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
	{
		// =====================================================================================
		//  Tool ids are kebab-case (^[a-z0-9]+(-[a-z0-9]+)*$). Handlers run ON the game thread
		//  (the dispatcher guarantees it), so editor / engine APIs are called directly. A handler
		//  returns FUnrealMcpToolResult::Success(text, structuredJson) or ::Error(message).
		// =====================================================================================

		// -------------------------------------------------------------------------------------
		// enhanced-input-list — enumerate every UInputAction + UInputMappingContext asset in the
		// project via the AssetRegistry (no asset is loaded — cheap, read-only).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("enhanced-input-list"))
			.Title(TEXT("List Enhanced Input Assets"))
			.Description(TEXT("Lists every Enhanced Input asset in the project via the Asset Registry, without "
			                  "loading any of them: Input Actions (UInputAction) and Mapping Contexts "
			                  "(UInputMappingContext). Optionally filter by a content-path prefix. Returns "
			                  "{ actionCount, contextCount, actions:[{ name, path }], contexts:[{ name, path }] }."))
			.ParamString(TEXT("pathPrefix"), TEXT("Optional content-path prefix filter, e.g. '/Game/Input'. Empty = whole project."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FAssetRegistryModule& AssetRegistryModule =
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

				const FString PathPrefix = Call.GetString(TEXT("pathPrefix")).TrimStartAndEnd();

				auto CollectByClass = [&AssetRegistry, &PathPrefix](UClass* AssetClass, TArray<TSharedPtr<FJsonValue>>& OutJson)
				{
					TArray<FAssetData> Assets;
					AssetRegistry.GetAssetsByClass(AssetClass->GetClassPathName(), Assets);
					for (const FAssetData& Asset : Assets)
					{
						const FString ObjectPath = Asset.GetObjectPathString();
						if (!PathPrefix.IsEmpty() && !ObjectPath.StartsWith(PathPrefix))
						{
							continue;
						}
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
						Entry->SetStringField(TEXT("path"), ObjectPath);
						OutJson.Add(MakeShared<FJsonValueObject>(Entry));
					}
				};

				TArray<TSharedPtr<FJsonValue>> ActionsJson;
				TArray<TSharedPtr<FJsonValue>> ContextsJson;
				CollectByClass(UInputAction::StaticClass(), ActionsJson);
				CollectByClass(UInputMappingContext::StaticClass(), ContextsJson);

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("actionCount"), ActionsJson.Num());
				Structured->SetNumberField(TEXT("contextCount"), ContextsJson.Num());
				Structured->SetArrayField(TEXT("actions"), ActionsJson);
				Structured->SetArrayField(TEXT("contexts"), ContextsJson);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d Input Action(s) and %d Mapping Context(s)."),
						ActionsJson.Num(), ContextsJson.Num()), Structured);
			});

		// -------------------------------------------------------------------------------------
		// enhanced-input-get — read one Input Action or Mapping Context's scalar config (read-only).
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("enhanced-input-get"))
			.Title(TEXT("Get Enhanced Input Asset"))
			.Description(TEXT("Inspects a single Enhanced Input asset (read-only). For a UInputAction returns "
			                  "{ type:'action', path, name, valueType }; for a UInputMappingContext returns "
			                  "{ type:'context', path, name, mappingCount, mappings:[{ action, key }] }."))
			.ParamString(TEXT("path"), TEXT("Asset path of the Input Action or Mapping Context, e.g. '/Game/Input/IA_Jump'."),
				EUnrealMcpParamRequirement::Required)
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Path = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Path.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/Input/IA_Jump')."));
				}
				if (!UEditorAssetLibrary::DoesAssetExist(Path))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No asset found at '%s'."), *Path));
				}

				UObject* Asset = UEditorAssetLibrary::LoadAsset(Path);
				if (!Asset)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'."), *Path));
				}

				if (const UInputAction* Action = Cast<UInputAction>(Asset))
				{
					TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
					Structured->SetStringField(TEXT("type"), TEXT("action"));
					Structured->SetStringField(TEXT("path"), Path);
					Structured->SetStringField(TEXT("name"), Action->GetName());
					Structured->SetStringField(TEXT("valueType"), UnrealAIEnhancedInput_ValueTypeToString(Action->ValueType));
					return FUnrealMcpToolResult::Success(
						FString::Printf(TEXT("Input Action '%s' (valueType %s)."),
							*Action->GetName(), *UnrealAIEnhancedInput_ValueTypeToString(Action->ValueType)), Structured);
				}

				if (const UInputMappingContext* Context = Cast<UInputMappingContext>(Asset))
				{
					TArray<TSharedPtr<FJsonValue>> MappingsJson;
					for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("action"), Mapping.Action ? Mapping.Action->GetName() : TEXT("None"));
						Entry->SetStringField(TEXT("key"), Mapping.Key.GetFName().ToString());
						MappingsJson.Add(MakeShared<FJsonValueObject>(Entry));
					}

					TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
					Structured->SetStringField(TEXT("type"), TEXT("context"));
					Structured->SetStringField(TEXT("path"), Path);
					Structured->SetStringField(TEXT("name"), Context->GetName());
					Structured->SetNumberField(TEXT("mappingCount"), MappingsJson.Num());
					Structured->SetArrayField(TEXT("mappings"), MappingsJson);
					return FUnrealMcpToolResult::Success(
						FString::Printf(TEXT("Mapping Context '%s' has %d mapping(s)."),
							*Context->GetName(), MappingsJson.Num()), Structured);
				}

				return FUnrealMcpToolResult::Error(FString::Printf(
					TEXT("'%s' is not an Enhanced Input asset (expected a UInputAction or UInputMappingContext)."), *Path));
			});

		// -------------------------------------------------------------------------------------
		// enhanced-input-action-create — create a UInputAction asset (value type). Mutating.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("enhanced-input-action-create"))
			.Title(TEXT("Create Input Action"))
			.Description(TEXT("Creates a UInputAction asset (an Enhanced Input action) at a content path. 'path' is "
			                  "the destination package path (e.g. '/Game/Input/IA_Jump'); optional 'valueType' is one "
			                  "of Boolean | Axis1D | Axis2D | Axis3D (default Boolean). The asset is created in-memory "
			                  "and the package marked dirty; pass 'save':true to also write it to disk. Returns "
			                  "{ path, name, valueType, saved }."))
			.ParamString(TEXT("path"), TEXT("Destination package path for the new Input Action, e.g. '/Game/Input/IA_Jump'."),
				EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("valueType"), TEXT("Value type: Boolean | Axis1D | Axis2D | Axis3D. Defaults to Boolean."))
			.ParamBool(TEXT("save"), TEXT("Save the new asset to disk after creation. Default false (in-memory only)."))
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Destination = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Destination.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/Input/IA_Jump')."));
				}

				EInputActionValueType ValueType = EInputActionValueType::Boolean;
				const FString ValueTypeStr = Call.GetString(TEXT("valueType")).TrimStartAndEnd();
				if (!ValueTypeStr.IsEmpty() && !UnrealAIEnhancedInput_ParseValueType(ValueTypeStr, ValueType))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Unknown 'valueType' '%s' (use Boolean | Axis1D | Axis2D | Axis3D)."), *ValueTypeStr));
				}

				FString PackagePath, AssetName;
				if (!UnrealAIEnhancedInput_SplitObjectPath(Destination, PackagePath, AssetName))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("'%s' is not a valid destination package path (e.g. '/Game/Input/IA_Jump')."), *Destination));
				}
				if (!UnrealAIEnhancedInput_IsWritableContentRoot(Destination))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Refusing to create '%s' under an engine content root; use a project root like '/Game'."), *Destination));
				}
				if (UEditorAssetLibrary::DoesAssetExist(Destination))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'."), *Destination));
				}

				const FString PackageName = PackagePath / AssetName;
				UPackage* Package = CreatePackage(*PackageName);
				if (!Package)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to create package '%s'."), *PackageName));
				}
				UInputAction* NewAction = NewObject<UInputAction>(
					Package, UInputAction::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
				if (!NewAction)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to create Input Action '%s'."), *AssetName));
				}
				NewAction->ValueType = ValueType;
				FAssetRegistryModule::AssetCreated(NewAction);
				Package->MarkPackageDirty();

				const bool bSave = Call.GetBool(TEXT("save"), false);
				const bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(NewAction, /*bOnlyIfIsDirty*/ false) : false;

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), NewAction->GetPathName());
				Structured->SetStringField(TEXT("name"), NewAction->GetName());
				Structured->SetStringField(TEXT("valueType"), UnrealAIEnhancedInput_ValueTypeToString(ValueType));
				Structured->SetBoolField(TEXT("saved"), bSaved);
				const TCHAR* SaveSuffix = !bSave ? TEXT("") : (bSaved ? TEXT(", saved") : TEXT(", SAVE FAILED"));
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Created Input Action '%s' (valueType %s)%s."),
						*AssetName, *UnrealAIEnhancedInput_ValueTypeToString(ValueType), SaveSuffix), Structured);
			});

		// -------------------------------------------------------------------------------------
		// enhanced-input-context-create — create a UInputMappingContext asset. Mutating.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("enhanced-input-context-create"))
			.Title(TEXT("Create Input Mapping Context"))
			.Description(TEXT("Creates a UInputMappingContext asset (an Enhanced Input mapping context) at a content "
			                  "path. 'path' is the destination package path (e.g. '/Game/Input/IMC_Default'). The asset "
			                  "is created in-memory and the package marked dirty; pass 'save':true to also write it to "
			                  "disk. Returns { path, name, saved }."))
			.ParamString(TEXT("path"), TEXT("Destination package path for the new Mapping Context, e.g. '/Game/Input/IMC_Default'."),
				EUnrealMcpParamRequirement::Required)
			.ParamBool(TEXT("save"), TEXT("Save the new asset to disk after creation. Default false (in-memory only)."))
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Destination = Call.GetString(TEXT("path")).TrimStartAndEnd();
				if (Destination.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'path' (e.g. '/Game/Input/IMC_Default')."));
				}

				FString PackagePath, AssetName;
				if (!UnrealAIEnhancedInput_SplitObjectPath(Destination, PackagePath, AssetName))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("'%s' is not a valid destination package path (e.g. '/Game/Input/IMC_Default')."), *Destination));
				}
				if (!UnrealAIEnhancedInput_IsWritableContentRoot(Destination))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Refusing to create '%s' under an engine content root; use a project root like '/Game'."), *Destination));
				}
				if (UEditorAssetLibrary::DoesAssetExist(Destination))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'."), *Destination));
				}

				const FString PackageName = PackagePath / AssetName;
				UPackage* Package = CreatePackage(*PackageName);
				if (!Package)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to create package '%s'."), *PackageName));
				}
				UInputMappingContext* NewContext = NewObject<UInputMappingContext>(
					Package, UInputMappingContext::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
				if (!NewContext)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to create Mapping Context '%s'."), *AssetName));
				}
				FAssetRegistryModule::AssetCreated(NewContext);
				Package->MarkPackageDirty();

				const bool bSave = Call.GetBool(TEXT("save"), false);
				const bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(NewContext, /*bOnlyIfIsDirty*/ false) : false;

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("path"), NewContext->GetPathName());
				Structured->SetStringField(TEXT("name"), NewContext->GetName());
				Structured->SetBoolField(TEXT("saved"), bSaved);
				const TCHAR* SaveSuffix = !bSave ? TEXT("") : (bSaved ? TEXT(", saved") : TEXT(", SAVE FAILED"));
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Created Mapping Context '%s'%s."), *AssetName, SaveSuffix), Structured);
			});

		// -------------------------------------------------------------------------------------
		// enhanced-input-add-mapping — add a key -> action mapping to a Mapping Context. Mutating.
		// -------------------------------------------------------------------------------------
		Registry.Tool(TEXT("enhanced-input-add-mapping"))
			.Title(TEXT("Add Input Mapping"))
			.Description(TEXT("Adds a key -> action mapping to a UInputMappingContext. 'contextPath' is the Mapping "
			                  "Context asset; 'actionPath' is the UInputAction; 'key' is an FKey name (e.g. 'SpaceBar', "
			                  "'Gamepad_FaceButton_Bottom'). The package is marked dirty; pass 'save':true to also write "
			                  "it to disk. Returns { contextPath, actionPath, key, mappingCount, saved }."))
			.ParamString(TEXT("contextPath"), TEXT("Asset path of the Mapping Context to add to, e.g. '/Game/Input/IMC_Default'."),
				EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("actionPath"), TEXT("Asset path of the Input Action to bind, e.g. '/Game/Input/IA_Jump'."),
				EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("key"), TEXT("FKey name to bind, e.g. 'SpaceBar' or 'Gamepad_FaceButton_Bottom'."),
				EUnrealMcpParamRequirement::Required)
			.ParamBool(TEXT("save"), TEXT("Save the context to disk after adding. Default false (in-memory only)."))
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString ContextPath = Call.GetString(TEXT("contextPath")).TrimStartAndEnd();
				const FString ActionPath = Call.GetString(TEXT("actionPath")).TrimStartAndEnd();
				const FString KeyName = Call.GetString(TEXT("key")).TrimStartAndEnd();
				if (ContextPath.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'contextPath' (e.g. '/Game/Input/IMC_Default')."));
				}
				if (ActionPath.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'actionPath' (e.g. '/Game/Input/IA_Jump')."));
				}
				if (KeyName.IsEmpty())
				{
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'key' (e.g. 'SpaceBar')."));
				}

				if (!UEditorAssetLibrary::DoesAssetExist(ContextPath))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No Mapping Context found at '%s'."), *ContextPath));
				}
				if (!UEditorAssetLibrary::DoesAssetExist(ActionPath))
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No Input Action found at '%s'."), *ActionPath));
				}

				UInputMappingContext* Context = Cast<UInputMappingContext>(UEditorAssetLibrary::LoadAsset(ContextPath));
				if (!Context)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("'%s' is not a UInputMappingContext."), *ContextPath));
				}
				UInputAction* Action = Cast<UInputAction>(UEditorAssetLibrary::LoadAsset(ActionPath));
				if (!Action)
				{
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a UInputAction."), *ActionPath));
				}

				// Use a named FName temporary (not `FKey ResolvedKey(FName(*KeyName))`) — the inline form
				// is a most-vexing-parse: the compiler reads it as a function declaration, not a variable.
				const FName KeyFName(*KeyName);
				const FKey ResolvedKey(KeyFName);
				if (!ResolvedKey.IsValid())
				{
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Unknown key '%s' (expected an FKey name like 'SpaceBar')."), *KeyName));
				}

				Context->MapKey(Action, ResolvedKey);
				Context->MarkPackageDirty();

				const bool bSave = Call.GetBool(TEXT("save"), false);
				const bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(Context, /*bOnlyIfIsDirty*/ false) : false;

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("contextPath"), ContextPath);
				Structured->SetStringField(TEXT("actionPath"), ActionPath);
				Structured->SetStringField(TEXT("key"), ResolvedKey.GetFName().ToString());
				Structured->SetNumberField(TEXT("mappingCount"), Context->GetMappings().Num());
				Structured->SetBoolField(TEXT("saved"), bSaved);
				const TCHAR* SaveSuffix = !bSave ? TEXT("") : (bSaved ? TEXT(", saved") : TEXT(", SAVE FAILED"));
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Mapped '%s' -> '%s' in '%s' (%d mapping(s))%s."),
						*ResolvedKey.GetFName().ToString(), *Action->GetName(), *Context->GetName(),
						Context->GetMappings().Num(), SaveSuffix), Structured);
			});
	}
};

/**
 * Editor module that owns the provider and registers it as a modular feature, so Unreal-MCP discovers
 * it — on boot via initial enumeration, or live via the OnModularFeatureRegistered event when this
 * plugin loads after Unreal-MCP. Unregistering on shutdown triggers a registry rebuild + manifest
 * revision bump on the Unreal-MCP side (the token-economy win: disabling the extension live-removes
 * its tools from the advertised set).
 */
class FUnrealAIEnhancedInputModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeUnique<FUnrealAIEnhancedInputProvider>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
		UE_LOG(LogUnrealAIEnhancedInput, Log, TEXT("[UnrealAIEnhancedInput] registered MCP tool provider '%s'."), *Provider->GetExtensionId());
	}

	virtual void ShutdownModule() override
	{
		if (Provider.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
			Provider.Reset();
			UE_LOG(LogUnrealAIEnhancedInput, Log, TEXT("[UnrealAIEnhancedInput] unregistered MCP tool provider."));
		}
	}

private:
	TUniquePtr<FUnrealAIEnhancedInputProvider> Provider;
};

IMPLEMENT_MODULE(FUnrealAIEnhancedInputModule, UnrealAIEnhancedInput)
