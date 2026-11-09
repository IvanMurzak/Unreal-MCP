// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpEditorDiagnostics.h"

#include "Tools/UnrealMcpLogCollector.h"
#include "Tools/UnrealMcpObjectRef.h"
#include "UnrealMcpToolRegistry.h"

#include "Components/DirectionalLightComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "IMessageLogListing.h"
#include "Interfaces/IPluginManager.h"
#include "Logging/TokenizedMessage.h"
#include "MessageLogModule.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace UnrealMcpEditorDiagnosticsLocal
{
	const TCHAR* const SevError = TEXT("Error");
	const TCHAR* const SevWarning = TEXT("Warning");
	const TCHAR* const SevInfo = TEXT("Info");
	const TCHAR* const Viewport = TEXT("Viewport");
	const TCHAR* const LogPrefix = TEXT("Log:");

	constexpr int32 MaxLineChars = 400;
	constexpr int32 MaxLogItems = 200;        // distinct log lines kept, the most recent
	constexpr int32 MaxMessageLogItems = 100; // per Message Log page, the newest
	constexpr int32 MaxMessageChars = 2000;

	/** The Message Log pages read, by their listing names (only the registered ones are read). */
	const TCHAR* const MessageLogPages[] = {
		TEXT("MapCheck"), TEXT("PIE"), TEXT("BlueprintLog"), TEXT("LoadErrors"), TEXT("AssetCheck"),
		TEXT("EditorErrors"), TEXT("LightingResults"), TEXT("AssetTools"), TEXT("PackagingResults"), TEXT("HLODResults"),
	};

	/** A Message Log page mirrors its messages into the log under its own name: those lines are already MessageLog rows. */
	bool IsMessageLogPage(const FName& Category)
	{
		for (const TCHAR* Page : MessageLogPages)
		{
			if (Category == Page)
				return true;
		}
		return false;
	}

	FAutoConsoleVariable* GPushVariable = nullptr;

	FString Clean(const FString& Text, int32 MaxChars)
	{
		FString Out = Text.Replace(TEXT("\r\n"), TEXT("\n")).TrimStartAndEnd();
		if (Out.Len() > MaxChars)
			Out = Out.Left(MaxChars - 3) + TEXT("...");
		return Out;
	}

	/** Whether @p Text shows @p Message as is or JSON-escaped (an rql-* answer is QueryResult JSON). */
	bool Shows(const FString& Text, const FString& Message)
	{
		if (Text.IsEmpty())
			return false;
		if (Text.Contains(Message, ESearchCase::CaseSensitive))
			return true;
		const FString Escaped = Message.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""))
			.Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\t"), TEXT("\\t"));
		return Escaped != Message && Text.Contains(Escaped, ESearchCase::CaseSensitive);
	}

	bool IsLog(const FString& Source)
	{
		return Source.StartsWith(LogPrefix);
	}

	/** Push order: errors before warnings, then the scene before compile, Message Log and log lines, then age. */
	int32 SourceRank(const FString& Source)
	{
		if (Source == Viewport)
			return 0;
		if (Source == TEXT("Compiler"))
			return 1;
		return IsLog(Source) ? 3 : 2;
	}

	EUnrealMcpDiagnosticsPush ParsePushMode(FString Text)
	{
		Text = Text.TrimStartAndEnd().ToLower();
		if (Text == TEXT("off") || Text == TEXT("0") || Text == TEXT("false") || Text == TEXT("none"))
			return EUnrealMcpDiagnosticsPush::Off;
		if (Text == TEXT("mutating") || Text == TEXT("writes"))
			return EUnrealMcpDiagnosticsPush::Mutating;
		return EUnrealMcpDiagnosticsPush::All;
	}

	EUnrealMcpDiagnosticsPush PushMode()
	{
		return GPushVariable ? ParsePushMode((*GPushVariable)->GetString()) : EUnrealMcpDiagnosticsPush::All;
	}
}

using namespace UnrealMcpEditorDiagnosticsLocal;

const TCHAR* FUnrealMcpEditorDiagnostics::ListQuery()
{
	return TEXT("diagnostics | where Severity != \"Info\" | select Source Severity Message Count");
}

FUnrealMcpEditorDiagnostics& FUnrealMcpEditorDiagnostics::Get()
{
	static FUnrealMcpEditorDiagnostics Instance;
	return Instance;
}

