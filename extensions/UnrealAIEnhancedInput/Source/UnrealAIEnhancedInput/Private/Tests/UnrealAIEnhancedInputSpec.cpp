// Copyright (c) 2026 IvanMurzak/Unreal-AI-EnhancedInput. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpRqlPlay.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "EnhancedInputComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "EnhancedActionKeyMapping.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "EnhancedInputDeveloperSettings.h"
#include "GameFramework/PlayerController.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Self.h"
#include "PlayerMappableKeySettings.h"
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
	/** What the play test changed in the editor world, put back after it. */
	TWeakObjectPtr<AWorldSettings> PlaySettings;
	TSubclassOf<AGameModeBase> PlayPreviousGameMode;
	TWeakObjectPtr<APlayerStart> PlayStart;
	bool bPlayLevelWasDirty = false;

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

	/** The result as JSON text, cut to @p MaxLength (messages stay short; checks of long results pass a larger one). */
	static FString Codes(const TSharedPtr<FJsonObject>& Result, int32 MaxLength = 600)
	{
		FString Text;
		if (Result.IsValid())
			FJsonSerializer::Serialize(Result.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		return Text.Left(MaxLength);
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

	static TSharedPtr<FJsonValue> S(const FString& Text) { return MakeShared<FJsonValueString>(Text); }
	static TSharedPtr<FJsonValue> N(double Number) { return MakeShared<FJsonValueNumber>(Number); }

	/** enhancedInput | call Method(Args...) as a mutation (each argument a literal); the QueryResult. */
	TSharedPtr<FJsonObject> Call(const FString& Method, const TArray<TSharedPtr<FJsonValue>>& Args)
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
		Source->SetArrayField(TEXT("path"), { S(TEXT("enhancedInput")) });
		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("rql"), TEXT("0.1"));
		Plan->SetObjectField(TEXT("source"), Source);
		Plan->SetArrayField(TEXT("stages"), { MakeShared<FJsonValueObject>(Stage) });
		FString Text;
		FJsonSerializer::Serialize(Plan.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		TSharedPtr<FJsonObject> Result;
		UnrealMcpRqlTools::RunPlan(Text, /*bMutate*/ true, &Result);
		return Result;
	}

	/** A fresh path under /Game/RqlSpec for @p Prefix. */
	static FString Fresh(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("/Game/RqlSpec/%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	/** The pins @p Pin is linked to, as "<owning node title>.<pin>" (for messages). */
	static FString Links(const UEdGraphPin* Pin)
	{
		TArray<FString> Out;
		if (Pin)
		{
			for (const UEdGraphPin* To : Pin->LinkedTo)
				Out.Add(FString::Printf(TEXT("%s.%s"), *To->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(), *To->PinName.ToString()));
		}
		return FString::Join(Out, TEXT(", "));
	}

	/** The call nodes of @p Blueprint calling @p Function. */
	static TArray<UK2Node_CallFunction*> CallsOf(UBlueprint* Blueprint, const TCHAR* Function)
	{
		TArray<UK2Node_CallFunction*> Calls;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Calls);
		return Calls.FilterByPredicate([Function](const UK2Node_CallFunction* Call) { return Call->FunctionReference.GetMemberName() == FName(Function); });
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
			for (const TCHAR* Operation : { TEXT("SetActionTriggers("), TEXT("SetActionModifiers("), TEXT("SetMappingTriggers("), TEXT("SetMappingModifiers("),
				TEXT("SetPlayerMappable("), TEXT("SetMappingPlayerMappable("), TEXT("EnableUserSettings("), TEXT("BindAction("), TEXT("BindActionToggle("),
				TEXT("AddMappingContextOnPossess("), TEXT("AddMappingContextOnEvent("), TEXT("RemoveMappingContextOnEvent(") })
				TestTrue(*FString::Printf(TEXT("lists %s"), Operation), Schema.Contains(Operation));
			TestTrue(TEXT("lists InjectInput("), Schema.Contains(TEXT("InjectInput(")));
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

		It("maps WASD into a 2D action with modifiers, lists them in KeyMappings and refuses an unknown modifier", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			ActionPath = FString::Printf(TEXT("/Game/RqlSpec/IA_Move_%s"), *Id);
			ContextPath = FString::Printf(TEXT("/Game/RqlSpec/IMC_Move_%s"), *Id);
			// enhancedInput | call CreateAction("@IA@", "Axis2D")
			TestTrue(TEXT("action created"), IsOk(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateAction","args":[{"lit":"@IA@"},{"lit":"Axis2D"}]}]})RQL"), true)));
			// enhancedInput | call CreateContext("@IMC@")
			TestTrue(TEXT("context created"), IsOk(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateContext","args":[{"lit":"@IMC@"}]}]})RQL"), true)));
			// [["W", "Swizzle"], ["S", "Swizzle, Negate"], ["A", "InputModifierNegate"], ["D", ""]] | call enhancedInput.AddMapping("@IMC@", "@IA@", $[0], $[1])
			const TSharedPtr<FJsonObject> Mapped = Run(TEXT(R"RQL({"rql":"0.1","source":{"list":[{"list":[{"lit":"W"},{"lit":"Swizzle"}]},{"list":[{"lit":"S"},{"lit":"Swizzle, Negate"}]},{"list":[{"lit":"A"},{"lit":"InputModifierNegate"}]},{"list":[{"lit":"D"},{"lit":""}]}]},"stages":[{"op":"call","method":"AddMapping","on":{"path":["enhancedInput"]},"args":[{"lit":"@IMC@"},{"lit":"@IA@"},{"path":[{"index":0}]},{"path":[{"index":1}]}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("WASD mapped (%s)"), *Codes(Mapped)), IsOk(Mapped));
			TMap<FString, TArray<FString>> Modifiers;
			if (const UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ContextPath))
			{
				for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
				{
					TArray<FString>& Names = Modifiers.FindOrAdd(Mapping.Key.GetFName().ToString());
					for (const UInputModifier* Modifier : Mapping.Modifiers)
						Names.Add(Modifier ? Modifier->GetClass()->GetName() : TEXT("null"));
				}
			}
			TestEqual(TEXT("four keys"), Modifiers.Num(), 4);
			TestEqual(TEXT("W swizzles"), Modifiers.FindRef(TEXT("W")), TArray<FString>{ TEXT("InputModifierSwizzleAxis") });
			TestEqual(TEXT("S swizzles then negates"), Modifiers.FindRef(TEXT("S")), TArray<FString>({ TEXT("InputModifierSwizzleAxis"), TEXT("InputModifierNegate") }));
			TestEqual(TEXT("A negates (full class name)"), Modifiers.FindRef(TEXT("A")), TArray<FString>{ TEXT("InputModifierNegate") });
			TestEqual(TEXT("D has none"), Modifiers.FindRef(TEXT("D")).Num(), 0);
			// unreal.asset("@IMC@") | select KeyMappings
			const FString Listed = Codes(Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@IMC@"}]},"stages":[{"op":"select","fields":[{"name":"KeyMappings","expr":{"path":["KeyMappings"]}}]}]})RQL"), false));
			TestTrue(*FString::Printf(TEXT("KeyMappings lists the modifiers (%s)"), *Listed), Listed.Contains(TEXT("SwizzleAxis")) && Listed.Contains(TEXT("Negate")));
			// enhancedInput | call AddMapping("@IMC@", "@IA@", "Up", "Negate, NoSuchModifier")
			const TSharedPtr<FJsonObject> Refused = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"AddMapping","args":[{"lit":"@IMC@"},{"lit":"@IA@"},{"lit":"Up"},{"lit":"Negate, NoSuchModifier"}]}]})RQL"), true);
			TestFalse(TEXT("unknown modifier refused"), IsOk(Refused));
			TestTrue(*FString::Printf(TEXT("the error lists the modifiers (%s)"), *Codes(Refused)), Codes(Refused).Contains(TEXT("SwizzleAxis")));
			const UInputMappingContext* After = LoadObject<UInputMappingContext>(nullptr, *ContextPath);
			TestFalse(TEXT("nothing mapped for the refused key"), After && After->GetMappings().ContainsByPredicate([](const FEnhancedActionKeyMapping& Mapping)
			{
				return Mapping.Key == EKeys::Up;
			}));
		});

		It("builds an action's Triggers and Modifiers from typed literals (instanced into the action)", [this]()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			ActionPath = FString::Printf(TEXT("/Game/RqlSpec/IA_Hold_%s"), *Id);
			// enhancedInput | call CreateAction("@IA@", "Boolean")
			TestTrue(TEXT("action created"), IsOk(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"CreateAction","args":[{"lit":"@IA@"},{"lit":"Boolean"}]}]})RQL"), true)));
			// unreal.asset("@IA@") | set Triggers = [{$type: "InputTriggerHold", HoldTimeThreshold: 0.5}]
			const TSharedPtr<FJsonObject> Triggers = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@IA@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Triggers"]},"value":{"list":[{"object":[{"key":"$type","value":{"lit":"InputTriggerHold"}},{"key":"HoldTimeThreshold","value":{"lit":0.5}}]}]}}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("Triggers set (%s)"), *Codes(Triggers)), IsOk(Triggers));
			// unreal.asset("@IA@") | set Modifiers = append(Modifiers, {$type: "InputModifierNegate"})
			const TCHAR* AppendNegate = TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@IA@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Modifiers"]},"value":{"call":"append","args":[{"path":["Modifiers"]},{"object":[{"key":"$type","value":{"lit":"InputModifierNegate"}}]}]}}]}]})RQL");
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				const TSharedPtr<FJsonObject> Modifiers = Run(AppendNegate, true);
				TestTrue(*FString::Printf(TEXT("Modifiers appended (%s)"), *Codes(Modifiers)), IsOk(Modifiers));
			}
			const UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
			if (!TestNotNull(TEXT("the action"), Action))
				return;
			const UInputTriggerHold* Hold = Action->Triggers.Num() == 1 ? Cast<UInputTriggerHold>(Action->Triggers[0]) : nullptr;
			TestTrue(TEXT("one Hold trigger"), Hold != nullptr);
			TestTrue(TEXT("HoldTimeThreshold 0.5, owned by the action"), Hold && Hold->HoldTimeThreshold == 0.5f && Hold->GetOuter() == Action);
			TestEqual(TEXT("two modifiers (append kept the first)"), Action->Modifiers.Num(), 2);
			for (const UInputModifier* Modifier : Action->Modifiers)
				TestTrue(TEXT("Negate, owned by the action"), Cast<UInputModifierNegate>(Modifier) != nullptr && Modifier->GetOuter() == Action);
			// unreal.asset("@IA@") | set Triggers = [{HoldTimeThreshold: 0.5}]
			const TSharedPtr<FJsonObject> Abstract = Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@IA@"}]},"stages":[{"op":"set","assign":[{"target":{"path":["Triggers"]},"value":{"list":[{"object":[{"key":"HoldTimeThreshold","value":{"lit":0.5}}]}]}}]}]})RQL"), true);
			TestFalse(TEXT("an abstract trigger is refused"), IsOk(Abstract));
			TestTrue(*FString::Printf(TEXT("asks for a concrete $type (%s)"), *Codes(Abstract)), Codes(Abstract).Contains(TEXT("InputTrigger is abstract: name a concrete class with $type")));
			TestEqual(TEXT("the refused write changed nothing"), Action->Triggers.Num(), 1);
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
			const UEdGraphNode* ParentCall = Then && Then->LinkedTo.Num() == 1 ? Then->LinkedTo[0]->GetOwningNode() : nullptr;
			TestTrue(TEXT("BeginPlay calls the parent's first"), ParentCall && ParentCall->IsA<UK2Node_CallParentFunction>() && ParentCall->IsNodeEnabled());
			TArray<UK2Node_Event*> Events;
			FBlueprintEditorUtils::GetAllNodesOfClass(Child, Events);
			TestEqual(TEXT("exactly one BeginPlay in the child"), Events.FilterByPredicate([](const UK2Node_Event* Event)
			{
				return Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay");
			}).Num(), 1);
			TArray<UK2Node_CallParentFunction*> ParentCalls;
			FBlueprintEditorUtils::GetAllNodesOfClass(Child, ParentCalls);
			TestEqual(TEXT("one Parent: BeginPlay (the placeholder's, enabled; not a second one)"), ParentCalls.FilterByPredicate([](const UK2Node_CallParentFunction* Call)
			{
				return Call->FunctionReference.GetMemberName() == TEXT("ReceiveBeginPlay");
			}).Num(), 1);
			const UEdGraphPin* AfterParent = ParentCall ? ParentCall->FindPin(UEdGraphSchema_K2::PN_Then) : nullptr;
			const UK2Node_CallFunction* Add = AfterParent && AfterParent->LinkedTo.Num() == 1 ? Cast<UK2Node_CallFunction>(AfterParent->LinkedTo[0]->GetOwningNode()) : nullptr;
			TestTrue(*FString::Printf(TEXT("then Add Mapping Context (%s)"), *Links(AfterParent)), Add && Add->FunctionReference.GetMemberName() == TEXT("AddMappingContext"));
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

	Describe("play test", [this]()
	{
		AfterEach([this]()
		{
			if (GEditor && GEditor->PlayWorld)
				GEditor->EndPlayMap();
			if (APlayerStart* Start = PlayStart.Get())
				Start->GetWorld()->EditorDestroyActor(Start, /*bShouldModifyLevel*/ true);
			if (AWorldSettings* Settings = PlaySettings.Get())
			{
				Settings->DefaultGameMode = PlayPreviousGameMode;
				Settings->GetOutermost()->SetDirtyFlag(bPlayLevelWasDirty);
			}
			PlaySettings.Reset();
			PlayStart.Reset();
		});

		It("refuses InjectInput without a play session and names StartPlay", [this]()
		{
			MakeBlueprint(ACharacter::StaticClass());
			// enhancedInput | call InjectInput("@IA@")
			const TSharedPtr<FJsonObject> Result = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"InjectInput","args":[{"lit":"@IA@"}]}]})RQL"), true);
			TestFalse(TEXT("refused"), IsOk(Result));
			TestTrue(*FString::Printf(TEXT("names StartPlay (%s)"), *Codes(Result)), Codes(Result).Contains(TEXT("StartPlay()")));
		});

		It("injects an action for exact frames inside the call, reports its trigger events and the applied mapping contexts; a held key fires its mapped action", [this]()
		{
			MakeBlueprint(ACharacter::StaticClass());
			UInputAction* Action = Cast<UInputAction>(UnrealMcpRql::LoadObject(UInputAction::StaticClass(), ActionPath));
			UInputMappingContext* Context = Cast<UInputMappingContext>(UnrealMcpRql::LoadObject(UInputMappingContext::StaticClass(), ContextPath));
			if (!TestNotNull(TEXT("the action asset"), Action) || !TestNotNull(TEXT("the context asset"), Context))
				return;
			Context->MapKey(Action, EKeys::W);

			// Play the open level with GameModeBase from an added PlayerStart (both undone after: later specs use the level).
			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			AWorldSettings* Settings = EditorWorld ? EditorWorld->GetWorldSettings() : nullptr;
			if (!TestNotNull(TEXT("editor world settings"), Settings))
				return;
			PlaySettings = Settings;
			bPlayLevelWasDirty = EditorWorld->GetOutermost()->IsDirty();
			PlayPreviousGameMode = Settings->DefaultGameMode;
			Settings->DefaultGameMode = AGameModeBase::StaticClass();
			PlayStart = EditorWorld->SpawnActor<APlayerStart>(FVector(0.0, 0.0, 5000.0), FRotator::ZeroRotator);
			FString Error;
			if (!TestTrue(*FString::Printf(TEXT("play started (%s)"), *Error), UnrealMcpRql::StartPlay(/*bPaused*/ true, Error)))
				return;
			APlayerController* Controller = UnrealMcpRql::PlayController();
			if (!TestNotNull(TEXT("player 0"), Controller))
				return;
			UEnhancedInputLocalPlayerSubsystem* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer());
			UEnhancedInputComponent* Bindings = Cast<UEnhancedInputComponent>(Controller->InputComponent);
			if (!TestNotNull(TEXT("the Enhanced Input subsystem"), Input) || !TestNotNull(TEXT("an Enhanced Input component"), Bindings))
				return;
			Input->AddMappingContext(Context, 3);
			int32 Triggered = 0, Completed = 0;
			Bindings->BindActionValueLambda(Action, ETriggerEvent::Triggered, [&Triggered](const FInputActionValue&) { ++Triggered; });
			Bindings->BindActionValueLambda(Action, ETriggerEvent::Completed, [&Completed](const FInputActionValue&) { ++Completed; });

			// enhancedInput | call InjectInput("@IA@", "(1, 0, 0)", 0, 3)
			const TSharedPtr<FJsonObject> Result = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"InjectInput","args":[{"lit":"@IA@"},{"lit":"(1, 0, 0)"},{"lit":0},{"lit":3}]}]})RQL"), true);
			const FString Receipt = Codes(Result, 4000);
			TestTrue(*FString::Printf(TEXT("InjectInput ok (%s)"), *Receipt), IsOk(Result));
			TestTrue(*FString::Printf(TEXT("held 3 frames, then released (%s)"), *Receipt), Receipt.Contains(TEXT("3 frame(s) held + 1 released")));
			TestEqual(TEXT("the bound Triggered event fired once per held frame"), Triggered, 3);
			TestEqual(TEXT("Completed fired on release"), Completed, 1);
			TestTrue(*FString::Printf(TEXT("the notes list the events (%s)"), *Receipt),
				Receipt.Contains(TEXT("Triggered x3")) && Receipt.Contains(TEXT("Completed x1")));
			const FString ContextName = FPackageName::GetShortName(ContextPath);
			TestTrue(*FString::Printf(TEXT("the applied context in the state (%s)"), *Receipt), Receipt.Contains(ContextName + TEXT(" (priority 3)")));
			TestTrue(TEXT("every play state lists it"), UnrealMcpRql::ReadPlayState().MappingContexts.Contains(ContextName + TEXT(" (priority 3)")));

			// The core AdvancePlay key path: W, mapped to the action in the applied context, fires it like a real key.
			Triggered = Completed = 0;
			int32 Advanced = 0;
			TestTrue(*FString::Printf(TEXT("held W (%s)"), *Error), UnrealMcpRql::AdvancePlayWithKeys(0.0, 4, TEXT("W"), Advanced, Error));
			TestEqual(TEXT("the mapped action triggered on every held frame"), Triggered, 4);
			TestEqual(TEXT("and completed on release"), Completed, 1);

			// A trigger that fires on release: held frames are Ongoing, the release frame triggers.
			Triggered = Completed = 0;
			// enhancedInput | call InjectInput("@IA@", "(1, 0, 0)", 0, 2, "Released")
			const TSharedPtr<FJsonObject> OnRelease = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"InjectInput","args":[{"lit":"@IA@"},{"lit":"(1, 0, 0)"},{"lit":0},{"lit":2},{"lit":"Released"}]}]})RQL"), true);
			TestTrue(*FString::Printf(TEXT("Released trigger ok (%s)"), *Codes(OnRelease)), IsOk(OnRelease));
			TestEqual(TEXT("a Released trigger fires once, on the release frame"), Triggered, 1);

			// An unknown trigger is refused before anything is simulated.
			const double Time = UnrealMcpRql::ReadPlayState().Time;
			// enhancedInput | call InjectInput("@IA@", "(1, 0, 0)", 0, 3, "NoSuchTrigger")
			const TSharedPtr<FJsonObject> Bad = Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["enhancedInput"]},"stages":[{"op":"call","method":"InjectInput","args":[{"lit":"@IA@"},{"lit":"(1, 0, 0)"},{"lit":0},{"lit":3},{"lit":"NoSuchTrigger"}]}]})RQL"), true);
			TestFalse(TEXT("unknown trigger refused"), IsOk(Bad));
			TestEqual(TEXT("nothing simulated"), UnrealMcpRql::ReadPlayState().Time, Time, 1e-6);
			Bindings->ClearActionBindings();
		});
	});

	Describe("triggers and modifiers as text", [this]()
	{
		It("builds an action's triggers and modifiers from text, reads them back as the same text and refuses wrong names before changing", [this]()
		{
			ActionPath = Fresh(TEXT("IA_Interact"));
			const FString Chord = Fresh(TEXT("IA_Modifier"));
			TestTrue(TEXT("chord action created"), IsOk(Call(TEXT("CreateAction"), { S(Chord), S(TEXT("Boolean")) })));
			const TSharedPtr<FJsonObject> Created = Call(TEXT("CreateAction"), { S(ActionPath), S(TEXT("Boolean")), S(TEXT("Hold(0.5, OneShot=true)")),
				S(TEXT("DeadZone(LowerThreshold=0.25, Type=Axial), Scalar(2, 1, 1), Negate(Y)")) });
			TestTrue(*FString::Printf(TEXT("created with text (%s)"), *Codes(Created)), IsOk(Created));
			const UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
			if (!TestNotNull(TEXT("the action"), Action))
				return;
			const UInputTriggerHold* Hold = Action->Triggers.Num() == 1 ? Cast<UInputTriggerHold>(Action->Triggers[0]) : nullptr;
			TestTrue(TEXT("Hold 0.5 one shot, owned by the action"), Hold && Hold->HoldTimeThreshold == 0.5f && Hold->bIsOneShot && Hold->GetOuter() == Action);
			const UInputModifierDeadZone* DeadZone = Action->Modifiers.Num() == 3 ? Cast<UInputModifierDeadZone>(Action->Modifiers[0]) : nullptr;
			TestTrue(TEXT("DeadZone 0.25 axial"), DeadZone && DeadZone->LowerThreshold == 0.25f && DeadZone->Type == EDeadZoneType::Axial);
			const UInputModifierScalar* Scalar = Action->Modifiers.Num() == 3 ? Cast<UInputModifierScalar>(Action->Modifiers[1]) : nullptr;
			TestTrue(TEXT("Scalar (2, 1, 1)"), Scalar && Scalar->Scalar.Equals(FVector(2, 1, 1)));
			const UInputModifierNegate* Negate = Action->Modifiers.Num() == 3 ? Cast<UInputModifierNegate>(Action->Modifiers[2]) : nullptr;
			TestTrue(TEXT("Negate(Y) negates Y only"), Negate && !Negate->bX && Negate->bY && !Negate->bZ);

			// unreal.asset("@IA@") | select TriggersText ModifiersText PlayerMappable
			const TCHAR* ReadBack = TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@IA@"}]},"stages":[{"op":"select","fields":[{"name":"TriggersText","expr":{"path":["TriggersText"]}},{"name":"ModifiersText","expr":{"path":["ModifiersText"]}},{"name":"PlayerMappable","expr":{"path":["PlayerMappable"]}}]}]})RQL");
			const FString Text = Codes(Run(ReadBack, false));
			TestTrue(*FString::Printf(TEXT("TriggersText (%s)"), *Text), Text.Contains(TEXT("Hold(HoldTimeThreshold=0.5, bIsOneShot=true)")));
			TestTrue(*FString::Printf(TEXT("ModifiersText (%s)"), *Text), Text.Contains(TEXT("DeadZone(LowerThreshold=0.25, Type=Axial), Scalar(Scalar=(2, 1, 1)), Negate(bX=false, bZ=false)")));

			const TSharedPtr<FJsonObject> Chorded = Call(TEXT("SetActionTriggers"), { S(ActionPath), S(FString::Printf(TEXT("Chorded(%s), Pressed"), *Chord)) });
			TestTrue(*FString::Printf(TEXT("chord + pressed (%s)"), *Codes(Chorded)), IsOk(Chorded) && Codes(Chorded).Contains(TEXT("ChordAction(ChordAction=")) && Codes(Chorded).Contains(TEXT("Pressed")));
			const UInputTriggerChordAction* ChordTrigger = Action->Triggers.Num() == 2 ? Cast<UInputTriggerChordAction>(Action->Triggers[0]) : nullptr;
			TestTrue(TEXT("the chord names the other action"), ChordTrigger && ChordTrigger->ChordAction && ChordTrigger->ChordAction->GetPathName().StartsWith(Chord));

			const TSharedPtr<FJsonObject> Unknown = Call(TEXT("SetActionTriggers"), { S(ActionPath), S(TEXT("Hodl(0.5)")) });
			TestTrue(*FString::Printf(TEXT("an unknown trigger lists the triggers (%s)"), *Codes(Unknown)), !IsOk(Unknown) && Codes(Unknown).Contains(TEXT("HoldAndRelease")) && Codes(Unknown).Contains(TEXT("Pulse")));
			const TSharedPtr<FJsonObject> Setting = Call(TEXT("SetActionTriggers"), { S(ActionPath), S(TEXT("Hold(Seconds=1)")) });
			TestTrue(*FString::Printf(TEXT("an unknown setting lists the settings (%s)"), *Codes(Setting)), !IsOk(Setting) && Codes(Setting).Contains(TEXT("HoldTimeThreshold=1 (first argument)")));
			const TSharedPtr<FJsonObject> Enum = Call(TEXT("SetActionModifiers"), { S(ActionPath), S(TEXT("DeadZone(Type=Square)")) });
			TestTrue(*FString::Printf(TEXT("a wrong enum value lists the entries (%s)"), *Codes(Enum)), !IsOk(Enum) && Codes(Enum).Contains(TEXT("Axial")));
			TestEqual(TEXT("the refused writes changed nothing"), Action->Triggers.Num(), 2);
			TestEqual(TEXT("the refused modifiers changed nothing"), Action->Modifiers.Num(), 3);
			TestTrue(TEXT("clearing with \"\""), IsOk(Call(TEXT("SetActionTriggers"), { S(ActionPath), S(TEXT("")) })) && Action->Triggers.Num() == 0);
		});

		It("gives single mappings their own triggers and modifiers and lists every mapping in inputMappings", [this]()
		{
			ActionPath = Fresh(TEXT("IA_Use"));
			ContextPath = Fresh(TEXT("IMC_Use"));
			TestTrue(TEXT("action"), IsOk(Call(TEXT("CreateAction"), { S(ActionPath), S(TEXT("Boolean")) })));
			TestTrue(TEXT("context"), IsOk(Call(TEXT("CreateContext"), { S(ContextPath) })));
			TestTrue(TEXT("E with a Tap"), IsOk(Call(TEXT("AddMapping"), { S(ContextPath), S(ActionPath), S(TEXT("E")), S(TEXT("")), S(TEXT("Tap(0.3)")) })));
			TestTrue(TEXT("gamepad X"), IsOk(Call(TEXT("AddMapping"), { S(ContextPath), S(ActionPath), S(TEXT("Gamepad_FaceButton_Left")) })));
			const TSharedPtr<FJsonObject> Hold = Call(TEXT("SetMappingTriggers"), { S(ContextPath), S(ActionPath), S(TEXT("Gamepad_FaceButton_Left")), S(TEXT("HoldAndRelease(0.4)")) });
			TestTrue(*FString::Printf(TEXT("gamepad hold-and-release (%s)"), *Codes(Hold)), IsOk(Hold));
			TestTrue(TEXT("gamepad scalar"), IsOk(Call(TEXT("SetMappingModifiers"), { S(ContextPath), S(ActionPath), S(TEXT("Gamepad_FaceButton_Left")), S(TEXT("Scalar(0.5)")) })));
			const TSharedPtr<FJsonObject> Missing = Call(TEXT("SetMappingTriggers"), { S(ContextPath), S(ActionPath), S(TEXT("Q")), S(TEXT("Pressed")) });
			TestTrue(*FString::Printf(TEXT("a key that is not mapped lists the mappings (%s)"), *Codes(Missing)), !IsOk(Missing) && Codes(Missing).Contains(TEXT(": E")));

			const UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ContextPath);
			if (!TestNotNull(TEXT("the context"), Context) || !TestEqual(TEXT("two mappings"), Context->GetMappings().Num(), 2))
				return;
			const FEnhancedActionKeyMapping& Keyboard = Context->GetMappings()[0];
			TestTrue(TEXT("E taps (0.3), owned by the context"), Keyboard.Triggers.Num() == 1 && Cast<UInputTriggerTap>(Keyboard.Triggers[0])
				&& Cast<UInputTriggerTap>(Keyboard.Triggers[0])->TapReleaseTimeThreshold == 0.3f && Keyboard.Triggers[0]->GetOuter() == Context);
			const FEnhancedActionKeyMapping& Pad = Context->GetMappings()[1];
			TestTrue(TEXT("pad holds and releases"), Pad.Triggers.Num() == 1 && Cast<UInputTriggerHoldAndRelease>(Pad.Triggers[0]));
			TestTrue(TEXT("pad scales"), Pad.Modifiers.Num() == 1 && Cast<UInputModifierScalar>(Pad.Modifiers[0]));

			// inputMappings | where Context == "@CTX@" | select Key Triggers Modifiers PlayerMappable Settings MappingName
			const FString Rows = Codes(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["inputMappings"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Context"]},{"lit":"@CTX@"}]}},{"op":"select","fields":[{"name":"Key","expr":{"path":["Key"]}},{"name":"Triggers","expr":{"path":["Triggers"]}},{"name":"Modifiers","expr":{"path":["Modifiers"]}},{"name":"PlayerMappable","expr":{"path":["PlayerMappable"]}},{"name":"Settings","expr":{"path":["Settings"]}},{"name":"MappingName","expr":{"path":["MappingName"]}}]}]})RQL"), false,
				{ { TEXT("CTX"), FPackageName::GetShortName(ContextPath) } }));
			TestTrue(*FString::Printf(TEXT("the rows (%s)"), *Rows), Rows.Contains(TEXT("Tap(TapReleaseTimeThreshold=0.3)")) && Rows.Contains(TEXT("HoldAndRelease(HoldTimeThreshold=0.4)"))
				&& Rows.Contains(TEXT("Scalar(Scalar=(0.5, 0.5, 0.5))")));
		});

		It("lists the trigger and modifier types with their settings", [this]()
		{
			// inputTriggerTypes | where Name == "Hold" | select Settings
			const FString Hold = Codes(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["inputTriggerTypes"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Hold"}]}},{"op":"select","fields":[{"name":"Settings","expr":{"path":["Settings"]}}]}]})RQL"), false));
			TestTrue(*FString::Printf(TEXT("Hold's settings (%s)"), *Hold), Hold.Contains(TEXT("HoldTimeThreshold=1 (first argument)")) && Hold.Contains(TEXT("bIsOneShot=false")));
			// inputModifierTypes | where Name == "Negate" | select Class
			const FString Negate = Codes(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["inputModifierTypes"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Name"]},{"lit":"Negate"}]}},{"op":"select","fields":[{"name":"Class","expr":{"path":["Class"]}}]}]})RQL"), false));
			TestTrue(*FString::Printf(TEXT("Negate's class (%s)"), *Negate), Negate.Contains(TEXT("InputModifierNegate")));
		});
	});

	Describe("player-mappable keys", [this]()
	{
		It("makes an action rebindable, overrides or ignores single mappings and reads it back", [this]()
		{
			ActionPath = Fresh(TEXT("IA_Jump"));
			ContextPath = Fresh(TEXT("IMC_Keys"));
			TestTrue(TEXT("action"), IsOk(Call(TEXT("CreateAction"), { S(ActionPath), S(TEXT("Boolean")) })));
			TestTrue(TEXT("context"), IsOk(Call(TEXT("CreateContext"), { S(ContextPath) })));
			for (const TCHAR* Key : { TEXT("SpaceBar"), TEXT("Gamepad_FaceButton_Bottom"), TEXT("Gamepad_FaceButton_Right") })
				TestTrue(*FString::Printf(TEXT("mapped %s"), Key), IsOk(Call(TEXT("AddMapping"), { S(ContextPath), S(ActionPath), S(Key) })));
			const TSharedPtr<FJsonObject> Mappable = Call(TEXT("SetPlayerMappable"), { S(ActionPath), S(TEXT("Jump")), S(TEXT("On Foot")) });
			TestTrue(*FString::Printf(TEXT("action mappable (%s)"), *Codes(Mappable)), IsOk(Mappable) && Codes(Mappable).Contains(FPackageName::GetShortName(ActionPath)));
			TestTrue(TEXT("one pad key has its own name"), IsOk(Call(TEXT("SetMappingPlayerMappable"), { S(ContextPath), S(ActionPath), S(TEXT("Gamepad_FaceButton_Bottom")), S(TEXT("Override")), S(TEXT("Jump (Pad)")), S(TEXT("On Foot")) })));
			TestTrue(TEXT("one key is never rebindable"), IsOk(Call(TEXT("SetMappingPlayerMappable"), { S(ContextPath), S(ActionPath), S(TEXT("Gamepad_FaceButton_Right")), S(TEXT("Ignore")) })));
			const TSharedPtr<FJsonObject> Mode = Call(TEXT("SetMappingPlayerMappable"), { S(ContextPath), S(ActionPath), S(TEXT("SpaceBar")), S(TEXT("Sometimes")) });
			TestTrue(*FString::Printf(TEXT("a wrong mode is refused (%s)"), *Codes(Mode)), !IsOk(Mode) && Codes(Mode).Contains(TEXT("Override")));

			const UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
			const UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ContextPath);
			if (!TestNotNull(TEXT("the action"), Action) || !TestNotNull(TEXT("the context"), Context) || !TestEqual(TEXT("three mappings"), Context->GetMappings().Num(), 3))
				return;
			const UPlayerMappableKeySettings* Settings = Action->GetPlayerMappableKeySettings();
			TestTrue(TEXT("action settings: display name, category, name, owned by the action"), Settings && Settings->DisplayName.ToString() == TEXT("Jump")
				&& Settings->DisplayCategory.ToString() == TEXT("On Foot") && Settings->Name == FName(*FPackageName::GetShortName(ActionPath)) && Settings->GetOuter() == Action);
			const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
			TestTrue(TEXT("SpaceBar inherits"), Settings && Mappings[0].IsPlayerMappable() && Mappings[0].GetMappingName() == Settings->Name);
			TestTrue(TEXT("the pad key overrides"), Mappings[1].IsPlayerMappable() && Mappings[1].GetDisplayName().ToString() == TEXT("Jump (Pad)")
				&& Mappings[1].GetMappingName() == FName(*(FPackageName::GetShortName(ActionPath) + TEXT("_Gamepad_FaceButton_Bottom"))));
			TestFalse(TEXT("the ignored key is not rebindable"), Mappings[2].IsPlayerMappable());

			// unreal.asset("@IA@") | select TriggersText ModifiersText PlayerMappable
			const FString Read = Codes(Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@IA@"}]},"stages":[{"op":"select","fields":[{"name":"TriggersText","expr":{"path":["TriggersText"]}},{"name":"ModifiersText","expr":{"path":["ModifiersText"]}},{"name":"PlayerMappable","expr":{"path":["PlayerMappable"]}}]}]})RQL"), false));
			TestTrue(*FString::Printf(TEXT("PlayerMappable member (%s)"), *Read), Read.Contains(TEXT("On Foot")));
			// inputMappings | where Context == "@CTX@" | select Key Triggers Modifiers PlayerMappable Settings MappingName
			const FString Rows = Codes(Run(TEXT(R"RQL({"rql":"0.1","source":{"path":["inputMappings"]},"stages":[{"op":"where","pred":{"op":"==","args":[{"path":["Context"]},{"lit":"@CTX@"}]}},{"op":"select","fields":[{"name":"Key","expr":{"path":["Key"]}},{"name":"Triggers","expr":{"path":["Triggers"]}},{"name":"Modifiers","expr":{"path":["Modifiers"]}},{"name":"PlayerMappable","expr":{"path":["PlayerMappable"]}},{"name":"Settings","expr":{"path":["Settings"]}},{"name":"MappingName","expr":{"path":["MappingName"]}}]}]})RQL"), false,
				{ { TEXT("CTX"), FPackageName::GetShortName(ContextPath) } }));
			TestTrue(*FString::Printf(TEXT("rows show inherit / override / ignore (%s)"), *Rows), Rows.Contains(TEXT("Inherit")) && Rows.Contains(TEXT("Override")) && Rows.Contains(TEXT("Ignore")));
		});

		It("turns the Enhanced Input user settings on and off", [this]()
		{
			UEnhancedInputDeveloperSettings* Settings = GetMutableDefault<UEnhancedInputDeveloperSettings>();
			const bool bWas = Settings->bEnableUserSettings;
			const TSharedPtr<FJsonObject> On = Call(TEXT("EnableUserSettings"), {});
			TestTrue(*FString::Printf(TEXT("enabled (%s)"), *Codes(On)), IsOk(On) && Codes(On).Contains(TEXT("-> true")));
			TestTrue(TEXT("the setting is on"), Settings->bEnableUserSettings != 0);
			TestTrue(TEXT("restored"), IsOk(Call(TEXT("EnableUserSettings"), { MakeShared<FJsonValueBoolean>(bWas) })));
			TestEqual(TEXT("back to what it was"), Settings->bEnableUserSettings != 0, bWas);
		});
	});

	Describe("control schemes in Blueprints", [this]()
	{
		It("swaps a pawn's scheme on possession: removes for the old controller, adds for the new one, registered for rebinding", [this]()
		{
			UBlueprint* Blueprint = MakeBlueprint(ACharacter::StaticClass());
			if (!TestNotNull(TEXT("character blueprint"), Blueprint))
				return;
			const TSharedPtr<FJsonObject> Added = Call(TEXT("AddMappingContextOnPossess"), { S(BlueprintPath), S(ContextPath), N(2) });
			TestTrue(*FString::Printf(TEXT("added (%s)"), *Codes(Added)), IsOk(Added) && ResultRef(Added).StartsWith(TEXT("K2Node_CallFunction#")));
			UK2Node_Event* Changed = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, APawn::StaticClass(), TEXT("ReceiveControllerChanged"));
			if (!TestNotNull(TEXT("an On Controller Changed event"), Changed))
				return;
			const UEdGraphPin* Then = Changed->FindPin(UEdGraphSchema_K2::PN_Then);
			const UK2Node_DynamicCast* OldCast = Then && Then->LinkedTo.Num() == 1 ? Cast<UK2Node_DynamicCast>(Then->LinkedTo[0]->GetOwningNode()) : nullptr;
			TestTrue(*FString::Printf(TEXT("first: the old controller cast to a PlayerController (%s)"), *Links(Then)), OldCast && OldCast->TargetType == APlayerController::StaticClass()
				&& OldCast->GetCastSourcePin()->LinkedTo.Num() == 1 && OldCast->GetCastSourcePin()->LinkedTo[0]->PinName == TEXT("OldController"));
			const TArray<UK2Node_CallFunction*> Removes = CallsOf(Blueprint, TEXT("RemoveMappingContext"));
			const TArray<UK2Node_CallFunction*> Adds = CallsOf(Blueprint, TEXT("AddMappingContext"));
			if (!TestEqual(TEXT("one Remove"), Removes.Num(), 1) || !TestEqual(TEXT("one Add"), Adds.Num(), 1))
				return;
			const UK2Node_IfThenElse* IsLocal = OldCast && OldCast->GetValidCastPin()->LinkedTo.Num() == 1 ? Cast<UK2Node_IfThenElse>(OldCast->GetValidCastPin()->LinkedTo[0]->GetOwningNode()) : nullptr;
			TestTrue(*FString::Printf(TEXT("a cast controller is checked to be local (%s)"), OldCast ? *Links(OldCast->GetValidCastPin()) : TEXT("no cast")), IsLocal != nullptr);
			TestTrue(TEXT("Remove runs for a local controller"), IsLocal && IsLocal->GetThenPin()->LinkedTo.Num() == 1 && IsLocal->GetThenPin()->LinkedTo[0]->GetOwningNode() == Removes[0]);
			const UK2Node_DynamicCast* NewCast = OldCast && OldCast->GetInvalidCastPin()->LinkedTo.Num() == 1 ? Cast<UK2Node_DynamicCast>(OldCast->GetInvalidCastPin()->LinkedTo[0]->GetOwningNode()) : nullptr;
			TestTrue(*FString::Printf(TEXT("no old player: on to the new controller's cast (%s)"), OldCast ? *Links(OldCast->GetInvalidCastPin()) : TEXT("no cast")),
				NewCast && NewCast->GetCastSourcePin()->LinkedTo.Num() == 1 && NewCast->GetCastSourcePin()->LinkedTo[0]->PinName == TEXT("NewController"));
			const UEdGraphPin* Priority = Adds[0]->FindPin(TEXT("Priority"));
			TestTrue(TEXT("priority 2"), Priority && Priority->DefaultValue == TEXT("2"));
			const UEdGraphPin* Options = Adds[0]->FindPin(TEXT("Options"));
			TestTrue(*FString::Printf(TEXT("registers with the user settings (%s)"), Options ? *Options->DefaultValue : TEXT("no pin")), Options && Options->DefaultValue.Contains(TEXT("bNotifyUserSettings=True")));
			TestEqual(TEXT("compiles without errors"), CompileErrors(Blueprint), 0);
			const TSharedPtr<FJsonObject> Again = Call(TEXT("AddMappingContextOnEvent"), { S(BlueprintPath), S(ContextPath), S(TEXT("BeginPlay")) });
			TestTrue(*FString::Printf(TEXT("the same context again is refused, naming the event (%s)"), *Codes(Again)), !IsOk(Again) && Codes(Again).Contains(TEXT("already adds")) && Codes(Again).Contains(TEXT("ControllerChanged")));
			// unreal.asset("@BP@") | select InputSetup
			const FString Setup = Codes(Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"select","fields":[{"name":"InputSetup","expr":{"path":["InputSetup"]}}]}]})RQL"), false), 100000);
			TestTrue(*FString::Printf(TEXT("InputSetup lists both (%s)"), *Setup), Setup.Contains(TEXT("ControllerChanged")) && Setup.Contains(TEXT("Remove")) && Setup.Contains(TEXT("Priority")));
		});

		It("adds and removes contexts on custom events and controller events; refuses pawn events on a controller", [this]()
		{
			UBlueprint* Controller = MakeBlueprint(APlayerController::StaticClass());
			if (!TestNotNull(TEXT("controller blueprint"), Controller))
				return;
			const TSharedPtr<FJsonObject> Possess = Call(TEXT("AddMappingContextOnEvent"), { S(BlueprintPath), S(ContextPath), S(TEXT("OnPossess")), N(1) });
			TestTrue(*FString::Printf(TEXT("OnPossess (%s)"), *Codes(Possess)), IsOk(Possess));
			const TArray<UK2Node_CallFunction*> Adds = CallsOf(Controller, TEXT("AddMappingContext"));
			const UEdGraphPin* Target = Adds.Num() == 1 ? Adds[0]->FindPin(UEdGraphSchema_K2::PN_Self) : nullptr;
			const UEdGraphNode* Subsystem = Target && Target->LinkedTo.Num() == 1 ? Target->LinkedTo[0]->GetOwningNode() : nullptr;
			const UEdGraphPin* PlayerPin = Subsystem ? Subsystem->FindPin(TEXT("PlayerController")) : nullptr;
			TestTrue(*FString::Printf(TEXT("the controller's own subsystem (%s)"), *Links(PlayerPin)), PlayerPin && PlayerPin->LinkedTo.Num() == 1 && Cast<UK2Node_Self>(PlayerPin->LinkedTo[0]->GetOwningNode()));
			const FString Second = Fresh(TEXT("IMC_Menu"));
			TestTrue(TEXT("menu context"), IsOk(Call(TEXT("CreateContext"), { S(Second) })));
			const TSharedPtr<FJsonObject> Wrong = Call(TEXT("AddMappingContextOnEvent"), { S(BlueprintPath), S(Second), S(TEXT("Possessed")) });
			TestTrue(*FString::Printf(TEXT("Possessed on a controller names the controller events (%s)"), *Codes(Wrong)), !IsOk(Wrong) && Codes(Wrong).Contains(TEXT("OnPossess")));
			const TSharedPtr<FJsonObject> Custom = Call(TEXT("AddMappingContextOnEvent"), { S(BlueprintPath), S(Second), S(TEXT("OpenMenu")), N(10) });
			TestTrue(*FString::Printf(TEXT("a custom event is created (%s)"), *Codes(Custom)), IsOk(Custom) && FBlueprintEditorUtils::FindCustomEventNode(Controller, TEXT("OpenMenu")));
			const TSharedPtr<FJsonObject> Remove = Call(TEXT("RemoveMappingContextOnEvent"), { S(BlueprintPath), S(Second), S(TEXT("CloseMenu")) });
			TestTrue(*FString::Printf(TEXT("removed on another custom event (%s)"), *Codes(Remove)), IsOk(Remove) && CallsOf(Controller, TEXT("RemoveMappingContext")).Num() == 1);
			const TSharedPtr<FJsonObject> Twice = Call(TEXT("RemoveMappingContextOnEvent"), { S(BlueprintPath), S(Second), S(TEXT("CloseMenu")) });
			TestTrue(*FString::Printf(TEXT("the same removal twice is refused (%s)"), *Codes(Twice)), !IsOk(Twice) && Codes(Twice).Contains(TEXT("already removes")));
			TestEqual(TEXT("compiles without errors"), CompileErrors(Controller), 0);
		});

		It("binds actions to inherited functions, appends a second bind, passes axis values and toggles with a FlipFlop", [this]()
		{
			UBlueprint* Blueprint = MakeBlueprint(ACharacter::StaticClass());
			if (!TestNotNull(TEXT("character blueprint"), Blueprint))
				return;
			const TSharedPtr<FJsonObject> Jump = Call(TEXT("BindAction"), { S(BlueprintPath), S(ActionPath), S(TEXT("Started")), S(TEXT("Jump")) });
			TestTrue(*FString::Printf(TEXT("Started -> Jump (%s)"), *Codes(Jump)), IsOk(Jump));
			TestTrue(TEXT("Completed -> StopJumping"), IsOk(Call(TEXT("BindAction"), { S(BlueprintPath), S(ActionPath), S(TEXT("Completed")), S(TEXT("StopJumping")) })));
			TestTrue(TEXT("a second Started call appends"), IsOk(Call(TEXT("BindAction"), { S(BlueprintPath), S(ActionPath), S(TEXT("Started")), S(TEXT("UnCrouch")) })));
			const TSharedPtr<FJsonObject> Twice = Call(TEXT("BindAction"), { S(BlueprintPath), S(ActionPath), S(TEXT("Started")), S(TEXT("Jump")) });
			TestTrue(*FString::Printf(TEXT("the same call twice is refused (%s)"), *Codes(Twice)), !IsOk(Twice) && Codes(Twice).Contains(TEXT("already calls")));
			const TSharedPtr<FJsonObject> NoParameter = Call(TEXT("BindAction"), { S(BlueprintPath), S(ActionPath), S(TEXT("Completed")), S(TEXT("UnCrouch")), S(TEXT("Nope")) });
			TestTrue(*FString::Printf(TEXT("an unknown value parameter is refused (%s)"), *Codes(NoParameter)), !IsOk(NoParameter) && Codes(NoParameter).Contains(TEXT("no input parameter")));
			TestEqual(TEXT("the refused bind added no node"), CallsOf(Blueprint, TEXT("UnCrouch")).Num(), 1);
			const TSharedPtr<FJsonObject> Unknown = Call(TEXT("BindAction"), { S(BlueprintPath), S(ActionPath), S(TEXT("Started")), S(TEXT("Jmup")) });
			TestTrue(*FString::Printf(TEXT("an unknown function lists the callable ones (%s)"), *Codes(Unknown)), !IsOk(Unknown) && Codes(Unknown).Contains(TEXT("StopJumping")));
			const TSharedPtr<FJsonObject> Pin = Call(TEXT("BindAction"), { S(BlueprintPath), S(ActionPath), S(TEXT("Pressed")), S(TEXT("Jump")) });
			TestTrue(*FString::Printf(TEXT("an unknown trigger event lists them (%s)"), *Codes(Pin)), !IsOk(Pin) && Codes(Pin).Contains(TEXT("Completed")));
			TArray<UK2Node_EnhancedInputAction*> Events;
			FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
			if (!TestEqual(TEXT("one action event"), Events.Num(), 1))
				return;
			const UEdGraphPin* Started = Events[0]->FindPin(TEXT("Started"));
			const UK2Node_CallFunction* First = Started && Started->LinkedTo.Num() == 1 ? Cast<UK2Node_CallFunction>(Started->LinkedTo[0]->GetOwningNode()) : nullptr;
			TestTrue(*FString::Printf(TEXT("Started runs Jump (%s)"), *Links(Started)), First && First->FunctionReference.GetMemberName() == TEXT("Jump"));
			const UEdGraphPin* AfterJump = First ? First->FindPin(UEdGraphSchema_K2::PN_Then) : nullptr;
			const UK2Node_CallFunction* Second = AfterJump && AfterJump->LinkedTo.Num() == 1 ? Cast<UK2Node_CallFunction>(AfterJump->LinkedTo[0]->GetOwningNode()) : nullptr;
			TestTrue(*FString::Printf(TEXT("then UnCrouch (%s)"), *Links(AfterJump)), Second && Second->FunctionReference.GetMemberName() == TEXT("UnCrouch"));

			const FString Crouch = Fresh(TEXT("IA_Crouch"));
			TestTrue(TEXT("crouch action"), IsOk(Call(TEXT("CreateAction"), { S(Crouch), S(TEXT("Boolean")), S(TEXT("Pressed")) })));
			const TSharedPtr<FJsonObject> Toggle = Call(TEXT("BindActionToggle"), { S(BlueprintPath), S(Crouch), S(TEXT("Crouch")), S(TEXT("UnCrouch")) });
			TestTrue(*FString::Printf(TEXT("crouch toggles (%s)"), *Codes(Toggle)), IsOk(Toggle) && ResultRef(Toggle).StartsWith(TEXT("K2Node_MacroInstance#")));
			const TSharedPtr<FJsonObject> ToggleAgain = Call(TEXT("BindActionToggle"), { S(BlueprintPath), S(Crouch), S(TEXT("Crouch")), S(TEXT("UnCrouch")) });
			TestTrue(*FString::Printf(TEXT("a second toggle on the same pin is refused (%s)"), *Codes(ToggleAgain)), !IsOk(ToggleAgain) && Codes(ToggleAgain).Contains(TEXT("already runs")));

			const FString Throttle = Fresh(TEXT("IA_Throttle"));
			TestTrue(TEXT("axis action"), IsOk(Call(TEXT("CreateAction"), { S(Throttle), S(TEXT("Axis1D")) })));
			TestTrue(TEXT("Triggered -> AddMovementInput"), IsOk(Call(TEXT("BindAction"), { S(BlueprintPath), S(Throttle), S(TEXT("Triggered")), S(TEXT("AddMovementInput")) })));
			const TArray<UK2Node_CallFunction*> Moves = CallsOf(Blueprint, TEXT("AddMovementInput"));
			const UEdGraphPin* Scale = Moves.Num() == 1 ? Moves[0]->FindPin(TEXT("ScaleValue")) : nullptr;
			TestTrue(*FString::Printf(TEXT("the axis value feeds ScaleValue (%s)"), *Links(Scale)), Scale && Scale->LinkedTo.Num() == 1 && Scale->LinkedTo[0]->PinName == TEXT("ActionValue"));
			TestEqual(TEXT("compiles without errors"), CompileErrors(Blueprint), 0);
			// unreal.asset("@BP@") | select InputSetup
			const FString Setup = Codes(Run(TEXT(R"RQL({"rql":"0.1","source":{"call":"asset","on":{"path":["unreal"]},"args":[{"lit":"@BP@"}]},"stages":[{"op":"select","fields":[{"name":"InputSetup","expr":{"path":["InputSetup"]}}]}]})RQL"), false), 100000);
			TestTrue(*FString::Printf(TEXT("InputSetup lists the bindings (%s)"), *Setup), Setup.Contains(TEXT("StopJumping")) && Setup.Contains(TEXT("FlipFlop")) && Setup.Contains(TEXT("AddMovementInput.ScaleValue")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
