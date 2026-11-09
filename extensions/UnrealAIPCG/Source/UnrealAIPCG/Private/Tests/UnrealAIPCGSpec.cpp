// Copyright (c) 2026 IvanMurzak/Unreal-AI-PCG. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Components/BoxComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "EngineUtils.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "MeshSelectors/PCGMeshSelectorWeighted.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGVolume.h"

#include "UnrealAIPCGCatalog.h"
#include "UnrealAIPCGGraphEdit.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions").
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::RunPlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it (the `pcg | call` plans are built by
//  Call() below: one call stage with literal arguments, as Rql.Parser writes it). Authoring results are read back from
//  the graph asset; generation from the PCG component of a placed PCG Volume.
//
//  Run via:  Automation RunTests UnrealAIPCG
// ============================================================================================

namespace UnrealAIPCGSpecLocal
{
	FString Json(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		if (Object.IsValid())
			FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		return Out;
	}

	/** A literal argument: text, a number or a bool. */
	TSharedPtr<FJsonValue> Lit(const FString& Text) { return MakeShared<FJsonValueString>(Text); }
	TSharedPtr<FJsonValue> Lit(double Number) { return MakeShared<FJsonValueNumber>(Number); }

	/** The plan of `pcg | call Method(args...)`. */
	FString Call(const FString& Method, const TArray<TSharedPtr<FJsonValue>>& Args)
	{
		TArray<TSharedPtr<FJsonValue>> Literals;
		for (const TSharedPtr<FJsonValue>& Arg : Args)
		{
			TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
			Literal->SetField(TEXT("lit"), Arg);
			Literals.Add(MakeShared<FJsonValueObject>(Literal));
		}
		TSharedPtr<FJsonObject> Stage = MakeShared<FJsonObject>();
		Stage->SetStringField(TEXT("op"), TEXT("call"));
		Stage->SetStringField(TEXT("method"), Method);
		Stage->SetArrayField(TEXT("args"), Literals);
		TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetArrayField(TEXT("path"), { MakeShared<FJsonValueString>(TEXT("pcg")) });
		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("rql"), TEXT("0.1"));
		Plan->SetObjectField(TEXT("source"), Source);
		Plan->SetArrayField(TEXT("stages"), { MakeShared<FJsonValueObject>(Stage) });
		return Json(Plan);
	}

	/** Run `pcg | call Method(args...)` as rql-mutate; the result (and its error) in @p Out. */
	bool Run(const FString& Method, const TArray<TSharedPtr<FJsonValue>>& Args, TSharedPtr<FJsonObject>* Out = nullptr)
	{
		TSharedPtr<FJsonObject> Result;
		const bool bOk = UnrealMcpRqlTools::RunPlan(Call(Method, Args), true, &Result);
		// Every call and its answer in the log: a failing expectation is read next to what the operation said.
		UE_LOG(LogTemp, Display, TEXT("UnrealAIPCGSpec pcg | call %s -> %s"), *Method, *Json(Result).Left(2000));
		if (Out)
			*Out = Result;
		return bOk;
	}

	UPCGGraph* LoadGraph(const FString& Path)
	{
		return FindObject<UPCGGraph>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	}

	bool HasEdge(const UPCGGraph& Graph, const FString& Edge)
	{
		const TSharedPtr<FJsonValue> Edges = UnrealAIPCG::EdgesJson(Graph);
		for (const TSharedPtr<FJsonValue>& Value : Edges->AsArray())
		{
			if (Value->AsString() == Edge)
				return true;
		}
		return false;
	}

	FString SettingValue(UPCGGraph& Graph, const FString& Node, const FString& Setting)
	{
		UPCGNode* Found = UnrealAIPCG::FindNode(Graph, Node);
		if (!Found)
			return TEXT("<no node>");
		const TSharedPtr<FJsonValue> Settings = UnrealAIPCG::SettingsJson(*Found);
		for (const TSharedPtr<FJsonValue>& Row : Settings->AsArray())
		{
			if (Row->AsObject()->GetStringField(TEXT("Name")) == Setting)
				return Row->AsObject()->GetStringField(TEXT("Value"));
		}
		return TEXT("<no setting>");
	}

	/** Wires graph @p G: Create Points Grid -> Static Mesh Spawner of @p Mesh -> Output. */
	bool AddMeshGrid(const FString& G, const FString& Mesh)
	{
		return Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("Create Points Grid")) })
			&& Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("Static Mesh Spawner")) })
			&& Run(TEXT("Connect"), { Lit(G), Lit(TEXT("CreatePointsGrid")), Lit(TEXT("StaticMeshSpawner")) })
			&& Run(TEXT("Connect"), { Lit(G), Lit(TEXT("StaticMeshSpawner")), Lit(TEXT("Output")) })
			&& Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("StaticMeshSpawner")), Lit(TEXT("Meshes")), Lit(Mesh) });
	}

	/** editor | call DeleteAsset(path, true): the test leaves no unsaved asset behind. */
	bool DeleteAsset(const FString& Path)
	{
		return UnrealMcpRqlTools::RunPlan(FString(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"DeleteAsset","args":[{"lit":"@PATH@"},{"lit":true}]}]})RQL")).Replace(TEXT("@PATH@"), *Path), true);
	}

	void DestroyLabelled(const FString& Label)
	{
		if (UWorld* World = UnrealMcpRql::EditorWorld())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == Label)
					World->EditorDestroyActor(*It, true);
			}
		}
	}
}

