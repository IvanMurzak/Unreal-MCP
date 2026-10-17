// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIPCGRqlApi.h"

#include "PCGCommon.h" // EPCGNodeTitleType
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGNode.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIPCG, Log, All);

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): PCG is queried and changed with rql-query /
 * rql-mutate instead of dedicated tools. Reflection exposes UPCGGraph and UPCGComponent; the module adds the
 * `pcgGraphs` asset source, the `pcg` operations, a graph's nodes and a component's graph / generation state (not
 * reflected properties), and the agent guide rendered into the unreal-rql skill.
 *
 * Keep GetExtensionVersion() in sync with the .uplugin VersionName (scripts/extension_release.py bump updates both).
 */
class FUnrealAIPCGRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("PCG"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-pcg"); }
	virtual FString GetExtensionVersion() const override { return TEXT("0.1.1"); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddAssetSource(TEXT("pcgGraphs"), TEXT("PCG graph assets (PCGGraph) of the project, at most 200"), UPCGGraph::StaticClass());
		Context.AddApiClass(UUnrealAIPCGRqlApi::StaticClass(), TEXT("PCGOperations"));
		Context.AddObjectSource(TEXT("pcg"), TEXT("PCG operations: call AddComponent(actor, graph) / Generate(actor)"),
			UUnrealAIPCGRqlApi::StaticClass(), []() -> UObject* { return GetMutableDefault<UUnrealAIPCGRqlApi>(); });
		Context.AddJsonMember(UPCGGraph::StaticClass(), TEXT("Nodes"), TEXT("the graph's nodes: Name, Title"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Nodes;
				if (const UPCGGraph* Graph = Cast<UPCGGraph>(Object))
				{
					for (const UPCGNode* Node : Graph->GetNodes())
					{
						if (!Node)
							continue;
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("Name"), Node->GetName());
						Entry->SetStringField(TEXT("Title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
						Nodes.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				return MakeShared<FJsonValueArray>(Nodes);
			});
		Context.AddJsonMember(UPCGComponent::StaticClass(), TEXT("Generation"), TEXT("the bound graph (path) and whether it is generating"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				if (const UPCGComponent* Component = Cast<UPCGComponent>(Object))
				{
					const UPCGGraph* Graph = Component->GetGraph();
					Out->SetStringField(TEXT("Graph"), Graph ? Graph->GetPathName() : FString());
					Out->SetBoolField(TEXT("IsGenerating"), Component->IsGenerating());
				}
				return MakeShared<FJsonValueObject>(Out);
			});
		Context.AddGuide(TEXT("PCG"), TEXT(R"MD(PCG graphs are assets: `pcgGraphs`. PCG components are components:
`actors | get Components | where $ is PCGComponent` (members: the reflected properties, plus `Generation` — the bound graph
and whether it is generating).

```
pcgGraphs | select $ref Name Nodes
actors | get Components | where $ is PCGComponent | select Owner.Label Generation
pcg | call AddComponent("Forest", "/Game/PCG/PCG_Trees")          actor (label, name or ref), optional graph
pcg | call Generate("Forest")
```

Adding a component is undoable; generation runs in the editor world.)MD"));
	}
};

/** Editor module that owns the RQL module and registers it as a modular feature, so Unreal-MCP discovers it. */
class FUnrealAIPCGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RqlModule = MakeUnique<FUnrealAIPCGRqlModule>();
		IModularFeatures::Get().RegisterModularFeature(IUnrealMcpRqlModule::GetModularFeatureName(), RqlModule.Get());
		UE_LOG(LogUnrealAIPCG, Log, TEXT("[UnrealAIPCG] registered RQL module '%s'."), *RqlModule->GetRqlModuleName());
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
	TUniquePtr<FUnrealAIPCGRqlModule> RqlModule;
};

IMPLEMENT_MODULE(FUnrealAIPCGModule, UnrealAIPCG)