void FUnrealMcpEditorDiagnostics::GatherViewport(TArray<FGathered>& Out) const
{
	// What the viewport's on-screen message delegate holds (world partition, navigation, Nanite, virtual shadow maps,
	// GPU scene, ...). The engine broadcasts it on the game thread when it draws the viewport; so do we.
	if (FCoreDelegates::OnGetOnScreenMessages.IsBound())
	{
		FCoreDelegates::FSeverityMessageMap Messages;
		FCoreDelegates::OnGetOnScreenMessages.Broadcast(Messages);
		for (const TPair<FCoreDelegates::EOnScreenMessageSeverity, FText>& Pair : Messages)
		{
			const TCHAR* Severity = Pair.Key == FCoreDelegates::EOnScreenMessageSeverity::Error ? SevError
				: Pair.Key == FCoreDelegates::EOnScreenMessageSeverity::Warning ? SevWarning : SevInfo;
			Out.Add({ Viewport, Severity, Pair.Value.ToString() });
		}
	}

	UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
	if (!World)
		return;

	// The renderer draws this one onto the canvas only (LightGridInjection.cpp, FSceneRenderer::OnGetOnScreenMessages):
	// nothing outside the renderer can read it, so check what it checks. Two or more visible directional lights that
	// share the highest ForwardShadingPriority compete for forward shading.
	TArray<UDirectionalLightComponent*> Lights;
	int32 TopPriority = MIN_int32;
	for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
	{
		UDirectionalLightComponent* Light = *It;
		// The lights the renderer has (ULightComponent::CreateRenderState_Concurrent adds these to the scene).
		if (!IsValid(Light) || Light->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Light->GetWorld() != World
			|| !Light->IsRegistered() || !Light->bAffectsWorld || !Light->ShouldComponentAddToScene() || !Light->ShouldRender()
			|| Light->Intensity <= 0.f)
			continue;
		if (Light->ForwardShadingPriority > TopPriority)
		{
			TopPriority = Light->ForwardShadingPriority;
			Lights.Reset();
		}
		if (Light->ForwardShadingPriority == TopPriority)
			Lights.Add(Light);
	}
	if (Lights.Num() >= 2)
	{
		TArray<FString> Names;
		for (const UDirectionalLightComponent* Light : Lights)
		{
			const AActor* Owner = Light->GetOwner();
			Names.Add(Owner ? Owner->GetActorLabel() : Light->GetName());
		}
		Names.Sort();
		Out.Add({ Viewport, SevWarning, FString::Printf(TEXT("Multiple directional lights are competing to be the single one used for forward "
			"shading, translucent, water or volumetric fog. Please adjust their ForwardShadingPriority. As a fallback, the main directional "
			"light will be selected based on overall brightness. (lights: %s; ForwardShadingPriority %d)"),
			*FString::Join(Names, TEXT(", ")), TopPriority) });
	}

	// The engine's map warnings (DrawMapWarnings in UnrealEngine.cpp), from the world state it reads.
	if (World->bKismetScriptError)
		Out.Add({ Viewport, SevError, TEXT("BLUEPRINT COMPILE ERROR") });
	if (World->NumLightingUnbuiltObjects > 0)
		Out.Add({ Viewport, SevInfo, FString::Printf(TEXT("LIGHTING NEEDS TO BE REBUILT (%d unbuilt objects)"), World->NumLightingUnbuiltObjects) });
	if (World->NumUnbuiltReflectionCaptures > 0)
		Out.Add({ Viewport, SevInfo, FString::Printf(TEXT("REFLECTION CAPTURES NEED TO BE REBUILT (%d unbuilt)"), World->NumUnbuiltReflectionCaptures) });
}

void FUnrealMcpEditorDiagnostics::GatherCompiler(TArray<FGathered>& Out) const
{
	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		const UBlueprint* Blueprint = *It;
		if (!IsValid(Blueprint) || Blueprint->HasAnyFlags(RF_ClassDefaultObject | RF_Transient))
			continue;
		const bool bError = Blueprint->Status == BS_Error;
		if (!bError && Blueprint->Status != BS_UpToDateWithWarnings)
			continue;
		const UPackage* Package = Blueprint->GetOutermost();
		if (!Package || Package == GetTransientPackage() || Package->GetName().StartsWith(TEXT("/Engine/")))
			continue;
		// Engine plugins' content (/Niagara, /ControlRig, ...) is not the project's to fix.
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPluginFromPath(Package->GetName());
		if (Plugin.IsValid() && Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine)
			continue;
		Out.Add({ TEXT("Compiler"), bError ? SevError : SevWarning, FString::Printf(TEXT("Blueprint %s %s (its messages: unreal.asset(\"%s\") | call Compile())"),
			*Blueprint->GetPathName(), bError ? TEXT("fails to compile") : TEXT("compiles with warnings"), *Package->GetName()) });
	}
}

