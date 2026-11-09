// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealAIPCGCatalog.h"
#include "UnrealAIPCGGraphEdit.h"
#include "UnrealAIPCGRqlApi.h"

#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"

/**
 * The extension's RQL module (Unreal-MCP docs/rql.md "Extensions"): PCG is queried and authored with rql-query /
 * rql-mutate instead of dedicated tools. Reflection exposes UPCGGraph, UPCGNode and UPCGComponent; the module adds the
 * `pcgGraphs`, `pcgNodeTypes` and `pcgTemplates` sources, the `pcg` operations (graph authoring, placement, generation),
 * the members reflection does not show (a graph's nodes, edges and parameters; a node's pins and settings as text; what a
 * component generated) and the agent guide rendered into the unreal-rql skill.
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
		Context.AddObjectListSource(TEXT("pcgNodeTypes"),
			TEXT("the PCG node types AddNode creates (Name, Type, Category, Description, Inputs, Outputs, Aliases): samplers, filters, spatial operations, spawners, ..."),
			UUnrealAIPCGNodeType::StaticClass(), []() { return UnrealAIPCGCatalog::NodeTypes(); });
		Context.AddObjectListSource(TEXT("pcgTemplates"),
			TEXT("the PCG graph templates of the engine and project (Name, Path, Category, Description); CreateGraph(path, template) copies one"),
			UUnrealAIPCGTemplate::StaticClass(), []() { return UnrealAIPCGCatalog::Templates(); });
		Context.AddApiSource(TEXT("pcg"),
			TEXT("PCG authoring: call CreateGraph / AddNode / RemoveNode / Connect / Disconnect / SetNodeSetting / SetParameter / RemoveParameter / BindParameter / AddComponent / SetGraph / SpawnVolume / Generate / Cleanup"),
			UUnrealAIPCGRqlApi::StaticClass(), TEXT("PCGOperations"));

		Context.AddObjectListMember(UPCGGraph::StaticClass(), TEXT("Nodes"),
			TEXT("the graph's nodes, the Input and Output nodes first (members Name, Title, Type, Inputs, Outputs, Settings)"), UPCGNode::StaticClass(),
			[](UObject* Object)
			{
				const UPCGGraph* Graph = Cast<UPCGGraph>(Object);
				return Graph ? TArray<UObject*>(UnrealAIPCG::AllNodes(*Graph)) : TArray<UObject*>();
			});
		Context.AddJsonMember(UPCGGraph::StaticClass(), TEXT("Edges"), TEXT("every connection: \"FromNode.Pin -> ToNode.Pin\""),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UPCGGraph* Graph = Cast<UPCGGraph>(Object);
				return Graph ? UnrealAIPCG::EdgesJson(*Graph) : MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
			});
		Context.AddJsonMember(UPCGGraph::StaticClass(), TEXT("Parameters"), TEXT("the graph parameters (user parameters): Name, Type, Value"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UPCGGraph* Graph = Cast<UPCGGraph>(Object);
				return Graph ? UnrealAIPCG::ParametersJson(*Graph) : MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
			});

		Context.AddJsonMember(UPCGNode::StaticClass(), TEXT("Title"), TEXT("the node's title in the graph editor"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UPCGNode* Node = Cast<UPCGNode>(Object);
				return MakeShared<FJsonValueString>(Node ? UnrealAIPCG::NodeTitle(*Node) : FString());
			});
		Context.AddJsonMember(UPCGNode::StaticClass(), TEXT("Type"), TEXT("the node type (a Type of pcgNodeTypes)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UPCGNode* Node = Cast<UPCGNode>(Object);
				const UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
				return MakeShared<FJsonValueString>(Settings ? UnrealAIPCGCatalog::TypeName(Settings->GetClass()) : FString());
			});
		Context.AddJsonMember(UPCGNode::StaticClass(), TEXT("Inputs"), TEXT("the input pins: Pin, Type, Connections (\"Node.Pin\"), Override (the setting a connected override pin sets)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UPCGNode* Node = Cast<UPCGNode>(Object);
				return Node ? UnrealAIPCG::PinsJson(*Node, false) : MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
			});
		Context.AddJsonMember(UPCGNode::StaticClass(), TEXT("Outputs"), TEXT("the output pins: Pin, Type, Connections (\"Node.Pin\")"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UPCGNode* Node = Cast<UPCGNode>(Object);
				return Node ? UnrealAIPCG::PinsJson(*Node, true) : MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
			});
		Context.AddJsonMember(UPCGNode::StaticClass(), TEXT("Settings"),
			TEXT("the node's settings SetNodeSetting takes: Name, Type (enums list their entries), Value (as text), Pin (the override pin BindParameter drives)"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				const UPCGNode* Node = Cast<UPCGNode>(Object);
				return Node ? UnrealAIPCG::SettingsJson(*Node) : MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
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
		Context.AddJsonMember(UPCGComponent::StaticClass(), TEXT("Generated"),
			TEXT("what the component generated: Instances, Meshes (\"<mesh>: <count>\"), Components, Actors, Points (at the graph Output), IsGenerating"),
			[](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				UPCGComponent* Component = Cast<UPCGComponent>(Object);
				return Component ? UnrealAIPCG::GeneratedJson(*Component) : MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
			});

		Context.AddGuide(TEXT("PCG"), TEXT(R"MD(Procedural placement (forests, rocks, grass, props scattered over a landscape or along splines) is a PCG graph:
author it with the `pcg` operations, never by writing C++ or Python. Needs the engine's PCG plugin (this extension
depends on it, so it is enabled with the extension). A graph is a chain of nodes: data (Get Landscape Data: the terrain
under the component; Get Spline Data; the Input node: the component's own volume) -> a sampler (points) -> filters /
spatial operations -> a spawner. Put it on a PCG Volume, generate, read the counts, change parameters or the seed, generate
again.

```
pcgNodeTypes | where Category == "Filter" | select Name Type Description           every node AddNode takes, with its pins
pcgTemplates | select Name Path Description                                       graph templates (CreateGraph copies one)
pcg | call CreateGraph("/Game/PCG/PCG_Forest")                                    or CreateGraph(path, template path)
pcg | call AddNode("/Game/PCG/PCG_Forest", "Get Landscape Data")                  returns the node's Name (GetLandscapeData)
pcg | call AddNode("/Game/PCG/PCG_Forest", "Surface Sampler")
pcg | call Connect("/Game/PCG/PCG_Forest", "GetLandscapeData", "SurfaceSampler", "", "Surface")
pcg | call SetNodeSetting("/Game/PCG/PCG_Forest", "SurfaceSampler", "PointsPerSquaredMeter", "0.02")
unreal.asset("/Game/PCG/PCG_Forest").Nodes | where Name == "SurfaceSampler" | select Settings Inputs Outputs
unreal.asset("/Game/PCG/PCG_Forest") | select Edges Parameters
pcg | call SetParameter("/Game/PCG/PCG_Forest", "Density", "0.02")                 a graph parameter (double)
pcg | call BindParameter("/Game/PCG/PCG_Forest", "Density", "SurfaceSampler", "PointsPerSquaredMeter")
pcg | call SpawnVolume("/Game/PCG/PCG_Forest", "(0, 0, 0)", "(50000, 50000, 20000)", "Forest")   500 x 500 m, 200 m tall
pcg | call Generate(unreal.find("Forest"))                                        waits: {Generated, Instances, Meshes, Errors, ...}
pcg | call Generate(unreal.find("Forest"), 7)                                     another seed: another variant
actors | where Label == "Forest" | get Components | where $ is PCGComponent | select Generated
```

A forest over hilly terrain, dense in hollows, thinner on slopes, none on steep slopes, high ground or a spline road:

```
G = "/Game/PCG/PCG_Forest"
AddNode(G, "Get Landscape Data")   AddNode(G, "Surface Sampler")   Connect(G, "GetLandscapeData", "SurfaceSampler", "", "Surface")
AddNode(G, "Normal To Density")    Connect(G, "SurfaceSampler", "NormalToDensity")        density = flatness (1 flat .. 0 wall)
AddNode(G, "Density Filter")       Connect(G, "NormalToDensity", "DensityFilter")
  SetNodeSetting(G, "DensityFilter", "LowerBound", "0.8")                                 drops slopes steeper than ~37 deg
AddNode(G, "Filter Attribute Elements by Range")      Connect(G, "DensityFilter", "AttributeFilterRange")
  SetNodeSetting(G, "AttributeFilterRange", "TargetAttribute", "$Position.Z")
  SetNodeSetting(G, "AttributeFilterRange", "MaxThreshold", "1500")   ("MinThreshold" too; output pin "InsideFilter")
AddNode(G, "Get Spline Data")      SetNodeSetting(G, "GetSplineData", "ActorSelector.ActorFilter", "AllWorldActors")
  SetNodeSetting(G, "GetSplineData", "ActorSelector.ActorSelectionTag", "Road")        the road actor's Tags has "Road"
AddNode(G, "Spline Sampler")       Connect(G, "GetSplineData", "SplineSampler")
AddNode(G, "Bounds Modifier")      Connect(G, "SplineSampler", "BoundsModifier")
  SetNodeSetting(G, "BoundsModifier", "Mode", "Set")   "BoundsMin" "(-500, -500, -5000)", "BoundsMax" "(500, 500, 5000)": 5 m each side
AddNode(G, "Difference")           Connect(G, "AttributeFilterRange", "Difference", "InsideFilter", "Source")
                                   Connect(G, "BoundsModifier", "Difference", "", "Differences")
AddNode(G, "Self Pruning")         Connect(G, "Difference", "SelfPruning")                 no overlapping trees
AddNode(G, "Transform Points")     Connect(G, "SelfPruning", "TransformPoints")
  SetNodeSetting(G, "TransformPoints", "RotationMax", "(0, 360, 0)")   ("ScaleMin" "0.8", "ScaleMax" "1.3")
AddNode(G, "Static Mesh Spawner")  Connect(G, "TransformPoints", "StaticMeshSpawner")   Connect(G, "StaticMeshSpawner", "Output")
  SetNodeSetting(G, "StaticMeshSpawner", "Meshes", "/Game/Trees/SM_Pine: 3, /Game/Trees/SM_Birch: 1")   weighted
SetParameter(G, "Density", "0.02")  BindParameter(G, "Density", "SurfaceSampler", "PointsPerSquaredMeter")
```

Denser in hollows: an "Attribute Noise" + a second Density Filter thin points randomly by density, or a
"Filter Attribute Elements by Range" on `$Position.Z` per height band with its own sampler density. Bushes and rocks:
a second Static Mesh Spawner fed by its own sampler (or a Density Filter band) of the same graph.

- **Values** are text: numbers, `"(x, y, z)"` (one number fills all; rotators `(pitch, yaw, roll)`), `"#rrggbb"`, enum
  entries by name (the Settings member lists them), `true` / `false`, asset paths, attribute selectors (`"$Position.Z"`,
  `"$Density"`, `"MyAttribute"`), a threshold as a number or an attribute, UE text for other structs. Nested settings by
  path: `"ActorSelector.ActorSelectionTag"`, `"SamplerParams.Dimension"`. A wrong name fails listing what exists.
- **Names**: AddNode returns the node's Name (the type for the first one, `SurfaceSampler`, then `SurfaceSampler_1`, ...; or
  your own: `AddNode(G, "Static Mesh Spawner", "Trees")`); nodes are also found by title; pins by label (`""` = the main
  pin; a setting's name reaches its override pin); `Input` / `Output` are the graph's own nodes (wire the spawner to Output
  so Generate counts the final points).
- **Placement**: `SpawnVolume` (a PCG Volume: its box is the area; cover the terrain's heights), or `SetGraph(actor, graph)`
  / `AddComponent(actor, graph)` on any actor with bounds. The component's `Seed` (`Generate(actor, seed)`) varies the result.
- Generation runs in the editor world and is not part of the undo step; `Cleanup(actor)` removes what it made. Created
  graphs stay unsaved until `editor | call SaveAsset(path)`.
- `Generate` waits at most 45 s. `Generated: false` means it goes on in the editor: read the `Generated` member until
  `IsGenerating` is false instead of generating again.
- Save after generating: `editor | call SaveLevel()` stores the generated instances in the map, so it reopens with the forest
  and needs no new `Generate`. A level saved before generating reopens without it in the editor: `GenerationTrigger`
  `GenerateOnLoad` generates when a game starts, not when the editor opens the map.)MD"));
	}
};

IMPLEMENT_MODULE(TUnrealMcpRqlEditorModule<FUnrealAIPCGRqlModule>, UnrealAIPCG)
