// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpEditorDiagnostics.h"
#include "UnrealMcpRqlSpecHelpers.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpLogCollector.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "Components/DirectionalLightComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Logging/MessageLog.h"
#include "UObject/Package.h"

/**
 * Editor diagnostics (docs/rql.md "Editor diagnostics"): the `diagnostics` source and the note a tool result carries when
 * new editor warnings appeared. Works on directional lights labelled "DiagSpecSun*", "DiagSpec" Message Log / log lines and
 * a /Temp Blueprint, and removes them afterwards.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpEditorDiagnosticsSpec, "UnrealMcp.Tools.Diagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	TUniquePtr<FUnrealMcpToolRegistry> Registry;
	TArray<TWeakObjectPtr<AActor>> Spawned;
	FString SavedMode;
	bool bCollectorWasRegistered = false;

	IConsoleVariable* PushVariable() const
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("UnrealMcp.DiagnosticsPush"));
	}

	void SetMode(const TCHAR* Mode)
	{
		if (IConsoleVariable* Variable = PushVariable())
			Variable->Set(Mode, ECVF_SetByCode);
	}

	/** A Standard tool that runs @p Body; read-only or not. */
	void AddTool(const FString& Name, bool bReadOnly, TFunction<FString()> Body)
	{
		Registry->Tool(Name)
			.Title(Name)
			.Description(TEXT("diagnostics spec tool"))
			.ReadOnlyHint(bReadOnly)
			.DestructiveHint(false)
			.Handle([Body](const FUnrealMcpToolCall&) { return FUnrealMcpToolResult::Success(Body ? Body() : FString(TEXT("ok"))); });
	}

	FUnrealMcpToolResult Run(const FString& Name)
	{
		return Registry->Execute(Name, FUnrealMcpToolCall(MakeShared<FJsonObject>()));
	}

	ADirectionalLight* SpawnSun(const FString& Label, int32 Priority)
	{
		UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
		ADirectionalLight* Sun = World ? World->SpawnActor<ADirectionalLight>() : nullptr;
		if (!Sun)
			return nullptr;
		Sun->SetActorLabel(Label);
		Cast<UDirectionalLightComponent>(Sun->GetLightComponent())->ForwardShadingPriority = Priority;
		Spawned.Add(Sun);
		return Sun;
	}

	/** diagnostics | where Message contains <Text> | select Source Severity Message, run as rql-query. */
	TSharedPtr<FJsonObject> Pull(const FString& Text, FUnrealMcpToolResult* OutResult = nullptr)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("query"), TEXT("diagnostics | where Message contains \"") + Text + TEXT("\" | select Source Severity Message"));
		Args->SetStringField(TEXT("plan"), TEXT(R"RQL({"rql":"0.1","source":{"path":["diagnostics"]},"stages":[{"op":"where","pred":{"op":"contains","args":[{"path":["Message"]},{"lit":")RQL")
			+ Text + TEXT(R"RQL("}]}},{"op":"select","fields":[{"name":"Source","expr":{"path":["Source"]}},{"name":"Severity","expr":{"path":["Severity"]}},{"name":"Message","expr":{"path":["Message"]}}]}]})RQL"));
		const FUnrealMcpToolResult Result = Registry->Execute(TEXT("rql-query"), FUnrealMcpToolCall(Args));
		if (OutResult)
			*OutResult = Result;
		return UnrealMcpRqlSpecHelpers::Parse(Result.Message);
	}

END_DEFINE_SPEC(FUnrealMcpEditorDiagnosticsSpec)