void FUnrealMcpEditorDiagnostics::GatherMessageLogs(TArray<FGathered>& Out) const
{
	FMessageLogModule* Module = FModuleManager::GetModulePtr<FMessageLogModule>(TEXT("MessageLog"));
	if (!Module)
		return;
	for (const TCHAR* Page : MessageLogPages)
	{
		if (!Module->IsRegisteredLogListing(Page))
			continue;
		const TArray<TSharedRef<FTokenizedMessage>>& Messages = Module->GetLogListing(Page)->GetFilteredMessages();
		const FString Source = FString(TEXT("MessageLog:")) + Page;
		for (int32 Index = FMath::Max(0, Messages.Num() - MaxMessageLogItems); Index < Messages.Num(); ++Index)
		{
			const EMessageSeverity::Type Severity = Messages[Index]->GetSeverity();
			if (Severity == EMessageSeverity::Info)
				continue;
			Out.Add({ Source, Severity == EMessageSeverity::Error ? SevError : SevWarning, Messages[Index]->ToText().ToString() });
		}
	}
}

FUnrealMcpEditorDiagnostics::FRow& FUnrealMcpEditorDiagnostics::Upsert(const FGathered& Item, const FDateTime& Now, bool& bOutAdded)
{
	const FString Key = Item.Source + TEXT("\n") + Item.Severity + TEXT("\n") + Item.Message;
	FRow* Existing = Items.Find(Key);
	bOutAdded = Existing == nullptr;
	if (Existing)
	{
		Existing->LastSeen = Now;
		return *Existing;
	}
	FRow& Added = Items.Add(Key);
	Added.Sequence = ++LastSequence;
	Added.Source = Item.Source;
	Added.Severity = Item.Severity;
	Added.Message = Item.Message;
	Added.FirstSeen = Now;
	Added.LastSeen = Now;
	return Added;
}

void FUnrealMcpEditorDiagnostics::IngestLog(const FDateTime& Now)
{
	FUnrealMcpLogCollector& Collector = FUnrealMcpLogCollector::Get();
	if (Collector.GetLastSequence() <= LogCursor)
		return;
	for (const FUnrealMcpLogEntry& Entry : Collector.SnapshotAfter(LogCursor))
	{
		LogCursor = FMath::Max(LogCursor, Entry.Sequence);
		const ELogVerbosity::Type Verbosity = static_cast<ELogVerbosity::Type>(Entry.Verbosity & ELogVerbosity::VerbosityMask);
		if (Verbosity > ELogVerbosity::Warning || Verbosity == ELogVerbosity::NoLogging || Entry.Category == TEXT("LogUnrealMcp")
			|| IsMessageLogPage(Entry.Category))
			continue;
		bool bAdded = false;
		FRow& Row = Upsert({ LogPrefix + Entry.Category.ToString(), Verbosity == ELogVerbosity::Warning ? SevWarning : SevError,
			Clean(Entry.Message, MaxMessageChars) }, Now, bAdded);
		if (bAdded)
			++LogItems;
		else
			++Row.Count;
	}

	// Keep the most recent MaxLogItems distinct log lines.
	if (LogItems > MaxLogItems)
	{
		// Oldest first: by LastSeen, then Sequence (the lines of one refresh share LastSeen).
		TArray<TTuple<FDateTime, int64, FString>> Logs;
		for (const TPair<FString, FRow>& Pair : Items)
		{
			if (IsLog(Pair.Value.Source))
				Logs.Emplace(Pair.Value.LastSeen, Pair.Value.Sequence, Pair.Key);
		}
		Logs.Sort([](const TTuple<FDateTime, int64, FString>& A, const TTuple<FDateTime, int64, FString>& B)
		{
			return A.Get<0>() != B.Get<0>() ? A.Get<0>() < B.Get<0>() : A.Get<1>() < B.Get<1>();
		});
		for (int32 Index = 0; Index < Logs.Num() - MaxLogItems; ++Index)
			Items.Remove(Logs[Index].Get<2>());
		LogItems = MaxLogItems;
	}
}

void FUnrealMcpEditorDiagnostics::Refresh()
{
	check(IsInGameThread());
	const FDateTime Now = FDateTime::UtcNow();

	TArray<FGathered> Current;
	GatherViewport(Current);
	GatherCompiler(Current);
	GatherMessageLogs(Current);

	// State rows: keep the ones still there, add the new ones, drop the ones that are gone (a return is new again).
	TSet<int64> Present; // by Sequence: unique per row, and stable while rows are added
	for (FGathered& Item : Current)
	{
		Item.Message = Clean(Item.Message, MaxMessageChars);
		bool bAdded = false;
		Present.Add(Upsert(Item, Now, bAdded).Sequence);
	}
	for (auto It = Items.CreateIterator(); It; ++It)
	{
		if (!IsLog(It.Value().Source) && !Present.Contains(It.Value().Sequence))
			It.RemoveCurrent();
	}

	IngestLog(Now);
}

