// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAITemplate, Log, All);

// Flip to 1 to make the sample emit an ADDITIONAL invalid tool descriptor (an empty-named parameter,
// which is a malformed schema). This exercises the §5 descriptor-validation isolation path: Unreal-MCP
// drops the invalid entry and records the failure on this extension's record, while the valid
// 'hello-extension' tool below stays registered and other extensions are unaffected.
#ifndef UNREAL_AI_TEMPLATE_INVALID_SCHEMA
#define UNREAL_AI_TEMPLATE_INVALID_SCHEMA 0
#endif

/**
 * The sample extension's tool provider — a textbook implementation of the Unreal-MCP extension
 * contract (docs/ARCHITECTURE.md §5, docs/EXTENSIONS.md). It contributes one `hello-extension` tool.
 */
class FUnrealAITemplateProvider : public IUnrealMcpToolProvider
{
public:
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-template"); }
	virtual FText GetDisplayName() const override { return NSLOCTEXT("UnrealAITemplate", "DisplayName", "Unreal AI Template"); }
	virtual FString GetExtensionVersion() const override { return TEXT("1.0.0"); }

	virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
	{
		Registry.Tool(TEXT("hello-extension"))
			.Title(TEXT("Hello Extension"))
			.Description(TEXT("Sample extension tool proving the IUnrealMcpToolProvider contract end-to-end. "
			                  "Returns a friendly greeting, optionally addressed to 'name'."))
			.ParamString(TEXT("name"), TEXT("Who to greet. Defaults to 'world'."))
			.ReadOnlyHint(true)
			.IdempotentHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				// Runs ON the game thread (the dispatcher guarantees it, §4).
				const FString Name = Call.Has(TEXT("name")) ? Call.GetString(TEXT("name")) : TEXT("world");
				const FString Greeting = FString::Printf(TEXT("Hello, %s! — from the Unreal-MCP extension sample."), *Name);

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("greeting"), Greeting);
				return FUnrealMcpToolResult::Success(Greeting, Structured);
			});

#if UNREAL_AI_TEMPLATE_INVALID_SCHEMA
		// Isolation fixture: an empty-named parameter is a malformed schema. Unreal-MCP drops THIS entry
		// and records the error on this extension; 'hello-extension' above remains registered.
		Registry.Tool(TEXT("hello-broken"))
			.Title(TEXT("Hello Broken"))
			.Description(TEXT("Intentionally invalid tool used to demonstrate descriptor-validation isolation."))
			.ParamString(TEXT(""), TEXT("Empty-named parameter -> malformed schema."))
			.Handle([](const FUnrealMcpToolCall&) -> FUnrealMcpToolResult
			{
				return FUnrealMcpToolResult::Success(TEXT("unreachable"));
			});
#endif
	}
};

/**
 * Editor module that owns the provider and registers it as a modular feature, so Unreal-MCP discovers
 * it (on boot via initial enumeration, or live via the OnModularFeatureRegistered event when this
 * plugin loads after Unreal-MCP). Unregistering on shutdown triggers a registry rebuild + manifest
 * revision bump on the Unreal-MCP side.
 */
class FUnrealAITemplateModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeUnique<FUnrealAITemplateProvider>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
		UE_LOG(LogUnrealAITemplate, Log, TEXT("[UnrealAITemplate] registered MCP tool provider '%s'."), *Provider->GetExtensionId());
	}

	virtual void ShutdownModule() override
	{
		if (Provider.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
			Provider.Reset();
			UE_LOG(LogUnrealAITemplate, Log, TEXT("[UnrealAITemplate] unregistered MCP tool provider."));
		}
	}

private:
	TUniquePtr<FUnrealAITemplateProvider> Provider;
};

IMPLEMENT_MODULE(FUnrealAITemplateModule, UnrealAITemplate)
