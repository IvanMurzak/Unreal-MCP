// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAITemplateRqlApi.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAITemplate, Log, All);

/**
 * The sample extension's RQL module — a textbook implementation of the Unreal-MCP extension seam for editor data
 * (docs/rql.md "Extensions", docs/EXTENSIONS.md). An extension does not add tools: agents reach its feature through
 * rql-query / rql-mutate. Every reflected type is already queryable; a module adds what reflection does not show:
 *
 *  - sources (where agents start a query) — here the `hello` operations object;
 *  - operations — every UFUNCTION of an API class (AddApiClass);
 *  - members — here a JSON member on every actor (AddJsonMember); ExposeProperties / AddObjectListMember also exist;
 *  - a guide, rendered into the `unreal-rql` skill.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName.
 */
class FUnrealAITemplateRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("Template"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-template"); }
	virtual FString GetExtensionVersion() const override { return TEXT("1.0.0"); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddApiClass(UUnrealAITemplateRqlApi::StaticClass(), TEXT("HelloOperations"));
		Context.AddObjectSource(TEXT("hello"), TEXT("the sample's operations: call Greet(name) / Relabel(actor, label)"),
			[]() -> UObject* { return GetMutableDefault<UUnrealAITemplateRqlApi>(); });
		Context.AddJsonMember(AActor::StaticClass(), TEXT("TagCount"), TEXT("how many actor tags the actor has (sample member)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const AActor* Actor = Cast<AActor>(Object);
				return MakeShared<FJsonValueNumber>(Actor ? Actor->Tags.Num() : 0);
			});
		Context.AddGuide(TEXT("Template (sample extension)"), TEXT(R"MD(```
hello | call Greet("Ada")                         returns the greeting
hello | call Relabel("Cube", "Crate")             relabel an actor (label, name or ref)
actors | where TagCount > 0 | select Label Tags TagCount
```)MD"));
	}
};

/**
 * Editor module that owns the RQL module and registers it as a modular feature, so Unreal-MCP discovers it (on boot,
 * or live when this plugin loads after Unreal-MCP — the rql-* tools are republished either way). Unregistering on
 * shutdown republishes them without the module.
 */
class FUnrealAITemplateModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RqlModule = MakeUnique<FUnrealAITemplateRqlModule>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
		UE_LOG(LogUnrealAITemplate, Log, TEXT("[UnrealAITemplate] registered RQL module '%s'."), *RqlModule->GetRqlModuleName());
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
	TUniquePtr<FUnrealAITemplateRqlModule> RqlModule;
};

IMPLEMENT_MODULE(FUnrealAITemplateModule, UnrealAITemplate)
