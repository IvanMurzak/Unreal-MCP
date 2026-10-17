// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAITemplateRqlApi.h"

/**
 * The sample extension's RQL module — a textbook implementation of the Unreal-MCP extension seam for editor data
 * (docs/rql.md "Extensions", docs/EXTENSIONS.md). An extension does not add tools: agents reach its feature through
 * rql-query / rql-mutate. Every reflected type is already queryable; a module adds what reflection does not show:
 *
 *  - sources (where agents start a query) — here the `hello` operations object (AddApiSource: its UFUNCTIONs are the
 *    operations);
 *  - members — here a JSON member on static mesh components (AddJsonMember); AddObjectMember / AddObjectListMember /
 *    ExposeProperties also exist;
 *  - a guide, rendered into the `unreal-rql` skill.
 *
 * The version is the .uplugin VersionName. TUnrealMcpRqlEditorModule (below) registers the module for the editor module's
 * lifetime, so Unreal-MCP discovers it on boot or live when this plugin loads later.
 */
class FUnrealAITemplateRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("Template"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-template"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAITemplate")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddApiSource(TEXT("hello"), TEXT("the sample's operations: call Greet(name) / Relabel(actor, label)"),
			UUnrealAITemplateRqlApi::StaticClass(), TEXT("HelloOperations"));
		// A member of a narrow class (not of every actor): what reflection does not show, here the material slot count.
		Context.AddJsonMember(UStaticMeshComponent::StaticClass(), TEXT("MaterialCount"),
			TEXT("how many material slots the mesh has (sample member)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Object);
				return MakeShared<FJsonValueNumber>(Mesh ? Mesh->GetNumMaterials() : 0);
			});
		Context.AddGuide(TEXT("Template (sample extension)"), TEXT(R"MD(```
hello | call Greet("Ada")                         returns the greeting
hello | call Relabel("Cube", "Crate")             relabel an actor (label, name or ref)
actors | get Components | where $ is StaticMeshComponent | where MaterialCount > 1 | select Owner.Label MaterialCount
```)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAITemplateRqlModule>, UnrealAITemplate)
