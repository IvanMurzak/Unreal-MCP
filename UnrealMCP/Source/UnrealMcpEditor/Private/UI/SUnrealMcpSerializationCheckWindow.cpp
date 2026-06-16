// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpSerializationCheckWindow.h"
#include "UI/SUnrealMcpAgentWidgets.h"
#include "UI/FUnrealMcpStyle.h"
#include "Tools/UnrealMcpPropertyJson.h"
#include "Tools/UnrealMcpObjectRef.h"
#include "UnrealMcpLog.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "HAL/PlatformApplicationMisc.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/PrettyJsonPrintPolicy.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

using UnrealMcpAgentWidgets::TemplateFoldout;
using UnrealMcpAgentWidgets::TemplateLabelDescription;

namespace
{
	// Unity-build ODR rule (CLAUDE.md): every file-local helper carries the family-unique `SerCheck` prefix
	// so a same-name/same-signature helper in another unity-grouped .cpp cannot collide.

	// Pretty-print a JSON object the way the Unity reference does (reflector .ToPrettyJson()). The tool
	// families return condensed JSON over the wire; this window is for a HUMAN to read, so it pretty-prints.
	FString SerCheckPrettyPrint(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	// The first selected actor (preferred), else the first selected non-actor object — the "use current
	// editor selection" affordance the Unity ObjectField gives implicitly. Returns null when nothing is
	// selected or there is no editor.
	UObject* SerCheckCurrentSelection()
	{
		if (!GEditor)
			return nullptr;

		if (USelection* ActorSelection = GEditor->GetSelectedActors())
		{
			TArray<AActor*> Actors;
			ActorSelection->GetSelectedObjects<AActor>(Actors);
			if (Actors.Num() > 0 && Actors[0])
				return Actors[0];
		}
		if (USelection* ObjectSelection = GEditor->GetSelectedObjects())
		{
			TArray<UObject*> Objects;
			ObjectSelection->GetSelectedObjects<UObject>(Objects);
			if (Objects.Num() > 0 && Objects[0])
				return Objects[0];
		}
		return nullptr;
	}

	// A short, label-able ref for an object: an actor uses its editor label (what ResolveObject matches first
	// via the world); everything else uses its object path. Keeps the "Use selection" round-trip stable.
	FString SerCheckRefForObject(UObject* Object)
	{
		if (const AActor* Actor = Cast<AActor>(Object))
			return Actor->GetActorLabel();
		return Object ? Object->GetPathName() : FString();
	}
}

TSharedRef<FJsonObject> SUnrealMcpSerializationCheckWindow::ShallowFilter(const TSharedPtr<FJsonObject>& Source)
{
	// Keep scalars (string/number/bool/null), drop nested objects + arrays — the "Recursive OFF" shallow view.
	TSharedRef<FJsonObject> Shallow = MakeShared<FJsonObject>();
	if (!Source.IsValid())
		return Shallow;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Source->Values)
	{
		const EJson Type = Field.Value.IsValid() ? Field.Value->Type : EJson::Null;
		if (Type != EJson::Object && Type != EJson::Array)
			Shallow->SetField(Field.Key, Field.Value);
	}
	return Shallow;
}

void SUnrealMcpSerializationCheckWindow::Construct(const FArguments& InArgs)
{
	OutputHeader = LOCTEXT("OutputHeaderEmpty", "Output");
	CopyLabel = LOCTEXT("Copy", "Copy");

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(12.0f)
		[
			SNew(SVerticalBox)
			// 1. Information foldout (collapsed help text).
			+ SVerticalBox::Slot().AutoHeight()[ BuildInformationFoldout() ]
			// 2. Separator.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ SNew(SSeparator) ]
			// 3. Input row: target ref + Use selection + Recursive + Serialize.
			+ SVerticalBox::Slot().AutoHeight()[ BuildInputRow() ]
			// 4. Separator.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[ SNew(SSeparator) ]
			// 5/6/7. Output header + scrollable JSON + Copy.
			+ SVerticalBox::Slot().FillHeight(1.0f)[ BuildOutputSection() ]
		]
	];
}

TSharedRef<SWidget> SUnrealMcpSerializationCheckWindow::BuildInformationFoldout()
{
	return TemplateFoldout(
		LOCTEXT("InfoHeading", "Information"),
		TemplateLabelDescription(LOCTEXT("InfoBody",
			"Pick a target by its actor label or object path (or press \"Use selection\" to fill it from the "
			"current editor selection), then press Serialize. The object is serialized to JSON in-process via "
			"the same reflection-based converter the object-get-data / actor-get-data MCP tools use — no sidecar "
			"round-trip. Recursive ON emits the full reflected property graph; OFF emits only the object's "
			"identity and top-level fields. Copy puts the JSON on the clipboard.")),
		/*bInitiallyExpanded*/ false);
}

