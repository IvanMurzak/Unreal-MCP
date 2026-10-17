// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIPCGRqlApi.h"

#include "PCGCommon.h" // EPCGNodeTitleType
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGNode.h"

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): PCG is queried and changed with rql-query /
 * rql-mutate instead of dedicated tools. Reflection exposes UPCGGraph and UPCGComponent; the module adds the
 * `pcgGraphs` asset source, the `pcg` operations, a graph's nodes and a component's bound graph / generation state (not
 * reflected properties), and the agent guide rendered into the unreal-rql skill.
 *
 * The version is the .uplugin VersionName.
 */
class FUnrealAIPCGRqlModule : public IUnrealMcpRqlModule
{
public:
	virtual FString GetRqlModuleName() const override { return TEXT("PCG"); }
	virtual FString GetExtensionId() const override { return TEXT("com.ivanmurzak.unreal-ai-pcg"); }
	virtual FString GetExtensionVersion() const override { return UnrealMcpRql::PluginVersion(TEXT("UnrealAIPCG")); }

	virtual void ConfigureRql(FUnrealMcpRqlModuleContext& Context) override
	{
		Context.AddAssetSource(TEXT("pcgGraphs"), TEXT("PCG graph assets (PCGGraph) of the project, at most 200"), UPCGGraph::StaticClass());
		Context.AddApiSource(TEXT("pcg"), TEXT("PCG operations: call AddComponent(actor, graph) / Generate(actor)"),
			UUnrealAIPCGRqlApi::StaticClass(), TEXT("PCGOperations"));
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
		Context.AddObjectMember(UPCGComponent::StaticClass(), TEXT("BoundGraph"), TEXT("the PCG graph the component generates"),
			UPCGGraph::StaticClass(), [](UObject* Object) -> UObject*
			{
				const UPCGComponent* Component = Cast<UPCGComponent>(Object);
				return Component ? Component->GetGraph() : nullptr;
			});
		Context.AddJsonMember(UPCGComponent::StaticClass(), TEXT("IsGenerating"), TEXT("whether the component is generating now"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UPCGComponent* Component = Cast<UPCGComponent>(Object);
				return MakeShared<FJsonValueBoolean>(Component && Component->IsGenerating());
			});
		Context.AddGuide(TEXT("PCG"), TEXT(R"MD(PCG graphs are assets: `pcgGraphs`. PCG components are components:
`actors | get Components | where $ is PCGComponent` (members: the reflected properties, plus `BoundGraph` and
`IsGenerating`).

```
pcgGraphs | select $ref Name Nodes
actors | get Components | where $ is PCGComponent | select Owner.Label BoundGraph IsGenerating
pcg | call AddComponent("Forest", "/Game/PCG/PCG_Trees")          actor (label, name or ref), optional graph
pcg | call Generate("Forest")
```

Adding a component is undoable; generation runs in the editor world.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIPCGRqlModule>, UnrealAIPCG)
