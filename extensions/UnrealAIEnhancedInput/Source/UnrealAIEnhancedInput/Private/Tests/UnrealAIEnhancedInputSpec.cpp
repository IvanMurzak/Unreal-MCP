// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

// ============================================================================================
//  UE Automation spec for the extension's RQL module (Unreal-MCP docs/rql.md "Extensions").
//
//  The spec discovers THIS extension's live module through IModularFeatures (the path Unreal-MCP uses) and runs RQL
//  QueryPlans through the editor's RQL engine (UnrealMcpRqlTools::RunPlan). The C++ runtime has no text parser, so
//  each plan is the one Rql.Parser produces for the query in the comment above it.
//
//  Run via:  Automation RunTests UnrealAIEnhancedInput
// ============================================================================================

BEGIN_DEFINE_SPEC(FUnrealAIEnhancedInputSpec, "UnrealAIEnhancedInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	FString BlueprintPath;
	FString ActionPath;
	FString ContextPath;

	/** Run a plan with @BP@ / @IA@ / @IMC@ (and @Values@) filled in; the QueryResult. */
	TSharedPtr<FJsonObject> Run(const FString& Plan, bool bMutate, std::initializer_list<TPair<const TCHAR*, FString>> Values = {})
	{
		FString Filled = Plan.Replace(TEXT("@BP@"), *BlueprintPath).Replace(TEXT("@IA@"), *ActionPath).Replace(TEXT("@IMC@"), *ContextPath);
		for (const TPair<const TCHAR*, FString>& Value : Values)
			Filled = Filled.Replace(*FString::Printf(TEXT("@%s@"), Value.Key), *Value.Value);
		TSharedPtr<FJsonObject> Result;
		UnrealMcpRqlTools::RunPlan(Filled, bMutate, &Result);
		return Result;
	}

	static bool IsOk(const TSharedPtr<FJsonObject>& Result)
	{
		return Result.IsValid() && Result->GetBoolField(TEXT("ok"));
	}

	static FString Codes(const TSharedPtr<FJsonObject>& Result)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		return Text.Left(600);
	}

	/** The $ref in the result column of a receipt's first row (the object a call returned). */
	static FString ResultRef(const TSharedPtr<FJsonObject>& Receipt)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Receipt.IsValid() || !Receipt->TryGetArrayField(TEXT("rows"), Rows) || Rows->Num() == 0)
			return FString();
		const TArray<TSharedPtr<FJsonValue>>& Cells = (*Rows)[0]->AsArray();
		FString Ref;
		if (Cells.IsValidIndex(2) && Cells[2]->Type == EJson::Object)
			Cells[2]->AsObject()->TryGetStringField(TEXT("$ref"), Ref);
		return Ref;
	}

	/** A new Blueprint of @p Parent at a fresh path (BlueprintPath), with a fresh action and context (ActionPath, ContextPath). */
	UBlueprint* MakeBlueprint(UClass* Parent)
	{
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		BlueprintPath = FString::Printf(TEXT("/Game/RqlSpec/BP_Input_%s"), *Id);
		ActionPath = FString::Printf(TEXT("/Game/RqlSpec/IA_Jump_%s"), *Id);
		ContextPath = FString::Printf(TEXT("/Game/RqlSpec/IMC_Default_%s"), *Id);
		// enhancedInput | call CreateAction("@IA@", "Boolean")
		TestTrue(TEXT("action created"), IsOk(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateAction","args":[{"lit":"@IA@"},{"lit":"Boolean"}]}]})RQL"), true)));
		// enhancedInput | call CreateContext("@IMC@")
		TestTrue(TEXT("context created"), IsOk(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateContext","args":[{"lit":"@IMC@"}]}]})RQL"), true)));
		UPackage* Package = CreatePackage(*BlueprintPath);
		return FKismetEditorUtilities::CreateBlueprint(Parent, Package, *FPackageName::GetShortName(BlueprintPath), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	int32 CompileErrors(UBlueprint* Blueprint)
	{
		FCompilerResultsLog Log;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
		{
			if (Message->GetSeverity() == EMessageSeverity::Error)
				AddInfo(Message->ToText().ToString());
		}
		return Log.NumErrors;
	}

END_DEFINE_SPEC(FUnrealAIEnhancedInputSpec)

void FUnrealAIEnhancedInputSpec::Define()
{
	Describe("RQL module", [this]()
	{
		It("registers this extension's RQL module as a modular feature", [this]()
		{
			IUnrealMcpRqlModule* Module = UnrealMcpRqlTools::FindModule(TEXT("com.ivanmurzak.unreal-ai-enhanced-input"));
			if (TestNotNull(TEXT("RQL module is registered"), Module))
			{
				TestEqual(TEXT("module name"), Module->GetRqlModuleName(), FString(TEXT("EnhancedInput")));
				TestFalse(TEXT("version reported"), Module->GetExtensionVersion().IsEmpty());
			}
		});

		It("adds its sources and guide to the unreal-rql skill", [this]()
		{
			const FString Skill = UnrealMcpRqlTools::SkillMarkdown();
			TestTrue(TEXT("| `inputActions` |"), Skill.Contains(TEXT("| `inputActions` |")));
			TestTrue(TEXT("| `inputContexts` |"), Skill.Contains(TEXT("| `inputContexts` |")));
			TestTrue(TEXT("| `enhancedInput` |"), Skill.Contains(TEXT("| `enhancedInput` |")));
			TestTrue(TEXT("## Enhanced Input"), Skill.Contains(TEXT("## Enhanced Input")));
		});
	});

	Describe("sources", [this]()
	{
		It("inputActions answers a query", [this]()
		{
			// inputActions | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["inputActions"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("inputContexts answers a query", [this]()
		{
			// inputContexts | count
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["inputContexts"]},"stages":[{"op":"count"}]})RQL"), false, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestTrue(TEXT("succeeds"), bResultOk);
		});

		It("describes InputMappingContext with its KeyMappings member", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("InputMappingContext"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists name:KeyMappings"), Schema.Contains(TEXT("\"name\":\"KeyMappings\"")));
		});
	});

	Describe("operations", [this]()
	{
		It("lists the authoring operations", [this]()
		{
			bool bOk = false;
			const FString Schema = UnrealMcpRqlTools::Schema(TEXT("EnhancedInputOperations"), bOk);
			TestTrue(TEXT("schema ok"), bOk);
			TestTrue(TEXT("lists CreateAction("), Schema.Contains(TEXT("CreateAction(")));
			TestTrue(TEXT("lists CreateContext("), Schema.Contains(TEXT("CreateContext(")));
			TestTrue(TEXT("lists AddMapping("), Schema.Contains(TEXT("AddMapping(")));
			TestTrue(TEXT("lists RemoveMapping("), Schema.Contains(TEXT("RemoveMapping(")));
			TestTrue(TEXT("lists AddActionEvent("), Schema.Contains(TEXT("AddActionEvent(")));
			TestTrue(TEXT("lists AddMappingContextOnBeginPlay("), Schema.Contains(TEXT("AddMappingContextOnBeginPlay(")));
		});

		It("creates an action and a context, maps a key and refuses an unknown key", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString NewActionPath = FString::Printf(TEXT("/Game/RqlSpec/IA_%s"), *Id);
			const FString NewContextPath = FString::Printf(TEXT("/Game/RqlSpec/IMC_%s"), *Id);
			// enhancedInput | call CreateAction("<action>", "Axis2D")
			TestTrue(TEXT("action created"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateAction","args":[{"lit":"%s"},{"lit":"Axis2D"}]}]})RQL"), *NewActionPath), true));
			// unreal.asset("<action>") | select ValueType
			TSharedPtr<FJsonObject> Action;
			const bool bActionOk = UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},"stages":[{"op":"select","fields":[{"name":"ValueType","expr":{"path":["ValueType"]}}]}]})RQL"), *NewActionPath), false, &Action);
			TestTrue(TEXT("action read"), bActionOk);
			FString ValueType;
			if (Action.IsValid() && Action->HasField(TEXT("rows")) && Action->GetArrayField(TEXT("rows")).Num() == 1)
				ValueType = Action->GetArrayField(TEXT("rows"))[0]->AsArray()[0]->AsString();
			TestEqual(TEXT("value type written"), ValueType, FString(TEXT("Axis2D")));
			// enhancedInput | call CreateContext("<context>")
			TestTrue(TEXT("context created"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateContext","args":[{"lit":"%s"}]}]})RQL"), *NewContextPath), true));
			// enhancedInput | call AddMapping("<context>", "<action>", "SpaceBar")
			TestTrue(TEXT("key mapped"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddMapping","args":[{"lit":"%s"},{"lit":"%s"},{"lit":"SpaceBar"}]}]})RQL"), *NewContextPath, *NewActionPath), true));
			// unreal.asset("<context>") | select KeyMappings
			TSharedPtr<FJsonObject> Mappings;
			const bool bMappingsOk = UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},"stages":[{"op":"select","fields":[{"name":"KeyMappings","expr":{"path":["KeyMappings"]}}]}]})RQL"), *NewContextPath), false, &Mappings);
			TestTrue(TEXT("context read"), bMappingsOk);
			FString Text;
			if (Mappings.IsValid())
				FJsonSerializer::Serialize(Mappings.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
			TestTrue(TEXT("the mapping is listed"), Text.Contains(TEXT("SpaceBar")));
			// unreal.asset("<context>") | select Mappings — the live mappings on every version (since 5.7 the reflected array is deprecated and empty)
			TSharedPtr<FJsonObject> Reflected;
			TestTrue(TEXT("Mappings read"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"%s"}]},"stages":[{"op":"select","fields":[{"name":"Mappings","expr":{"path":["Mappings"]}}]}]})RQL"), *NewContextPath), false, &Reflected));
			FString ReflectedText;
			if (Reflected.IsValid())
				FJsonSerializer::Serialize(Reflected.ToSharedRef(), TJsonWriterFactory<>::Create(&ReflectedText));
			TestTrue(*FString::Printf(TEXT("Mappings lists the mapping too (%s)"), *ReflectedText.Left(300)), ReflectedText.Contains(TEXT("SpaceBar")));
			// enhancedInput | call AddMapping("<context>", "<action>", "NoSuchKey")
			TestFalse(TEXT("unknown key refused"), UnrealMcpRqlTools::RunPlan(FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddMapping","args":[{"lit":"%s"},{"lit":"%s"},{"lit":"NoSuchKeyRqlSpec"}]}]})RQL"), *NewContextPath, *NewActionPath), true));
		});

		It("adds an action event to a Character Blueprint that wires to Jump and compiles; one event per action", [this]()
		{
			UBlueprint* Blueprint = MakeBlueprint(ACharacter::StaticClass());
			if (!TestNotNull(TEXT("character blueprint"), Blueprint))
				return;
			// enhancedInput | call AddActionEvent("@BP@", "@IA@")
			const TCHAR* AddEvent = TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddActionEvent","args":[{"lit":"@BP@"},{"lit":"@IA@"}]}]})RQL");
			const TSharedPtr<FJsonObject> Added = Run(AddEvent, true);
			TestTrue(*FString::Printf(TEXT("added (%s)"), *Codes(Added)), IsOk(Added));
			const FString Event = ResultRef(Added);
			TestTrue(*FString::Printf(TEXT("returns the node (%s)"), *Event), Event.StartsWith(TEXT("K2Node_EnhancedInputAction#")));
			TArray<UK2Node_EnhancedInputAction*> Nodes;
			FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Nodes);
			if (!TestEqual(TEXT("one action event"), Nodes.Num(), 1))
				return;
			for (const TCHAR* Pin : { TEXT("Triggered"), TEXT("Started"), TEXT("Completed"), TEXT("ActionValue") })
				TestNotNull(*FString::Printf(TEXT("pin %s"), Pin), Nodes[0]->FindPin(Pin));

			// unreal.asset("@BP@").EventGraph | call AddCallFunction("Jump")
			const FString Jump = ResultRef(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"Jump"}]}]})RQL"), true));
			// ref("@N@") | call Connect("Triggered", ref("@J@"), "execute")
			const TSharedPtr<FJsonObject> Wired = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@N@"}]},"stages":[{"op":"call","method":"Connect","args":[{"lit":"Triggered"},{"call":"ref","args":[{"lit":"@J@"}]},{"lit":"execute"}]}]})RQL"), true,
				{ { TEXT("N"), Event }, { TEXT("J"), Jump } });
			TestTrue(*FString::Printf(TEXT("Triggered wired to Jump (%s)"), *Codes(Wired)), IsOk(Wired));
			TestEqual(TEXT("compiles without errors"), CompileErrors(Blueprint), 0);

			const TSharedPtr<FJsonObject> Again = Run(AddEvent, true);
			TestTrue(*FString::Printf(TEXT("a second event for the action is refused (%s)"), *Codes(Again)), !IsOk(Again) && Codes(Again).Contains(TEXT("already has an action event")));
		});

		It("adds the mapping context at BeginPlay, ahead of what BeginPlay ran, once per context", [this]()
		{
			UBlueprint* Blueprint = MakeBlueprint(ACharacter::StaticClass());
			if (!TestNotNull(TEXT("character blueprint"), Blueprint))
				return;
			// unreal.asset("@BP@").EventGraph | call AddEvent("BeginPlay")
			const FString Begin = ResultRef(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddEvent","args":[{"lit":"BeginPlay"}]}]})RQL"), true));
			// unreal.asset("@BP@").EventGraph | call AddCallFunction("PrintString")
			const FString Print = ResultRef(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddCallFunction","args":[{"lit":"PrintString"}]}]})RQL"), true));
			// ref("@E@") | call Connect("then", ref("@P@"), "execute")
			TestTrue(TEXT("BeginPlay runs PrintString"), IsOk(Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"ref","args":[{"lit":"@E@"}]},"stages":[{"op":"call","method":"Connect","args":[{"lit":"then"},{"call":"ref","args":[{"lit":"@P@"}]},{"lit":"execute"}]}]})RQL"), true,
				{ { TEXT("E"), Begin }, { TEXT("P"), Print } })));

			// enhancedInput | call AddMappingContextOnBeginPlay("@BP@", "@IMC@")
			const TCHAR* AddContext = TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddMappingContextOnBeginPlay","args":[{"lit":"@BP@"},{"lit":"@IMC@"}]}]})RQL");
			const TSharedPtr<FJsonObject> Added = Run(AddContext, true);
			TestTrue(*FString::Printf(TEXT("added (%s)"), *Codes(Added)), IsOk(Added));
			TestTrue(TEXT("returns the Add Mapping Context node"), ResultRef(Added).StartsWith(TEXT("K2Node_CallFunction#")));

			UK2Node_Event* BeginPlay = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, AActor::StaticClass(), TEXT("ReceiveBeginPlay"));
			const UEdGraphPin* Then = BeginPlay ? BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then) : nullptr;
			const UK2Node_CallFunction* Add = Then && Then->LinkedTo.Num() == 1 ? Cast<UK2Node_CallFunction>(Then->LinkedTo[0]->GetOwningNode()) : nullptr;
			if (!TestNotNull(TEXT("BeginPlay runs Add Mapping Context first"), Add))
				return;
			TestEqual(TEXT("the call"), Add->FunctionReference.GetMemberName(), FName(TEXT("AddMappingContext")));
			const UEdGraphPin* ContextPin = Add->FindPin(TEXT("MappingContext"));
			TestTrue(TEXT("with the context"), ContextPin && ContextPin->DefaultObject && ContextPin->DefaultObject->GetPathName().StartsWith(ContextPath));
			const UEdGraphPin* AddThen = Add->FindPin(UEdGraphSchema_K2::PN_Then);
			TestTrue(TEXT("then what BeginPlay ran before"), AddThen && AddThen->LinkedTo.Num() == 1
				&& Cast<UK2Node_CallFunction>(AddThen->LinkedTo[0]->GetOwningNode())
				&& Cast<UK2Node_CallFunction>(AddThen->LinkedTo[0]->GetOwningNode())->FunctionReference.GetMemberName() == FName(TEXT("PrintString")));
			const UEdGraphPin* Target = Add->FindPin(UEdGraphSchema_K2::PN_Self);
			TestTrue(TEXT("on the local player's Enhanced Input subsystem"), Target && Target->LinkedTo.Num() == 1);
			TestEqual(TEXT("compiles without errors"), CompileErrors(Blueprint), 0);

			const TSharedPtr<FJsonObject> Again = Run(AddContext, true);
			TestTrue(*FString::Printf(TEXT("the same context again is refused (%s)"), *Codes(Again)), !IsOk(Again) && Codes(Again).Contains(TEXT("already adds")));
		});

		It("keeps a parent Blueprint's BeginPlay: a new BeginPlay in the child calls Parent: BeginPlay first", [this]()
		{
			UBlueprint* Parent = MakeBlueprint(ACharacter::StaticClass());
			if (!TestNotNull(TEXT("parent blueprint"), Parent))
				return;
			// unreal.asset("@BP@").EventGraph | call AddEvent("BeginPlay")
			TestTrue(TEXT("the parent implements BeginPlay"), IsOk(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["EventGraph"],"on":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]}},"stages":[{"op":"call","method":"AddEvent","args":[{"lit":"BeginPlay"}]}]})RQL"), true)));
			TestEqual(TEXT("the parent compiles"), CompileErrors(Parent), 0);
			UBlueprint* Child = MakeBlueprint(Parent->GeneratedClass);
			if (!TestNotNull(TEXT("child blueprint"), Child))
				return;
			// enhancedInput | call AddMappingContextOnBeginPlay("@BP@", "@IMC@")
			const TSharedPtr<FJsonObject> Added = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddMappingContextOnBeginPlay","args":[{"lit":"@BP@"},{"lit":"@IMC@"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("added (%s)"), *Codes(Added)), IsOk(Added));
			UK2Node_Event* BeginPlay = FBlueprintEditorUtils::FindOverrideForFunction(Child, AActor::StaticClass(), TEXT("ReceiveBeginPlay"));
			const UEdGraphPin* Then = BeginPlay ? BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then) : nullptr;
			TestTrue(TEXT("BeginPlay calls the parent's first"), Then && Then->LinkedTo.Num() == 1
				&& Then->LinkedTo[0]->GetOwningNode()->IsA<UK2Node_CallParentFunction>());
			TestEqual(TEXT("the child compiles"), CompileErrors(Child), 0);
		});

		It("refuses a Blueprint that is not an actor", [this]()
		{
			UBlueprint* Blueprint = MakeBlueprint(UObject::StaticClass());
			if (!TestNotNull(TEXT("object blueprint"), Blueprint))
				return;
			// enhancedInput | call AddActionEvent("@BP@", "@IA@")
			const TSharedPtr<FJsonObject> Refused = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddActionEvent","args":[{"lit":"@BP@"},{"lit":"@IA@"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("refused (%s)"), *Codes(Refused)), !IsOk(Refused) && Codes(Refused).Contains(TEXT("not an actor Blueprint")));
		});

		It("refuses an invalid asset path", [this]()
		{
			// enhancedInput | call CreateAction("/Engine/IA_RqlSpec")
			TSharedPtr<FJsonObject> Result;
			const bool bResultOk = UnrealMcpRqlTools::RunPlan(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateAction","args":[{"lit":"/Engine/IA_RqlSpec"}]}]})RQL"), true, &Result);
			TestTrue(TEXT("a well-formed result"), Result.IsValid());
			TestFalse(TEXT("fails defensively"), bResultOk);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