void FUnrealMcpEditorDiagnosticsSpec::Define()
{
	using namespace UnrealMcpRqlSpecHelpers;

	BeforeEach([this]()
	{
		Registry = MakeUnique<FUnrealMcpToolRegistry>();
		UnrealMcpRqlTools::Register(*Registry);
		AddTool(TEXT("diag-spec-write"), /*bReadOnly*/ false, nullptr);
		AddTool(TEXT("diag-spec-read"), /*bReadOnly*/ true, nullptr);
		SavedMode = PushVariable() ? PushVariable()->GetString() : FString();
		SetMode(TEXT("all"));
		FUnrealMcpLogCollector& Collector = FUnrealMcpLogCollector::Get();
		bCollectorWasRegistered = Collector.IsRegistered();
		Collector.Startup();
		FUnrealMcpEditorDiagnostics::Get().TakeNewBlock(FString()); // start from "seen everything"
	});

	AfterEach([this]()
	{
		for (const TWeakObjectPtr<AActor>& Actor : Spawned)
		{
			if (Actor.IsValid())
				Actor->GetWorld()->EditorDestroyActor(Actor.Get(), /*bShouldModifyLevel*/ false);
		}
		Spawned.Reset();
		Registry.Reset();
		if (!SavedMode.IsEmpty())
			SetMode(*SavedMode);
		if (!bCollectorWasRegistered)
			FUnrealMcpLogCollector::Get().Shutdown();
	});

	It("registers the push switch with the default 'all'", [this]()
	{
		TestNotNull(TEXT("UnrealMcp.DiagnosticsPush exists"), PushVariable());
	});

	It("reports competing directional lights once, after the call that adds the second, and lists them until fixed", [this]()
	{
		// Priority 1000: above any directional light the open level already has, so these two are the competing ones.
		AddTool(TEXT("diag-spec-suns"), /*bReadOnly*/ false, [this]()
		{
			SpawnSun(TEXT("DiagSpecSun_A"), 1000);
			SpawnSun(TEXT("DiagSpecSun_B"), 1000);
			return FString(TEXT("spawned"));
		});
		const FUnrealMcpToolResult Added = Run(TEXT("diag-spec-suns"));
		TestTrue(*FString::Printf(TEXT("the result names the conflict (%s)"), *Added.Notes),
			Added.Notes.Contains(TEXT("Multiple directional lights are competing")) && Added.Notes.Contains(TEXT("DiagSpecSun_A, DiagSpecSun_B")));
		TestTrue(TEXT("it says Warning [Viewport]"), Added.Notes.Contains(TEXT("- Warning [Viewport] Multiple directional lights")));
		TestTrue(TEXT("it names the query that lists everything"), Added.Notes.Contains(FUnrealMcpEditorDiagnostics::ListQuery()));
		TestEqual(TEXT("the tool's own text is untouched"), Added.Message, FString(TEXT("spawned")));

		const FUnrealMcpToolResult Next = Run(TEXT("diag-spec-write"));
		TestFalse(*FString::Printf(TEXT("not repeated on the next call (%s)"), *Next.Notes), Next.Notes.Contains(TEXT("DiagSpecSun")));

		FUnrealMcpToolResult PullResult;
		const TSharedPtr<FJsonObject> Listed = Pull(TEXT("DiagSpecSun"), &PullResult);
		TestTrue(*FString::Printf(TEXT("the source lists it (%s)"), *PullResult.Message), IsOk(Listed) && RowCount(Listed) == 1);
		TestEqual(TEXT("Source"), CellText(Listed, 0, 0), FString(TEXT("Viewport")));
		TestEqual(TEXT("Severity"), CellText(Listed, 0, 1), FString(TEXT("Warning")));

		Cast<UDirectionalLightComponent>(CastChecked<ADirectionalLight>(Spawned[1].Get())->GetLightComponent())->ForwardShadingPriority = 1001;
		TestEqual(TEXT("gone once one light has the higher priority"), RowCount(Pull(TEXT("DiagSpecSun"))), 0);
		const FUnrealMcpToolResult Fixed = Run(TEXT("diag-spec-write"));
		TestFalse(TEXT("nothing new to report after the fix"), Fixed.Notes.Contains(TEXT("DiagSpecSun")));
	});

	It("does not repeat in the result what the rql-query that read the source returns", [this]()
	{
		{
			FMessageLog MapCheck(TEXT("MapCheck"));
			MapCheck.Warning(FText::FromString(TEXT("DiagSpec read-before-push warning")));
		}
		FUnrealMcpToolResult PullResult;
		const TSharedPtr<FJsonObject> Listed = Pull(TEXT("DiagSpec read-before-push"), &PullResult);
		TestTrue(TEXT("listed with its Message Log page"), RowCount(Listed) == 1 && CellText(Listed, 0, 0) == TEXT("MessageLog:MapCheck"));
		TestFalse(*FString::Printf(TEXT("no note for what the query read (%s)"), *PullResult.Notes), PullResult.Notes.Contains(TEXT("DiagSpec")));
	});

	It("follows the push switch: mutating skips read-only tools, off skips every tool", [this]()
	{
		SetMode(TEXT("mutating"));
		UE_LOG(LogTemp, Warning, TEXT("DiagSpec mutating-mode warning"));
		TestFalse(TEXT("a read-only tool carries no note"), Run(TEXT("diag-spec-read")).Notes.Contains(TEXT("DiagSpec mutating-mode")));
		const FUnrealMcpToolResult Write = Run(TEXT("diag-spec-write"));
		TestTrue(*FString::Printf(TEXT("a writing tool does (%s)"), *Write.Notes),
			Write.Notes.Contains(TEXT("- Warning [Log:LogTemp] DiagSpec mutating-mode warning")));

		SetMode(TEXT("off"));
		// An Error UE_LOG in a test goes to the automation framework, not to GLog: feed the collector as GLog would.
		FUnrealMcpLogCollector::Get().Serialize(TEXT("DiagSpec off-mode error"), ELogVerbosity::Error, TEXT("LogTemp"));
		TestTrue(TEXT("off: no note"), Run(TEXT("diag-spec-write")).Notes.IsEmpty());
		TestEqual(TEXT("off: the source still lists it"), RowCount(Pull(TEXT("DiagSpec off-mode"))), 1);
	});

	It("caps the note, puts errors first, counts repeats and leaves out log lines the tool already returned", [this]()
	{
		for (int32 Index = 0; Index < 14; ++Index)
			UE_LOG(LogTemp, Warning, TEXT("DiagSpec flood warning %d"), Index);
		// An Error UE_LOG in a test goes to the automation framework, not to GLog: feed the collector as GLog would.
		FUnrealMcpLogCollector::Get().Serialize(TEXT("DiagSpec flood error"), ELogVerbosity::Error, TEXT("LogTemp"));
		const FUnrealMcpToolResult Flood = Run(TEXT("diag-spec-write"));
		TArray<FString> Lines;
		Flood.Notes.ParseIntoArrayLines(Lines);
		int32 Items = 0;
		for (const FString& Line : Lines)
			Items += Line.StartsWith(TEXT("- ")) ? 1 : 0;
		TestEqual(*FString::Printf(TEXT("at most %d lines (%s)"), FUnrealMcpEditorDiagnostics::MaxPushLines, *Flood.Notes), Items, FUnrealMcpEditorDiagnostics::MaxPushLines);
		TestTrue(TEXT("the rest is counted"), Flood.Notes.Contains(TEXT(" more)")));
		TestTrue(TEXT("the error comes first"), Lines.Num() > 1 && Lines[1].Contains(TEXT("DiagSpec flood error")));

		UE_LOG(LogTemp, Warning, TEXT("DiagSpec flood warning 0"));
		TestFalse(TEXT("a repeated line is not reported again"), Run(TEXT("diag-spec-write")).Notes.Contains(TEXT("DiagSpec flood")));
		TestEqual(TEXT("still one row"), RowCount(Pull(TEXT("DiagSpec flood warning 0"))), 1);

		AddTool(TEXT("diag-spec-echo"), /*bReadOnly*/ false, []()
		{
			UE_LOG(LogTemp, Warning, TEXT("DiagSpec echoed warning"));
			return FString(TEXT("log: DiagSpec echoed warning"));
		});
		TestFalse(TEXT("a line the tool returned is not repeated"), Run(TEXT("diag-spec-echo")).Notes.Contains(TEXT("DiagSpec echoed")));
	});

	It("reports a Blueprint that fails to compile while it does", [this]()
	{
		UPackage* Package = CreatePackage(TEXT("/Temp/DiagSpec_BrokenBlueprint"));
		UBlueprint* Blueprint = NewObject<UBlueprint>(Package, TEXT("DiagSpec_BrokenBlueprint"), RF_Public | RF_Standalone);
		Blueprint->Status = BS_Error;
		const FUnrealMcpToolResult Broken = Run(TEXT("diag-spec-write"));
		TestTrue(*FString::Printf(TEXT("Error [Compiler] (%s)"), *Broken.Notes),
			Broken.Notes.Contains(TEXT("- Error [Compiler] Blueprint /Temp/DiagSpec_BrokenBlueprint.DiagSpec_BrokenBlueprint fails to compile")));
		Blueprint->Status = BS_UpToDate;
		TestEqual(TEXT("gone once it compiles"), RowCount(Pull(TEXT("DiagSpec_BrokenBlueprint"))), 0);
		DropObject(Blueprint);
	});

	It("stays cheap on the game thread", [this]()
	{
		FUnrealMcpEditorDiagnostics& Diagnostics = FUnrealMcpEditorDiagnostics::Get();
		const int32 Runs = 50;
		const double Start = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Runs; ++Index)
			Diagnostics.TakeNewBlock(FString());
		const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0 / Runs;
		AddInfo(FString::Printf(TEXT("TakeNewBlock: %.3f ms per call (%d diagnostics)"), Milliseconds, Diagnostics.Snapshot().Num()));
		TestTrue(*FString::Printf(TEXT("under 20 ms per call (%.3f)"), Milliseconds), Milliseconds < 20.0);
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
