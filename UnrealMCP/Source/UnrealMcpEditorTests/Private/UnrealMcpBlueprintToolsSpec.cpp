// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"

#include "Engine/Blueprint.h"
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_Event.h"

/**
 * Blueprint tool family specs (docs/ARCHITECTURE.md §10, issue #11). Two layers:
 *  - Registration/manifest assertions run without an editor world (pure registry).
 *  - The create -> edit -> compile -> spawn round-trip runs in EditorContext (GEditor is valid), the same
 *    code path the live bridge e2e drives, just invoked directly through the registry on the game thread.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpBlueprintToolsSpec, "UnrealMcp.Tools.Blueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Run a registered tool by name with the given JSON arguments.
	static FUnrealMcpToolResult Run(const FUnrealMcpToolRegistry& Registry, const FString& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		return Registry.Execute(Tool, FUnrealMcpToolCall(Args));
	}

	static TSharedPtr<FJsonObject> Args() { return MakeShared<FJsonObject>(); }

	// Create a uniquely-named Actor Blueprint and return its object path (package + '.' + short name).
	// Non-static so it can assert its own precondition: a discarded blueprint-create failure would
	// otherwise surface as a confusing "Blueprint not found" in whichever downstream tool call runs first.
	FString CreateBlueprint(const FUnrealMcpToolRegistry& Registry)
	{
		const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMcpTests/BP_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
		TSharedPtr<FJsonObject> Create = Args();
		Create->SetStringField(TEXT("path"), PackagePath);
		Create->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
		TestTrue(TEXT("CreateBlueprint helper: blueprint-create succeeded"), Run(Registry, TEXT("blueprint-create"), Create).bSuccess);
		return PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath);
	}

	/** The "nodes" entry of a blueprint-graph result with this id (null when absent). */
	static TSharedPtr<FJsonObject> FindNodeJson(const TSharedPtr<FJsonObject>& GraphOut, const FString& Id)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!GraphOut.IsValid() || !GraphOut->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
			return nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject> Node = Value->AsObject();
			if (Node.IsValid() && Node->GetStringField(TEXT("id")) == Id)
				return Node;
		}
		return nullptr;
	}

	/** The "pins" entry named @p PinName of a node entry of a blueprint-graph result (null when absent). */
	static TSharedPtr<FJsonObject> FindPinJson(const TSharedPtr<FJsonObject>& NodeJson, const FString& PinName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!NodeJson.IsValid() || !NodeJson->TryGetArrayField(TEXT("pins"), Pins) || !Pins)
			return nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Pins)
		{
			const TSharedPtr<FJsonObject> Pin = Value->AsObject();
			if (Pin.IsValid() && Pin->GetStringField(TEXT("name")) == PinName)
				return Pin;
		}
		return nullptr;
	}

END_DEFINE_SPEC(FUnrealMcpBlueprintToolsSpec)

void FUnrealMcpBlueprintToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers all fifteen blueprint tools and bumps the revision", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpBlueprintTools::Register(Registry);

			const TArray<FString> Expected = {
				TEXT("blueprint-create"), TEXT("blueprint-get"),
				TEXT("blueprint-add-component"), TEXT("blueprint-remove-component"),
				TEXT("blueprint-add-variable"), TEXT("blueprint-modify-variable"),
				TEXT("blueprint-set-default"),
				TEXT("blueprint-add-function"), TEXT("blueprint-add-event"),
				TEXT("blueprint-graph"), TEXT("blueprint-add-node"),
				TEXT("blueprint-connect-pins"), TEXT("blueprint-disconnect-pins"),
				TEXT("blueprint-compile"), TEXT("blueprint-spawn") };

			for (const FString& Name : Expected)
				TestTrue(*FString::Printf(TEXT("has %s"), *Name), Registry.HasTool(Name));
			TestEqual(TEXT("exactly 15 tools"), Registry.Num(), Expected.Num());
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);
		});

		It("emits well-formed manifest descriptors (every tool is CORE with a schema hash)", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);

			TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			TestTrue(TEXT("tools array"), Manifest->TryGetArrayField(TEXT("tools"), Tools));
			for (const TSharedPtr<FJsonValue>& Value : *Tools)
			{
				TSharedPtr<FJsonObject> Desc = Value->AsObject();
				TestEqual(TEXT("core extension id"), Desc->GetStringField(TEXT("extensionId")), FString(TEXT("core")));
				TestFalse(TEXT("schema hash present"), Desc->GetStringField(TEXT("schemaHash")).IsEmpty());
				TestTrue(TEXT("has input schema"), Desc->HasField(TEXT("inputSchema")));
			}
		});
	});

	Describe("error handling", [this]()
	{
		It("returns an error for an unknown blueprint path", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);

			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("path"), TEXT("/Game/DoesNotExist/BP_Nope"));
			const FUnrealMcpToolResult Result = Run(Registry, TEXT("blueprint-get"), A);
			TestFalse(TEXT("not success"), Result.bSuccess);
		});

		It("rejects an unresolvable variable type", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);

			const FString ObjectPath = CreateBlueprint(Registry);

			TSharedPtr<FJsonObject> AddVar = Args();
			AddVar->SetStringField(TEXT("path"), ObjectPath);
			AddVar->SetStringField(TEXT("name"), TEXT("Bogus"));
			AddVar->SetStringField(TEXT("type"), TEXT("/Game/Nope.NotAType"));
			TestFalse(TEXT("rejected unresolvable type"), Run(Registry, TEXT("blueprint-add-variable"), AddVar).bSuccess);
		});

		It("rejects an abstract component class instead of crashing", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			// LightComponentBase is abstract; SCS CreateNode -> NewObject would otherwise fatally assert.
			TSharedPtr<FJsonObject> AddComp = Args();
			AddComp->SetStringField(TEXT("path"), ObjectPath);
			AddComp->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.LightComponentBase"));
			AddComp->SetStringField(TEXT("name"), TEXT("BadLight"));
			TestFalse(TEXT("abstract component rejected"), Run(Registry, TEXT("blueprint-add-component"), AddComp).bSuccess);
		});

		It("rejects a non-overridable event function and removing a missing component", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			// K2_DestroyActor exists on AActor but is not a blueprint-overridable event.
			TSharedPtr<FJsonObject> BadEvent = Args();
			BadEvent->SetStringField(TEXT("path"), ObjectPath);
			BadEvent->SetStringField(TEXT("name"), TEXT("K2_DestroyActor"));
			TestFalse(TEXT("non-overridable event rejected"), Run(Registry, TEXT("blueprint-add-event"), BadEvent).bSuccess);

			TSharedPtr<FJsonObject> RemoveMissing = Args();
			RemoveMissing->SetStringField(TEXT("path"), ObjectPath);
			RemoveMissing->SetStringField(TEXT("name"), TEXT("NoSuchComponent"));
			TestFalse(TEXT("removing a missing component rejected"), Run(Registry, TEXT("blueprint-remove-component"), RemoveMissing).bSuccess);
		});

		It("rejects set-default on a missing property", [this]()
		{
			if (!GEditor)
			{
				AddWarning(TEXT("GEditor unavailable; skipping set-default negative test (needs a compiled CDO)."));
				return;
			}
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			TSharedPtr<FJsonObject> Compile = Args();
			Compile->SetStringField(TEXT("path"), ObjectPath);
			TestTrue(TEXT("compiled"), Run(Registry, TEXT("blueprint-compile"), Compile).bSuccess);

			TSharedPtr<FJsonObject> SetMissing = Args();
			SetMissing->SetStringField(TEXT("path"), ObjectPath);
			SetMissing->SetStringField(TEXT("property"), TEXT("NoSuchProperty"));
			SetMissing->SetStringField(TEXT("value"), TEXT("1"));
			TestFalse(TEXT("missing property rejected"), Run(Registry, TEXT("blueprint-set-default"), SetMissing).bSuccess);
		});
	});

	Describe("create -> edit -> compile -> spawn round-trip", [this]()
	{
		It("runs the full MVP loop and reflects edits in blueprint-get", [this]()
		{
			if (!GEditor)
			{
				AddWarning(TEXT("GEditor unavailable; skipping the live editor round-trip."));
				return;
			}

			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);

			const FString PackagePath = FString::Printf(TEXT("/Game/UnrealMcpTests/BP_Spec_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
			const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath);

			// create
			TSharedPtr<FJsonObject> Create = Args();
			Create->SetStringField(TEXT("path"), PackagePath);
			Create->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
			TestTrue(TEXT("blueprint-create"), Run(Registry, TEXT("blueprint-create"), Create).bSuccess);

			// add-variable (float)
			TSharedPtr<FJsonObject> AddVar = Args();
			AddVar->SetStringField(TEXT("path"), ObjectPath);
			AddVar->SetStringField(TEXT("name"), TEXT("Health"));
			AddVar->SetStringField(TEXT("type"), TEXT("float"));
			AddVar->SetStringField(TEXT("defaultValue"), TEXT("100.0"));
			TestTrue(TEXT("blueprint-add-variable"), Run(Registry, TEXT("blueprint-add-variable"), AddVar).bSuccess);

			// add-component (StaticMeshComponent)
			TSharedPtr<FJsonObject> AddComp = Args();
			AddComp->SetStringField(TEXT("path"), ObjectPath);
			AddComp->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.StaticMeshComponent"));
			AddComp->SetStringField(TEXT("name"), TEXT("Mesh"));
			TestTrue(TEXT("blueprint-add-component"), Run(Registry, TEXT("blueprint-add-component"), AddComp).bSuccess);

			// add-event (ReceiveBeginPlay)
			TSharedPtr<FJsonObject> AddEvent = Args();
			AddEvent->SetStringField(TEXT("path"), ObjectPath);
			AddEvent->SetStringField(TEXT("name"), TEXT("ReceiveBeginPlay"));
			TestTrue(TEXT("blueprint-add-event"), Run(Registry, TEXT("blueprint-add-event"), AddEvent).bSuccess);

			// add-function
			TSharedPtr<FJsonObject> AddFunc = Args();
			AddFunc->SetStringField(TEXT("path"), ObjectPath);
			AddFunc->SetStringField(TEXT("name"), TEXT("DoThing"));
			TestTrue(TEXT("blueprint-add-function"), Run(Registry, TEXT("blueprint-add-function"), AddFunc).bSuccess);

			// compile — structured contract + clean success
			TSharedPtr<FJsonObject> Compile = Args();
			Compile->SetStringField(TEXT("path"), ObjectPath);
			const FUnrealMcpToolResult CompileResult = Run(Registry, TEXT("blueprint-compile"), Compile);
			TestTrue(TEXT("blueprint-compile succeeded"), CompileResult.bSuccess);
			TestTrue(TEXT("compile structured present"), CompileResult.Structured.IsValid());
			if (CompileResult.Structured.IsValid())
			{
				bool bSucceeded = false;
				TestTrue(TEXT("has succeeded field"), CompileResult.Structured->TryGetBoolField(TEXT("succeeded"), bSucceeded));
				TestTrue(TEXT("compile reports succeeded"), bSucceeded);
				TestEqual(TEXT("zero errors"), (int32)CompileResult.Structured->GetNumberField(TEXT("numErrors")), 0);
				const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
				TestTrue(TEXT("messages array present"), CompileResult.Structured->TryGetArrayField(TEXT("messages"), Messages));
			}

			// set-default on the new variable (CDO write)
			TSharedPtr<FJsonObject> SetDefault = Args();
			SetDefault->SetStringField(TEXT("path"), ObjectPath);
			SetDefault->SetStringField(TEXT("property"), TEXT("Health"));
			SetDefault->SetStringField(TEXT("value"), TEXT("250.0"));
			TestTrue(TEXT("blueprint-set-default"), Run(Registry, TEXT("blueprint-set-default"), SetDefault).bSuccess);

			// spawn
			TSharedPtr<FJsonObject> Spawn = Args();
			Spawn->SetStringField(TEXT("path"), ObjectPath);
			Spawn->SetStringField(TEXT("name"), TEXT("McpSpecActor"));
			TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
			Loc->SetNumberField(TEXT("x"), 10.0); Loc->SetNumberField(TEXT("y"), 20.0); Loc->SetNumberField(TEXT("z"), 30.0);
			Spawn->SetObjectField(TEXT("location"), Loc);
			TestTrue(TEXT("blueprint-spawn"), Run(Registry, TEXT("blueprint-spawn"), Spawn).bSuccess);

			// get — assert edits are reflected
			TSharedPtr<FJsonObject> Get = Args();
			Get->SetStringField(TEXT("path"), ObjectPath);
			const FUnrealMcpToolResult GetResult = Run(Registry, TEXT("blueprint-get"), Get);
			TestTrue(TEXT("blueprint-get"), GetResult.bSuccess);
			if (GetResult.Structured.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Vars = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Comps = nullptr;
				TestTrue(TEXT("variables present"), GetResult.Structured->TryGetArrayField(TEXT("variables"), Vars));
				TestTrue(TEXT("components present"), GetResult.Structured->TryGetArrayField(TEXT("components"), Comps));
				TestTrue(TEXT("at least one variable"), Vars && Vars->Num() >= 1);
				TestTrue(TEXT("at least one component"), Comps && Comps->Num() >= 1);
			}
			// The client's text block carries the summary data, not only the counts (clients that read only `content`).
			const FString GetText = GetResult.WireText();
			TestTrue(*FString::Printf(TEXT("text names the variable and the component (%s)"), *GetText.Left(300)),
				GetText.Contains(TEXT("Health")) && GetText.Contains(TEXT("Mesh")) && GetText.Contains(TEXT("DoThing")));

			// remove-component closes the SCS edit pair
			TSharedPtr<FJsonObject> RemoveComp = Args();
			RemoveComp->SetStringField(TEXT("path"), ObjectPath);
			RemoveComp->SetStringField(TEXT("name"), TEXT("Mesh"));
			TestTrue(TEXT("blueprint-remove-component"), Run(Registry, TEXT("blueprint-remove-component"), RemoveComp).bSuccess);
		});
	});

	Describe("structure edits", [this]()
	{
		It("modify-variable renames and retypes, reflected in blueprint-get", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			TSharedPtr<FJsonObject> AddVar = Args();
			AddVar->SetStringField(TEXT("path"), ObjectPath);
			AddVar->SetStringField(TEXT("name"), TEXT("Speed"));
			AddVar->SetStringField(TEXT("type"), TEXT("int"));
			TestTrue(TEXT("added Speed:int"), Run(Registry, TEXT("blueprint-add-variable"), AddVar).bSuccess);

			TSharedPtr<FJsonObject> Modify = Args();
			Modify->SetStringField(TEXT("path"), ObjectPath);
			Modify->SetStringField(TEXT("name"), TEXT("Speed"));
			Modify->SetStringField(TEXT("newName"), TEXT("Velocity"));
			Modify->SetStringField(TEXT("newType"), TEXT("float"));
			TestTrue(TEXT("renamed+retyped"), Run(Registry, TEXT("blueprint-modify-variable"), Modify).bSuccess);

			TSharedPtr<FJsonObject> Get = Args();
			Get->SetStringField(TEXT("path"), ObjectPath);
			const FUnrealMcpToolResult GetResult = Run(Registry, TEXT("blueprint-get"), Get);
			TestTrue(TEXT("get ok"), GetResult.bSuccess);
			bool bFoundVelocity = false;
			if (GetResult.Structured.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Vars = nullptr;
				if (GetResult.Structured->TryGetArrayField(TEXT("variables"), Vars))
				{
					for (const TSharedPtr<FJsonValue>& V : *Vars)
					{
						const TSharedPtr<FJsonObject> Obj = V->AsObject();
						if (Obj && Obj->GetStringField(TEXT("name")) == TEXT("Velocity"))
						{
							bFoundVelocity = true;
							TestEqual(TEXT("retyped to float"), Obj->GetStringField(TEXT("type")), FString(TEXT("float")));
						}
					}
				}
			}
			TestTrue(TEXT("renamed variable present"), bFoundVelocity);
		});

		It("rejects renaming a variable onto an existing name", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			for (const TCHAR* Name : { TEXT("Alpha"), TEXT("Beta") })
			{
				TSharedPtr<FJsonObject> AddVar = Args();
				AddVar->SetStringField(TEXT("path"), ObjectPath);
				AddVar->SetStringField(TEXT("name"), Name);
				AddVar->SetStringField(TEXT("type"), TEXT("int"));
				TestTrue(TEXT("added"), Run(Registry, TEXT("blueprint-add-variable"), AddVar).bSuccess);
			}

			TSharedPtr<FJsonObject> Collide = Args();
			Collide->SetStringField(TEXT("path"), ObjectPath);
			Collide->SetStringField(TEXT("name"), TEXT("Alpha"));
			Collide->SetStringField(TEXT("newName"), TEXT("Beta"));
			TestFalse(TEXT("rename onto existing name rejected"), Run(Registry, TEXT("blueprint-modify-variable"), Collide).bSuccess);
		});

		It("add-event yields an ENABLED node and rejects a duplicate", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			TSharedPtr<FJsonObject> AddEvent = Args();
			AddEvent->SetStringField(TEXT("path"), ObjectPath);
			AddEvent->SetStringField(TEXT("name"), TEXT("ReceiveBeginPlay"));
			TestTrue(TEXT("event added"), Run(Registry, TEXT("blueprint-add-event"), AddEvent).bSuccess);

			// The node must be ENABLED (not the disabled ghost AddDefaultEventNode seeds by default).
			UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *ObjectPath);
			TestNotNull(TEXT("blueprint resolved"), Blueprint);
			bool bFoundEnabledEvent = false;
			if (Blueprint)
			{
				if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
				{
					for (const UEdGraphNode* Node : EventGraph->Nodes)
					{
						const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
						if (EventNode && EventNode->EventReference.GetMemberName() == FName(TEXT("ReceiveBeginPlay")))
							bFoundEnabledEvent = EventNode->IsNodeEnabled();
					}
				}
			}
			TestTrue(TEXT("event node is enabled"), bFoundEnabledEvent);

			// A second add for the same event must be rejected (no duplicate ghost node).
			TestFalse(TEXT("duplicate event rejected"), Run(Registry, TEXT("blueprint-add-event"), AddEvent).bSuccess);
		});

		It("sets Instance Editable on add-variable and toggles it with modify-variable", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);
			UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *ObjectPath);
			if (!TestNotNull(TEXT("blueprint resolved"), Blueprint))
				return;
			auto IsInstanceEditable = [Blueprint](const TCHAR* Name)
			{
				const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
				return Index != INDEX_NONE && !(Blueprint->NewVariables[Index].PropertyFlags & CPF_DisableEditOnInstance);
			};

			TSharedPtr<FJsonObject> Editable = Args();
			Editable->SetStringField(TEXT("path"), ObjectPath);
			Editable->SetStringField(TEXT("name"), TEXT("Bonus"));
			Editable->SetStringField(TEXT("type"), TEXT("int"));
			Editable->SetBoolField(TEXT("instanceEditable"), true);
			TestTrue(TEXT("added instance editable"), Run(Registry, TEXT("blueprint-add-variable"), Editable).bSuccess);
			TestTrue(TEXT("Bonus is instance editable"), IsInstanceEditable(TEXT("Bonus")));

			TSharedPtr<FJsonObject> Plain = Args();
			Plain->SetStringField(TEXT("path"), ObjectPath);
			Plain->SetStringField(TEXT("name"), TEXT("Points"));
			Plain->SetStringField(TEXT("type"), TEXT("int"));
			TestTrue(TEXT("added"), Run(Registry, TEXT("blueprint-add-variable"), Plain).bSuccess);
			TestFalse(TEXT("default: not instance editable (as in the editor)"), IsInstanceEditable(TEXT("Points")));

			TSharedPtr<FJsonObject> Toggle = Args();
			Toggle->SetStringField(TEXT("path"), ObjectPath);
			Toggle->SetStringField(TEXT("name"), TEXT("Points"));
			Toggle->SetBoolField(TEXT("instanceEditable"), true);
			TestTrue(TEXT("modify with only instanceEditable"), Run(Registry, TEXT("blueprint-modify-variable"), Toggle).bSuccess);
			TestTrue(TEXT("Points is instance editable now"), IsInstanceEditable(TEXT("Points")));
			Toggle->SetBoolField(TEXT("instanceEditable"), false);
			TestTrue(TEXT("and back"), Run(Registry, TEXT("blueprint-modify-variable"), Toggle).bSuccess);
			TestFalse(TEXT("Points is class-default only again"), IsInstanceEditable(TEXT("Points")));
		});
	});

	Describe("graph authoring", [this]()
	{
		It("adds nodes, wires the event into a call and reports the link", [this]()
		{
			if (!GEditor)
			{
				AddWarning(TEXT("GEditor unavailable; skipping the graph-authoring round-trip."));
				return;
			}
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			// The event node (kind 'event' enables the placeholder a fresh Blueprint is seeded with).
			TSharedPtr<FJsonObject> AddEvent = Args();
			AddEvent->SetStringField(TEXT("path"), ObjectPath);
			AddEvent->SetStringField(TEXT("kind"), TEXT("event"));
			AddEvent->SetStringField(TEXT("name"), TEXT("BeginPlay"));
			const FUnrealMcpToolResult EventResult = Run(Registry, TEXT("blueprint-add-node"), AddEvent);
			TestTrue(*FString::Printf(TEXT("event node added (%s)"), *EventResult.Message), EventResult.bSuccess);
			FString EventId;
			if (EventResult.Structured.IsValid())
			{
				const TSharedPtr<FJsonObject> Node = EventResult.Structured->GetObjectField(TEXT("node"));
				if (Node.IsValid())
					EventId = Node->GetStringField(TEXT("id"));
			}
			TestFalse(TEXT("event node id returned"), EventId.IsEmpty());

			// A function call node, resolved through KismetSystemLibrary.
			TSharedPtr<FJsonObject> AddCall = Args();
			AddCall->SetStringField(TEXT("path"), ObjectPath);
			AddCall->SetStringField(TEXT("kind"), TEXT("call"));
			AddCall->SetStringField(TEXT("function"), TEXT("PrintString"));
			const FUnrealMcpToolResult CallResult = Run(Registry, TEXT("blueprint-add-node"), AddCall);
			TestTrue(*FString::Printf(TEXT("call node added (%s)"), *CallResult.Message), CallResult.bSuccess);
			FString CallId;
			if (CallResult.Structured.IsValid())
			{
				const TSharedPtr<FJsonObject> Node = CallResult.Structured->GetObjectField(TEXT("node"));
				if (Node.IsValid())
					CallId = Node->GetStringField(TEXT("id"));
			}
			TestFalse(TEXT("call node id returned"), CallId.IsEmpty());

			// Wire the event's 'then' into the call's 'execute'.
			TSharedPtr<FJsonObject> Connect = Args();
			Connect->SetStringField(TEXT("path"), ObjectPath);
			Connect->SetStringField(TEXT("fromNode"), EventId);
			Connect->SetStringField(TEXT("fromPin"), TEXT("then"));
			Connect->SetStringField(TEXT("toNode"), CallId);
			Connect->SetStringField(TEXT("toPin"), TEXT("execute"));
			const FUnrealMcpToolResult ConnectResult = Run(Registry, TEXT("blueprint-connect-pins"), Connect);
			TestTrue(*FString::Printf(TEXT("pins connected (%s)"), *ConnectResult.Message), ConnectResult.bSuccess);

			// blueprint-graph reports the link the connect made.
			TSharedPtr<FJsonObject> GraphArgs = Args();
			GraphArgs->SetStringField(TEXT("path"), ObjectPath);
			const FUnrealMcpToolResult GraphResult = Run(Registry, TEXT("blueprint-graph"), GraphArgs);
			TestTrue(TEXT("blueprint-graph"), GraphResult.bSuccess);
			if (GraphResult.Structured.IsValid())
			{
				const TSharedPtr<FJsonObject> CallNode = FindNodeJson(GraphResult.Structured, CallId);
				if (TestNotNull(TEXT("call node in the graph read"), CallNode.Get()))
				{
					const TSharedPtr<FJsonObject> ExecPin = FindPinJson(CallNode, TEXT("execute"));
					if (TestNotNull(TEXT("execute pin reported"), ExecPin.Get()))
					{
						const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
						TestTrue(TEXT("execute pin carries the link"), ExecPin->TryGetArrayField(TEXT("links"), Links) && Links && Links->Num() == 1);
					}
				}
			}

			// The wired graph compiles clean — a wire the compiler rejects would surface here.
			TSharedPtr<FJsonObject> Compile = Args();
			Compile->SetStringField(TEXT("path"), ObjectPath);
			TestTrue(TEXT("the wired graph compiles"), Run(Registry, TEXT("blueprint-compile"), Compile).bSuccess);

			// Disconnect by node + pin, then confirm the link is gone.
			TSharedPtr<FJsonObject> Disconnect = Args();
			Disconnect->SetStringField(TEXT("path"), ObjectPath);
			Disconnect->SetStringField(TEXT("node"), EventId);
			Disconnect->SetStringField(TEXT("pin"), TEXT("then"));
			TestTrue(TEXT("disconnected"), Run(Registry, TEXT("blueprint-disconnect-pins"), Disconnect).bSuccess);
			const FUnrealMcpToolResult AfterGraph = Run(Registry, TEXT("blueprint-graph"), GraphArgs);
			if (AfterGraph.Structured.IsValid())
			{
				const TSharedPtr<FJsonObject> CallNode = FindNodeJson(AfterGraph.Structured, CallId);
				const TSharedPtr<FJsonObject> ExecPin = FindPinJson(CallNode, TEXT("execute"));
				TestFalse(TEXT("execute pin is unlinked again"), ExecPin.IsValid() && ExecPin->HasField(TEXT("links")));
			}
		});

		It("refuses an ambiguous node title instead of guessing", [this]()
		{
			if (!GEditor)
			{
				AddWarning(TEXT("GEditor unavailable; skipping the ambiguity test."));
				return;
			}
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			// Two Print String nodes: their shared title can no longer identify one of them. The title is read
			// back from the tool (the editor draws it in the editor's own language, so it must never be
			// hard-coded as "Print String" here).
			FString SharedTitle;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				TSharedPtr<FJsonObject> AddCall = Args();
				AddCall->SetStringField(TEXT("path"), ObjectPath);
				AddCall->SetStringField(TEXT("kind"), TEXT("call"));
				AddCall->SetStringField(TEXT("function"), TEXT("PrintString"));
				AddCall->SetNumberField(TEXT("x"), 300 + Index * 250);
				AddCall->SetNumberField(TEXT("y"), 0);
				const FUnrealMcpToolResult Added = Run(Registry, TEXT("blueprint-add-node"), AddCall);
				TestTrue(TEXT("PrintString added"), Added.bSuccess);
				if (Added.Structured.IsValid() && SharedTitle.IsEmpty())
				{
					const TSharedPtr<FJsonObject> Node = Added.Structured->GetObjectField(TEXT("node"));
					if (Node.IsValid())
						SharedTitle = Node->GetStringField(TEXT("title"));
				}
			}
			TestFalse(TEXT("the node reports a title to address it by"), SharedTitle.IsEmpty());

			TSharedPtr<FJsonObject> Connect = Args();
			Connect->SetStringField(TEXT("path"), ObjectPath);
			Connect->SetStringField(TEXT("fromNode"), SharedTitle);
			Connect->SetStringField(TEXT("fromPin"), TEXT("then"));
			Connect->SetStringField(TEXT("toNode"), SharedTitle);
			Connect->SetStringField(TEXT("toPin"), TEXT("execute"));
			const FUnrealMcpToolResult Result = Run(Registry, TEXT("blueprint-connect-pins"), Connect);
			TestFalse(TEXT("ambiguous title refused"), Result.bSuccess);
			TestTrue(*FString::Printf(TEXT("the error names the candidates (%s)"), *Result.Message), Result.Message.Contains(TEXT("matches")));
		});

		It("rejects an unknown function and an unknown pin", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpBlueprintTools::Register(Registry);
			const FString ObjectPath = CreateBlueprint(Registry);

			TSharedPtr<FJsonObject> BadFunction = Args();
			BadFunction->SetStringField(TEXT("path"), ObjectPath);
			BadFunction->SetStringField(TEXT("kind"), TEXT("call"));
			BadFunction->SetStringField(TEXT("function"), TEXT("NoSuchRqlFunction"));
			const FUnrealMcpToolResult FunctionResult = Run(Registry, TEXT("blueprint-add-node"), BadFunction);
			TestFalse(TEXT("unknown function rejected"), FunctionResult.bSuccess);
			TestTrue(*FString::Printf(TEXT("the error says what it looked for (%s)"), *FunctionResult.Message),
				FunctionResult.Message.Contains(TEXT("Blueprint-callable function")));

			TSharedPtr<FJsonObject> AddCall = Args();
			AddCall->SetStringField(TEXT("path"), ObjectPath);
			AddCall->SetStringField(TEXT("kind"), TEXT("call"));
			AddCall->SetStringField(TEXT("function"), TEXT("PrintString"));
			const FUnrealMcpToolResult Added = Run(Registry, TEXT("blueprint-add-node"), AddCall);
			TestTrue(TEXT("PrintString added"), Added.bSuccess);
			// The drawn title is the editor's own language: read it back instead of assuming "Print String".
			FString CallTitle;
			if (Added.Structured.IsValid())
			{
				const TSharedPtr<FJsonObject> Node = Added.Structured->GetObjectField(TEXT("node"));
				if (Node.IsValid())
					CallTitle = Node->GetStringField(TEXT("title"));
			}
			TestFalse(TEXT("the node reports a title to address it by"), CallTitle.IsEmpty());

			TSharedPtr<FJsonObject> BadPin = Args();
			BadPin->SetStringField(TEXT("path"), ObjectPath);
			BadPin->SetStringField(TEXT("fromNode"), CallTitle);
			BadPin->SetStringField(TEXT("fromPin"), TEXT("NoSuchPin"));
			BadPin->SetStringField(TEXT("toNode"), CallTitle);
			BadPin->SetStringField(TEXT("toPin"), TEXT("NoSuchPin"));
			const FUnrealMcpToolResult PinResult = Run(Registry, TEXT("blueprint-connect-pins"), BadPin);
			TestFalse(TEXT("unknown pin rejected"), PinResult.bSuccess);
			TestTrue(*FString::Printf(TEXT("the error lists the pins (%s)"), *PinResult.Message), PinResult.Message.Contains(TEXT("has no pin")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