TArray<FUnrealMcpDiagnostic> FUnrealMcpEditorDiagnostics::Snapshot() const
{
	TArray<FUnrealMcpDiagnostic> Out;
	for (const TPair<FString, FRow>& Pair : Items)
		Out.Add(Pair.Value);
	Out.Sort([](const FUnrealMcpDiagnostic& A, const FUnrealMcpDiagnostic& B) { return A.Sequence < B.Sequence; });
	return Out;
}

FString FUnrealMcpEditorDiagnostics::TakeNewBlock(const FString& AlreadyShown, bool bReport)
{
	Refresh();
	TArray<const FRow*> New;
	for (TPair<FString, FRow>& Pair : Items)
	{
		FRow& Row = Pair.Value;
		if (Row.bSeen || Row.Severity == SevInfo)
			continue;
		if (Shows(AlreadyShown, Row.Message))
			Row.bSeen = true;
		else if (bReport)
		{
			Row.bSeen = true;
			New.Add(&Row);
		}
	}
	if (New.Num() == 0)
		return FString();

	New.Sort([](const FRow& A, const FRow& B)
	{
		if (A.Severity != B.Severity)
			return A.Severity == SevError;
		const int32 RankA = SourceRank(A.Source), RankB = SourceRank(B.Source);
		return RankA != RankB ? RankA < RankB : A.Sequence < B.Sequence;
	});
	FString Block = FString::Printf(TEXT("Editor diagnostics (new since your last call; fix them or tell the user, and check the list before you "
		"call the work done: rql-query %s):"), ListQuery());
	for (int32 Index = 0; Index < FMath::Min(New.Num(), MaxPushLines); ++Index)
	{
		const FRow& Item = *New[Index];
		Block += FString::Printf(TEXT("\n- %s [%s] %s"), *Item.Severity, *Item.Source,
			*Clean(Item.Message.Replace(TEXT("\n"), TEXT(" ")), MaxLineChars));
		if (Item.Count > 1)
			Block += FString::Printf(TEXT(" (x%d)"), Item.Count);
	}
	if (New.Num() > MaxPushLines)
		Block += FString::Printf(TEXT("\n(+%d more)"), New.Num() - MaxPushLines);
	return Block;
}

void FUnrealMcpEditorDiagnostics::Install()
{
	if (!GPushVariable)
	{
		const FString FromEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("UNREAL_MCP_DIAGNOSTICS_PUSH"));
		GPushVariable = new FAutoConsoleVariable(TEXT("UnrealMcp.DiagnosticsPush"), FromEnv.IsEmpty() ? TEXT("all") : *FromEnv,
			TEXT("When an Unreal-MCP tool result lists the editor warnings / errors that are new since the agent's last call: "
				"all (every tool, the default), mutating (tools that are not read-only) or off. Startup value: UNREAL_MCP_DIAGNOSTICS_PUSH."));
	}
	// Log rows start once the editor has started: the warnings of the engine's own startup (missing install files, DDC
	// fallbacks) are not about the project, and the Message Log pages (LoadErrors) keep what is.
	auto SkipStartupLog = []() { Get().LogCursor = FMath::Max(Get().LogCursor, FUnrealMcpLogCollector::Get().GetLastSequence()); };
	if (GIsRunning)
		SkipStartupLog();
	else
		FCoreDelegates::OnFEngineLoopInitComplete.AddLambda(SkipStartupLog);
	FUnrealMcpToolRegistry::SetResultAnnotator([](const FUnrealMcpRegisteredTool& Tool, FUnrealMcpToolResult& Result)
	{
		const EUnrealMcpDiagnosticsPush Mode = PushMode();
		if (Mode == EUnrealMcpDiagnosticsPush::Off)
			return;
		// What the tool's own text shows (log lines it returned, the rows of a diagnostics query) is not repeated, and
		// counts as seen even on a call that gets no note (a read-only tool in mutating mode).
		const bool bReport = Mode == EUnrealMcpDiagnosticsPush::All || !Tool.bReadOnlyHint;
		const FString Block = Get().TakeNewBlock(Result.Message, bReport);
		if (!Block.IsEmpty())
			Result.Notes = Result.Notes.IsEmpty() ? Block : Result.Notes + TEXT("\n") + Block;
	});
}

void FUnrealMcpEditorDiagnostics::Uninstall()
{
	FUnrealMcpToolRegistry::SetResultAnnotator(nullptr);
	delete GPushVariable;
	GPushVariable = nullptr;
}