BEGIN_DEFINE_SPEC(FUnrealAIPCGSpec, "UnrealAIPCG",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealAIPCGSpec)

void FUnrealAIPCGSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-pcg"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("PCG")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `pcgGraphs` |"), Skill.Contains(TEXT("| `pcgGraphs` |")));
			TestTrue(TEXT("| `pcg` |"), Skill.Contains(TEXT("| `pcg` |")));
			TestTrue(TEXT("## PCG"), Skill.Contains(TEXT("## PCG")));
		});
	});

	Describe("sources and members", [this]()
	{
		It("pcgGraphs answers a query", [this]()
		{
			// pcgGraphs | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcgGraphs"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("describes PCGGraph with its Nodes member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("PCGGraph"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:Nodes"), Schema.Contains(TEXT("\"name\":\"Nodes\"")));
		});

		It("describes PCGComponent with its BoundGraph and IsGenerating members", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("PCGComponent"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:BoundGraph"), Schema.Contains(TEXT("\"name\":\"BoundGraph\"")));
			TestTrue(TEXT("lists name:IsGenerating"), Schema.Contains(TEXT("\"name\":\"IsGenerating\"")));
		});

		It("matches a placed PCGComponent with `where $ is PCGComponent`", [this]()
		{
			// The product path of the extension's e2e count (`actors | get Components | where $ is PCGComponent | count`): with
			// a PCGComponent in the level the filter matches it -- an empty level answers 0 with RQL's no-match warning.
			if (!TestNotNull(TEXT("GEngine"), GEngine))
				return;
			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::Editor)
				{
					World = Context.World();
					break;
				}
			}
			if (!TestNotNull(TEXT("the editor world"), World))
				return;
			AActor* Host = World->SpawnActor<AActor>();
			if (!TestNotNull(TEXT("a host actor"), Host))
				return;
			const FString Label = TEXT("RqlSpecPCGComponentHost_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Host->SetActorLabel(Label);
			// PCG registers a component only on an actor with bounds (else LogPCG errors "invalid bounds"): a box root.
			UBoxComponent* Bounds = NewObject<UBoxComponent>(Host, TEXT("RqlSpecPCGBounds"));
			Bounds->SetBoxExtent(FVector(100.0));
			Host->SetRootComponent(Bounds);
			Host->AddInstanceComponent(Bounds);
			Bounds->RegisterComponent();
			UPCGComponent* Component = NewObject<UPCGComponent>(Host, TEXT("RqlSpecPCGComponent"));
			Host->AddInstanceComponent(Component);
			Component->RegisterComponent();

			// actors | where Label == "@LABEL@" | get Components | where $ is PCGComponent | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(FString(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"@LABEL@"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"PCGComponent"}]}},{"op":"count"}]})RQL")).Replace(TEXT("@LABEL@"), *Label), false, &Result);
			FString Text;
			if (Result.IsValid())
				FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
			TestTrue(*FString::Printf(TEXT("succeeds (%s)"), *Text.Left(400)), bResultOk);
			TestEqual(*FString::Printf(TEXT("counts the placed component (%s)"), *Text.Left(400)),
				Result.IsValid() ? static_cast<int32>(Result->GetNumberField(TEXT("value"))) : -1, 1);
			TestFalse(*FString::Printf(TEXT("no no-match warning (%s)"), *Text.Left(400)), Text.Contains(TEXT("no-match")));
			World->DestroyActor(Host);
		});
	});

	Describe("operations", [this]()
	{
		It("lists AddComponent and Generate", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("PCGOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists AddComponent("), Schema.Contains(TEXT("AddComponent(")));
			TestTrue(TEXT("lists Generate("), Schema.Contains(TEXT("Generate(")));
		});

		It("AddComponent fails defensively on an unknown actor", [this]()
		{
			// pcg | call AddComponent("RqlSpecNoActor")
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcg"]},"stages":[{"op":"call","method":"AddComponent","args":[{"lit":"RqlSpecNoActor"}]}]})RQL"), true, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), bResultOk);
		});

		It("Generate fails defensively on an unknown actor", [this]()
		{
			// pcg | call Generate("RqlSpecNoActor")
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcg"]},"stages":[{"op":"call","method":"Generate","args":[{"lit":"RqlSpecNoActor"}]}]})RQL"), true, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), bResultOk);
		});

		It("lists every authoring operation", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("PCGOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			for (const TCHAR* Operation : { TEXT("CreateGraph("), TEXT("AddNode("), TEXT("RemoveNode("), TEXT("Connect("), TEXT("Disconnect("), TEXT("SetNodeSetting("),
				TEXT("SetParameter("), TEXT("RemoveParameter("), TEXT("BindParameter("), TEXT("SetGraph("), TEXT("SpawnVolume("), TEXT("Cleanup(") })
				TestTrue(FString::Printf(TEXT("lists %s"), Operation), Schema.Contains(Operation));
		});
	});

	Describe("catalog", [this]()
	{
		It("pcgNodeTypes lists the palette's nodes with descriptions and pins", [this]()
		{
			const TArray<UObject*> Rows = UnrealAIPCGCatalog::NodeTypes();
			TestTrue(TEXT("dozens of node types"), Rows.Num() > 50);
			const auto Find = [&Rows](const TCHAR* Name) -> const UUnrealAIPCGNodeType*
			{
				for (const UObject* Row : Rows)
				{
					const UUnrealAIPCGNodeType* Type = Cast<UUnrealAIPCGNodeType>(Row);
					if (Type && Type->Name == Name)
						return Type;
				}
				return nullptr;
			};
			const UUnrealAIPCGNodeType* Sampler = Find(TEXT("Surface Sampler"));
			if (TestNotNull(TEXT("Surface Sampler"), Sampler))
			{
				TestEqual(TEXT("its type"), Sampler->Type, FString(TEXT("SurfaceSampler")));
				TestFalse(TEXT("a description"), Sampler->Description.IsEmpty());
				TestTrue(TEXT("a Surface input"), Sampler->Inputs.Contains(TEXT("Surface")));
			}
			for (const TCHAR* Name : { TEXT("Density Filter"), TEXT("Difference"), TEXT("Self Pruning"), TEXT("Transform Points"), TEXT("Static Mesh Spawner"),
				TEXT("Spline Sampler"), TEXT("Get Spline Data"), TEXT("Normal To Density"), TEXT("Bounds Modifier") })
				TestNotNull(Name, Find(Name));

			// pcgNodeTypes | where Category == "Filter" | count
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("pcgNodeTypes answers a query"), UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcgNodeTypes"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Category"]},{"lit":"Filter"}]}},{"op":"count"}]})RQL"), false, &Result));
			TestTrue(TEXT("with filters"), Result.IsValid() && Result->GetNumberField(TEXT("value")) > 0);
		});

		It("pcgTemplates lists graph templates CreateGraph copies", [this]()
		{
			// pcgTemplates | count
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("pcgTemplates answers a query"), UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcgTemplates"]},"stages":[{"op":"count"}]})RQL"), false, &Result));
			const UUnrealAIPCGTemplate* Template = nullptr;
			for (const UObject* Row : UnrealAIPCGCatalog::Templates())
			{
				Template = Cast<UUnrealAIPCGTemplate>(Row);
				if (Template && Template->Path.StartsWith(TEXT("/PCG/")))
					break;
				Template = nullptr;
			}
			if (!Template)
				return; // the engine's PCG content is not in the asset registry of this run
			const FString Path = TEXT("/Game/RqlSpecPCG/PCG_FromTemplate_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			// pcg | call CreateGraph("@PATH@", "<template>")
			TestTrue(*FString::Printf(TEXT("created from %s"), *Template->Path), UnrealAIPCGSpecLocal::Run(TEXT("CreateGraph"), { UnrealAIPCGSpecLocal::Lit(Path), UnrealAIPCGSpecLocal::Lit(Template->Path) }));
			const UPCGGraph* Graph = UnrealAIPCGSpecLocal::LoadGraph(Path);
			if (TestNotNull(TEXT("the copy"), Graph))
				TestFalse(TEXT("the copy is not a template"), Graph->bIsTemplate);
			TestTrue(TEXT("test asset deleted"), UnrealAIPCGSpecLocal::DeleteAsset(Path));
		});
	});

	Describe("authoring", [this]()
	{
		It("builds and wires a graph: nodes, connections, settings, meshes, members", [this]()
		{
			using namespace UnrealAIPCGSpecLocal;
			const FString G = TEXT("/Game/RqlSpecPCG/PCG_Build_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			// pcg | call CreateGraph("@G@")
			TestTrue(TEXT("created"), Run(TEXT("CreateGraph"), { Lit(G) }));
			UPCGGraph* Graph = LoadGraph(G);
			if (!TestNotNull(TEXT("the graph asset"), Graph))
				return;

			// pcg | call AddNode("@G@", "Create Points Grid")      ... "Density Filter", "Static Mesh Spawner", "Trees"
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("grid added"), Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("Create Points Grid")) }, &Result));
			TestTrue(TEXT("returns the node's name"), Json(Result).Contains(TEXT("CreatePointsGrid")));
			TestTrue(TEXT("filter added"), Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("density filter")) }));
			TestTrue(TEXT("spawner added with a name"), Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("StaticMeshSpawner")), Lit(TEXT("Trees")) }));
			TestEqual(TEXT("three nodes"), Graph->GetNodes().Num(), 3);

			// pcg | call Connect("@G@", "CreatePointsGrid", "DensityFilter")      ... ("DensityFilter", "Trees"), ("Trees", "Output")
			TestTrue(TEXT("grid -> filter"), Run(TEXT("Connect"), { Lit(G), Lit(TEXT("CreatePointsGrid")), Lit(TEXT("Density Filter")) }));
			TestTrue(TEXT("filter -> spawner"), Run(TEXT("Connect"), { Lit(G), Lit(TEXT("DensityFilter")), Lit(TEXT("Trees")) }));
			TestTrue(TEXT("spawner -> output"), Run(TEXT("Connect"), { Lit(G), Lit(TEXT("Trees")), Lit(TEXT("Output")) }));
			TestTrue(TEXT("Edges lists spawner -> output"), HasEdge(*Graph, TEXT("Trees.Out -> Output.Out")));
			TestTrue(TEXT("Edges lists grid -> filter"), HasEdge(*Graph, TEXT("CreatePointsGrid.Out -> DensityFilter.In")));
			TestTrue(TEXT("Edges lists filter -> spawner"), HasEdge(*Graph, TEXT("DensityFilter.Out -> Trees.In")));

			// pcg | call SetNodeSetting("@G@", "CreatePointsGrid", "CellSize", "(250, 250, 100)")
			TestTrue(TEXT("a vector setting"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("CreatePointsGrid")), Lit(TEXT("Cell Size")), Lit(TEXT("(250, 250, 100)")) }));
			TestEqual(TEXT("cell size"), SettingValue(*Graph, TEXT("CreatePointsGrid"), TEXT("CellSize")), FString(TEXT("(250, 250, 100)")));
			// pcg | call SetNodeSetting("@G@", "DensityFilter", "LowerBound", "0.25")      ("bInvertFilter" by its short name)
			TestTrue(TEXT("a number"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("DensityFilter")), Lit(TEXT("LowerBound")), Lit(TEXT("0.25")) }));
			TestEqual(TEXT("lower bound"), SettingValue(*Graph, TEXT("DensityFilter"), TEXT("LowerBound")), FString(TEXT("0.25")));
			TestTrue(TEXT("a bool by its short name"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("DensityFilter")), Lit(TEXT("InvertFilter")), Lit(TEXT("false")) }));
			// pcg | call SetNodeSetting("@G@", "CreatePointsGrid", "PointPosition", "CellCorners")
			TestTrue(TEXT("an enum entry"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("CreatePointsGrid")), Lit(TEXT("PointPosition")), Lit(TEXT("CellCorners")) }));
			TestEqual(TEXT("point position"), SettingValue(*Graph, TEXT("CreatePointsGrid"), TEXT("PointPosition")), FString(TEXT("CellCorners")));
			TestTrue(TEXT("back to the centre"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("CreatePointsGrid")), Lit(TEXT("PointPosition")), Lit(TEXT("CellCenter")) }));
			// pcg | call SetNodeSetting("@G@", "Trees", "Meshes", "/Engine/BasicShapes/Cube: 3, /Engine/BasicShapes/Cylinder")
			TestTrue(TEXT("weighted meshes"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("Trees")), Lit(TEXT("Meshes")), Lit(TEXT("/Engine/BasicShapes/Cube: 3, /Engine/BasicShapes/Cylinder")) }));
			TestEqual(TEXT("mesh entries"), SettingValue(*Graph, TEXT("Trees"), TEXT("MeshSelectorParameters.MeshEntries")),
				FString(TEXT("/Engine/BasicShapes/Cube: 3, /Engine/BasicShapes/Cylinder: 1")));

			// pcgGraphs | where Name == "@NAME@" | get Nodes | where Name == "Trees" | select Title Type Inputs Settings
			const FString NodesPlan = FString(TEXT(R"RQL({"rql":"0.1","source":{"path":["pcgGraphs"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"@NAME@"}]}},{"op":"get","expr":{"path":["Nodes"]}},{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Trees"}]}},{"op":"select","fields":[{"name":"Title","expr":{"path":["Title"]}},{"name":"Type","expr":{"path":["Type"]}},{"name":"Inputs","expr":{"path":["Inputs"]}},{"name":"Settings","expr":{"path":["Settings"]}}]}]})RQL")).Replace(TEXT("@NAME@"), *FPackageName::GetShortName(G));
			TSharedPtr<FJsonObject> Nodes;
			TestTrue(TEXT("Nodes answers a query"), UnrealMcpRqlTools::RunPlan(NodesPlan, false, &Nodes));
			TestTrue(*FString::Printf(TEXT("the node's type (%s)"), *Json(Nodes).Left(600)), Json(Nodes).Contains(TEXT("StaticMeshSpawner")));
			TestTrue(TEXT("its connection"), Json(Nodes).Contains(TEXT("DensityFilter.Out")));
			TestTrue(TEXT("its settings"), Json(Nodes).Contains(TEXT("MeshSelectorParameters.MeshEntries")));
			TestTrue(TEXT("the graph is unsaved"), Graph->GetPackage()->IsDirty());

			// pcg | call Disconnect("@G@", "Trees", "Output")      pcg | call RemoveNode("@G@", "DensityFilter")
			TestTrue(TEXT("disconnected"), Run(TEXT("Disconnect"), { Lit(G), Lit(TEXT("Trees")), Lit(TEXT("Output")) }, &Result));
			TestFalse(TEXT("no spawner -> output"), HasEdge(*Graph, TEXT("Trees.Out -> Output.Out")));
			TestTrue(TEXT("removed"), Run(TEXT("RemoveNode"), { Lit(G), Lit(TEXT("DensityFilter")) }));
			TestEqual(TEXT("two nodes"), Graph->GetNodes().Num(), 2);
			TestFalse(TEXT("its edges went with it"), HasEdge(*Graph, TEXT("CreatePointsGrid.Out -> DensityFilter.In")));
			TestTrue(TEXT("test asset deleted"), DeleteAsset(G));
		});

		It("fails with actionable errors that name what exists", [this]()
		{
			using namespace UnrealAIPCGSpecLocal;
			const FString G = TEXT("/Game/RqlSpecPCG/PCG_Errors_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			TestTrue(TEXT("created"), Run(TEXT("CreateGraph"), { Lit(G) }));
			TestTrue(TEXT("sampler added"), Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("Surface Sampler")) }));
			TSharedPtr<FJsonObject> Result;
			// pcg | call AddNode("@G@", "Densty")
			TestFalse(TEXT("an unknown node type fails"), Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("Densty")) }, &Result));
			TestTrue(TEXT("pointing at pcgNodeTypes"), Json(Result).Contains(TEXT("pcgNodeTypes")));
			// pcg | call Connect("@G@", "Input", "Samplr")
			TestFalse(TEXT("an unknown node fails"), Run(TEXT("Connect"), { Lit(G), Lit(TEXT("Input")), Lit(TEXT("Samplr")) }, &Result));
			TestTrue(TEXT("naming the nodes"), Json(Result).Contains(TEXT("SurfaceSampler")));
			// pcg | call Connect("@G@", "Input", "SurfaceSampler", "Terrain", "Surface")
			TestFalse(TEXT("an unknown pin fails"), Run(TEXT("Connect"), { Lit(G), Lit(TEXT("Input")), Lit(TEXT("SurfaceSampler")), Lit(TEXT("Terrain")), Lit(TEXT("Surface")) }, &Result));
			TestTrue(TEXT("naming the pins"), Json(Result).Contains(TEXT("output pins: In")));
			// pcg | call SetNodeSetting("@G@", "SurfaceSampler", "PointsPerAcre", "1")
			TestFalse(TEXT("an unknown setting fails"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("SurfaceSampler")), Lit(TEXT("PointsPerAcre")), Lit(TEXT("1")) }, &Result));
			TestTrue(TEXT("naming the settings"), Json(Result).Contains(TEXT("PointsPerSquaredMeter")));
			// pcg | call SetNodeSetting("@G@", "SurfaceSampler", "PointsPerSquaredMeter", "lots")
			TestFalse(TEXT("a bad value fails"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("SurfaceSampler")), Lit(TEXT("PointsPerSquaredMeter")), Lit(TEXT("lots")) }, &Result));
			TestTrue(TEXT("saying what it takes"), Json(Result).Contains(TEXT("takes a number")));
			TestTrue(TEXT("grid added"), Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("Create Points Grid")) }));
			// pcg | call SetNodeSetting("@G@", "CreatePointsGrid", "PointPosition", "Middle")
			TestFalse(TEXT("an unknown enum entry fails"), Run(TEXT("SetNodeSetting"), { Lit(G), Lit(TEXT("CreatePointsGrid")), Lit(TEXT("PointPosition")), Lit(TEXT("Middle")) }, &Result));
			TestTrue(TEXT("listing the entries"), Json(Result).Contains(TEXT("CellCenter")));
			// pcg | call BindParameter("@G@", "Missing", "SurfaceSampler", "PointsPerSquaredMeter")
			TestFalse(TEXT("an unknown parameter fails"), Run(TEXT("BindParameter"), { Lit(G), Lit(TEXT("Missing")), Lit(TEXT("SurfaceSampler")), Lit(TEXT("PointsPerSquaredMeter")) }, &Result));
			TestTrue(TEXT("saying how to add one"), Json(Result).Contains(TEXT("SetParameter")));
			// pcg | call RemoveNode("@G@", "Input")
			TestFalse(TEXT("the Input node stays"), Run(TEXT("RemoveNode"), { Lit(G), Lit(TEXT("Input")) }, &Result));
			TestTrue(TEXT("test asset deleted"), DeleteAsset(G));
		});

		It("graph parameters drive settings: SetParameter, BindParameter, Parameters", [this]()
		{
			using namespace UnrealAIPCGSpecLocal;
			const FString G = TEXT("/Game/RqlSpecPCG/PCG_Params_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			TestTrue(TEXT("created"), Run(TEXT("CreateGraph"), { Lit(G) }));
			UPCGGraph* Graph = LoadGraph(G);
			if (!TestNotNull(TEXT("the graph asset"), Graph))
				return;
			TestTrue(TEXT("sampler added"), Run(TEXT("AddNode"), { Lit(G), Lit(TEXT("Surface Sampler")) }));
			// pcg | call SetParameter("@G@", "Density", "0.05")
			TestTrue(TEXT("a double parameter"), Run(TEXT("SetParameter"), { Lit(G), Lit(TEXT("Density")), Lit(TEXT("0.05")) }));
			TestTrue(TEXT("a vector parameter"), Run(TEXT("SetParameter"), { Lit(G), Lit(TEXT("Spacing")), Lit(TEXT("(100, 100, 100)")) }));
			TestTrue(TEXT("an integer parameter by type"), Run(TEXT("SetParameter"), { Lit(G), Lit(TEXT("Count")), Lit(TEXT("3")), Lit(TEXT("int")) }));
			TestTrue(TEXT("its default changed"), Run(TEXT("SetParameter"), { Lit(G), Lit(TEXT("density")), Lit(TEXT("0.02")) }));
			const FString Parameters = FString::JoinBy(UnrealAIPCG::ParametersJson(*Graph)->AsArray(), TEXT("|"),
				[](const TSharedPtr<FJsonValue>& Row) { return Row->AsObject()->GetStringField(TEXT("Name")) + TEXT("=") + Row->AsObject()->GetStringField(TEXT("Value")); });
			TestEqual(TEXT("Parameters"), Parameters, FString(TEXT("Density=0.02|Spacing=(100, 100, 100)|Count=3")));
			TSharedPtr<FJsonObject> Result;
			TestFalse(TEXT("a value of the wrong type fails"), Run(TEXT("SetParameter"), { Lit(G), Lit(TEXT("Count")), Lit(TEXT("many")) }, &Result));

			// pcg | call BindParameter("@G@", "Density", "SurfaceSampler", "PointsPerSquaredMeter")
			TestTrue(TEXT("bound"), Run(TEXT("BindParameter"), { Lit(G), Lit(TEXT("Density")), Lit(TEXT("SurfaceSampler")), Lit(TEXT("PointsPerSquaredMeter")) }, &Result));
			TestTrue(TEXT("to the override pin"), Json(Result).Contains(TEXT("Density -> SurfaceSampler.")));
			TestEqual(TEXT("a Get Graph Parameter node"), Graph->GetNodes().Num(), 2);
			TestTrue(TEXT("bound again (reusing the node)"), Run(TEXT("BindParameter"), { Lit(G), Lit(TEXT("Density")), Lit(TEXT("SurfaceSampler")), Lit(TEXT("Looseness")) }));
			TestEqual(TEXT("still one getter"), Graph->GetNodes().Num(), 2);
			// pcg | call RemoveParameter("@G@", "Count")
			TestTrue(TEXT("removed"), Run(TEXT("RemoveParameter"), { Lit(G), Lit(TEXT("Count")) }));
			TestEqual(TEXT("two parameters left"), UnrealAIPCG::ParametersJson(*Graph)->AsArray().Num(), 2);
			TestTrue(TEXT("test asset deleted"), DeleteAsset(G));
		});

		It("generates with a mesh not loaded yet without stalling the wait", [this]()
		{
			using namespace UnrealAIPCGSpecLocal;
			// A mesh nothing else loads: the spawner must load it asynchronously, which only a frame would finish.
			const FString Mesh = TEXT("/Engine/ArtTools/RenderToTexture/Meshes/S_1_Unit_Sphere_HI.S_1_Unit_Sphere_HI");
			if (FindObject<UStaticMesh>(nullptr, *Mesh))
			{
				AddWarning(TEXT("the mesh is already loaded in this editor: the stall this covers cannot happen"));
			}
			IConsoleVariable* DelayFrames = IConsoleManager::Get().FindConsoleVariable(TEXT("s.StreamableDelegateDelayFrames"));
			const int32 DelayFramesBefore = DelayFrames ? DelayFrames->GetInt() : 0;
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			const FString G = TEXT("/Game/RqlSpecPCG/PCG_Load_") + Id;
			const FString Label = TEXT("RqlSpecPCGLoad_") + Id;
			TestTrue(TEXT("created"), Run(TEXT("CreateGraph"), { Lit(G) }));
			TestTrue(TEXT("a mesh on each grid point"), AddMeshGrid(G, TEXT("/Engine/BasicShapes/Cube")));
			// SetNodeSetting loads the mesh it is given: write the path directly, so it is still unloaded at Generate.
			UPCGGraph* Graph = LoadGraph(G);
			const UPCGNode* Spawner = Graph ? UnrealAIPCG::FindNode(*Graph, TEXT("StaticMeshSpawner")) : nullptr;
			const UPCGStaticMeshSpawnerSettings* Settings = Spawner ? Cast<UPCGStaticMeshSpawnerSettings>(Spawner->GetSettings()) : nullptr;
			UPCGMeshSelectorWeighted* Selector = Settings ? Cast<UPCGMeshSelectorWeighted>(Settings->MeshSelectorParameters) : nullptr;
			if (!TestNotNull(TEXT("the spawner's weighted mesh selector"), Selector))
			{
				DeleteAsset(G);
				return;
			}
			Selector->MeshEntries = { FPCGMeshSelectorWeightedEntry(TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(Mesh)), 1) };
			TestTrue(TEXT("volume placed"), Run(TEXT("SpawnVolume"), { Lit(G), Lit(TEXT("(0, 0, 0)")), Lit(TEXT("(2000, 2000, 1000)")), Lit(Label) }));

			// pcg | call Generate("@LABEL@", -1, 20): the stall ran out this whole budget and reported Generated false.
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("generated"), Run(TEXT("Generate"), { Lit(Label), Lit(-1.0), Lit(20.0) }, &Result));
			TestTrue(*FString::Printf(TEXT("finished within the wait (%s)"), *Json(Result).Left(600)), Json(Result).Contains(TEXT("Generated\":true")));
			TestTrue(TEXT("counted the mesh"), Json(Result).Contains(TEXT("S_1_Unit_Sphere_HI: ")));
			if (DelayFrames)
				TestEqual(TEXT("the load callback delay is restored"), DelayFrames->GetInt(), DelayFramesBefore);

			TestTrue(TEXT("cleaned up"), Run(TEXT("Cleanup"), { Lit(Label) }));
			DestroyLabelled(Label);
			TestTrue(TEXT("test asset deleted"), DeleteAsset(G));
		});

		It("generates on a PCG volume, regenerates with another parameter and seed, and cleans up", [this]()
		{
			using namespace UnrealAIPCGSpecLocal;
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
			const FString G = TEXT("/Game/RqlSpecPCG/PCG_Generate_") + Id;
			const FString Label = TEXT("RqlSpecPCGVolume_") + Id;
			TestTrue(TEXT("created"), Run(TEXT("CreateGraph"), { Lit(G) }));
			UPCGGraph* Graph = LoadGraph(G);
			if (!TestNotNull(TEXT("the graph asset"), Graph))
				return;
			// A grid of points (world space, 1000 x 1000 cm), a cube on each: no landscape needed.
			TestTrue(TEXT("a cube on each grid point"), AddMeshGrid(G, TEXT("/Engine/BasicShapes/Cube")));
			TestTrue(TEXT("spacing parameter"), Run(TEXT("SetParameter"), { Lit(G), Lit(TEXT("Spacing")), Lit(TEXT("(100, 100, 100)")) }));
			TestTrue(TEXT("bound to the cell size"), Run(TEXT("BindParameter"), { Lit(G), Lit(TEXT("Spacing")), Lit(TEXT("CreatePointsGrid")), Lit(TEXT("CellSize")) }));

			// pcg | call SpawnVolume("@G@", "(0, 0, 0)", "(2000, 2000, 1000)", "@LABEL@")
			TestTrue(TEXT("volume placed"), Run(TEXT("SpawnVolume"), { Lit(G), Lit(TEXT("(0, 0, 0)")), Lit(TEXT("(2000, 2000, 1000)")), Lit(Label) }));
			APCGVolume* Volume = nullptr;
			if (UWorld* World = UnrealMcpRql::EditorWorld())
			{
				for (TActorIterator<APCGVolume> It(World); It; ++It)
				{
					if (It->GetActorLabel() == Label)
						Volume = *It;
				}
			}
			if (!TestNotNull(TEXT("the PCG volume"), Volume) || !TestNotNull(TEXT("its PCG component"), Volume->PCGComponent.Get()))
			{
				DeleteAsset(G);
				return;
			}
			TestEqual(TEXT("bound to the graph"), Volume->PCGComponent->GetGraph(), Graph);
			TestTrue(TEXT("sized by its scale"), Volume->GetActorScale3D().Equals(FVector(10.0, 10.0, 5.0)));

			// pcg | call Generate("@LABEL@")
			TSharedPtr<FJsonObject> Result;
			TestTrue(TEXT("generated"), Run(TEXT("Generate"), { Lit(Label) }, &Result));
			TestTrue(*FString::Printf(TEXT("finished (%s)"), *Json(Result).Left(600)), Json(Result).Contains(TEXT("Generated\":true")));
			const int32 Dense = UnrealAIPCG::CountGenerated(*Volume->PCGComponent).Instances;
			TestTrue(*FString::Printf(TEXT("cubes on the grid (%d)"), Dense), Dense >= 64);
			TestTrue(TEXT("counted per mesh"), Json(Result).Contains(TEXT("Cube: ")));

			// pcg | call SetParameter("@G@", "Spacing", "(250, 250, 100)")      pcg | call Generate("@LABEL@", 7)
			TestTrue(TEXT("sparser"), Run(TEXT("SetParameter"), { Lit(G), Lit(TEXT("Spacing")), Lit(TEXT("(250, 250, 100)")) }));
			TestTrue(TEXT("regenerated with a seed"), Run(TEXT("Generate"), { Lit(Label), Lit(7.0) }, &Result));
			const int32 Sparse = UnrealAIPCG::CountGenerated(*Volume->PCGComponent).Instances;
			TestTrue(*FString::Printf(TEXT("the parameter drives the grid (%d < %d)"), Sparse, Dense), Sparse > 0 && Sparse < Dense);
			TestEqual(TEXT("the seed"), Volume->PCGComponent->Seed, 7);

			// actors | where Label == "@LABEL@" | get Components | where $ is PCGComponent | select Generated
			TSharedPtr<FJsonObject> Generated;
			TestTrue(TEXT("Generated member"), UnrealMcpRqlTools::RunPlan(FString(TEXT(R"RQL({"rql":"0.1","source":{"path":["actors"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Label"]},{"lit":"@LABEL@"}]}},{"op":"get","expr":{"path":["Components"]}},{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"PCGComponent"}]}},{"op":"select","fields":[{"name":"Generated","expr":{"path":["Generated"]}}]}]})RQL")).Replace(TEXT("@LABEL@"), *Label), false, &Generated));
			TestTrue(*FString::Printf(TEXT("reads the instances (%s)"), *Json(Generated).Left(400)), Json(Generated).Contains(FString::Printf(TEXT("\"Instances\":%d,"), Sparse)));

			// pcg | call Cleanup("@LABEL@")
			TestTrue(TEXT("cleaned up"), Run(TEXT("Cleanup"), { Lit(Label) }));
			TestEqual(TEXT("nothing generated"), UnrealAIPCG::CountGenerated(*Volume->PCGComponent).Instances, 0);
			DestroyLabelled(Label);
			TestTrue(TEXT("test asset deleted"), DeleteAsset(G));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