TSharedRef<SWidget> SUnrealMcpSerializationCheckWindow::BuildInputRow()
{
	return SNew(SVerticalBox)
		// Target ref + Use selection.
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(60.0f)
				[
					SNew(STextBlock)
					.TextStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FTextBlockStyle>("UnrealMcp.Text.Description"))
					.Text(LOCTEXT("TargetLabel", "Target"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SAssignNew(TargetField, SEditableTextBox)
				.HintText(LOCTEXT("TargetHint", "Actor label or object path (e.g. /Game/Maps/Demo.Demo:PersistentLevel.Cube)"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
			[
				UnrealMcpStyleWidgets::CompactTextButton(LOCTEXT("UseSelection", "Use selection"),
					FOnClicked::CreateSP(this, &SUnrealMcpSerializationCheckWindow::OnUseSelectionClicked))
			]
		]
		// Recursive + Serialize.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SAssignNew(RecursiveToggle, SCheckBox)
				.IsChecked(ECheckBoxState::Checked)
				[
					SNew(STextBlock).Margin(FMargin(4, 0, 0, 0)).Text(LOCTEXT("Recursive", "Recursive"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				UnrealMcpStyleWidgets::StyledTextButton("UnrealMcp.Button.Primary", LOCTEXT("Serialize", "Serialize"),
					FOnClicked::CreateSP(this, &SUnrealMcpSerializationCheckWindow::OnSerializeClicked))
			]
		];
}

TSharedRef<SWidget> SUnrealMcpSerializationCheckWindow::BuildOutputSection()
{
	return SNew(SVerticalBox)
		// Output header ("Output" / "Output (NN ms)").
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
			.Font(FUnrealMcpStyle::SubHeaderFont())
			.Text_Lambda([this]() { return OutputHeader; })
		]
		// Scrollable monospace JSON output in a dark rounded panel.
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FUnrealMcpStyle::Get().GetBrush("UnrealMcp.Input"))
			.Padding(8.0f)
			[
				SAssignNew(OutputBox, SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AllowMultiLine(true)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.Text(FText::GetEmpty())
			]
		]
		// Copy button.
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 6, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(&FUnrealMcpStyle::Get().GetWidgetStyle<FButtonStyle>("UnrealMcp.Button.Secondary"))
			.HAlign(HAlign_Center).VAlign(VAlign_Center)
			.OnClicked(FOnClicked::CreateSP(this, &SUnrealMcpSerializationCheckWindow::OnCopyClicked))
			[
				SNew(STextBlock).Text_Lambda([this]() { return CopyLabel; })
			]
		];
}

FReply SUnrealMcpSerializationCheckWindow::OnUseSelectionClicked()
{
	if (UObject* Selected = SerCheckCurrentSelection())
	{
		if (TargetField.IsValid())
			TargetField->SetText(FText::FromString(SerCheckRefForObject(Selected)));
	}
	return FReply::Handled();
}

FReply SUnrealMcpSerializationCheckWindow::OnSerializeClicked()
{
	const FString Ref = TargetField.IsValid() ? TargetField->GetText().ToString().TrimStartAndEnd() : FString();
	const bool bRecursive = RecursiveToggle.IsValid() && RecursiveToggle->IsChecked();

	if (Ref.IsEmpty())
	{
		OutputHeader = LOCTEXT("OutputHeaderEmpty", "Output");
		SetOutput(LOCTEXT("ErrNoTarget", "Error: enter an actor label or object path (or press \"Use selection\").").ToString());
		return FReply::Handled();
	}

	// Resolve via the SAME ref rule the tool families use (path or actor label against the editor world).
	UObject* Target = FUnrealMcpObjectRef::ResolveObject(Ref, FUnrealMcpObjectRef::GetEditorWorld());
	if (!Target)
	{
		OutputHeader = LOCTEXT("OutputHeaderEmpty", "Output");
		SetOutput(FString::Printf(TEXT("Error: could not resolve target '%s' (not a known actor label or object path)."), *Ref));
		return FReply::Handled();
	}

	const double StartSeconds = FPlatformTime::Seconds();

	// In-process serialization (§3.2) via the converter the object-get-data / actor-get-data tools use.
	// Recursive OFF: only the object's identity + top-level scalar fields are wanted, so request the
	// non-object leaves. SerializeObject has no `recursive` arg (FJsonObjectConverter always walks the
	// reflected graph), so the shallow variant prunes nested object fields AFTER serialization — matching
	// Unity's recursive=false "shallow" intent without forking the serialization path.
	TSharedPtr<FJsonObject> Json = FUnrealMcpPropertyJson::SerializeObject(Target, /*Paths*/ {});
	if (!Json.IsValid())
		Json = MakeShared<FJsonObject>();

	if (!bRecursive)
		Json = ShallowFilter(Json);

	const FString Pretty = SerCheckPrettyPrint(Json.ToSharedRef());
	const int32 ElapsedMs = FMath::RoundToInt((FPlatformTime::Seconds() - StartSeconds) * 1000.0);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] Serialization Check: serialized '%s' (%s) in %d ms."),
		*Ref, bRecursive ? TEXT("recursive") : TEXT("shallow"), ElapsedMs);

	OutputHeader = FText::Format(LOCTEXT("OutputHeaderMs", "Output ({0} ms)"), FText::AsNumber(ElapsedMs));
	SetOutput(Pretty);
	return FReply::Handled();
}

void SUnrealMcpSerializationCheckWindow::SetOutput(const FString& Text)
{
	FullOutputText = Text;
	if (OutputBox.IsValid())
		OutputBox->SetText(FText::FromString(Text));
}

FReply SUnrealMcpSerializationCheckWindow::OnCopyClicked()
{
	FPlatformApplicationMisc::ClipboardCopy(*FullOutputText);

	// Flash "Copied!" and restore the label ~1.5s later. The timer is registered on this widget so it is
	// torn down with the tab (no dangling callback). RegisterActiveTimer fires once (returns Stop).
	CopyLabel = LOCTEXT("Copied", "Copied!");
	RegisterActiveTimer(1.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SUnrealMcpSerializationCheckWindow::OnCopyResetTimer));
	return FReply::Handled();
}

EActiveTimerReturnType SUnrealMcpSerializationCheckWindow::OnCopyResetTimer(double, float)
{
	CopyLabel = LOCTEXT("Copy", "Copy");
	return EActiveTimerReturnType::Stop;
}

#undef LOCTEXT_NAMESPACE
