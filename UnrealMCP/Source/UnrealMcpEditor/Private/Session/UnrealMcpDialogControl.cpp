// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Session/UnrealMcpDialogControl.h"
#include "UnrealMcpLog.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Text/STextBlock.h"

namespace UnrealMcpDialogControlLocal
{
	/** A long text (the package list of "Save Content") is cut here. */
	constexpr int32 MaxMessageChars = 2000;

	// Matched by the type name SNew gives a widget (Slate has no RTTI); both are SButton or derive from it.
	const FName ButtonType(TEXT("SButton"));
	const FName PrimaryButtonType(TEXT("SPrimaryButton"));
	const FName CheckBoxType(TEXT("SCheckBox"));
	const FName HeaderRowType(TEXT("SHeaderRow"));
	const FName TextBlockType(TEXT("STextBlock"));
	const FName RichTextBlockType(TEXT("SRichTextBlock"));

	/** The text @p Widget shows itself, if it is a text block. */
	FString OwnText(const TSharedRef<SWidget>& Widget)
	{
		const FName Type = Widget->GetType();
		if (Type == TextBlockType)
			return StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString().TrimStartAndEnd();
		if (Type == RichTextBlockType)
			return StaticCastSharedRef<SRichTextBlock>(Widget)->GetText().ToString().TrimStartAndEnd();
		return FString();
	}

	/** A button, the label it shows, and whether it and every widget around it are enabled. */
	struct FLabelledButton
	{
		TSharedRef<SButton> Button;
		FString Label;
		bool bEnabled = true;
	};

	void Walk(const TSharedRef<SWidget>& Widget, bool bParentEnabled, TArray<FString>& OutTexts, TArray<FLabelledButton>& OutButtons);

	void WalkChildren(const TSharedRef<SWidget>& Widget, bool bEnabled, TArray<FString>& OutTexts, TArray<FLabelledButton>& OutButtons)
	{
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
			Walk(Children->GetChildAt(Index), bEnabled, OutTexts, OutButtons);
	}

	/**
	 * The visible texts outside buttons, check boxes and list headers, and the buttons that show a label (the texts inside
	 * them; icon-only ones have none).
	 */
	void Walk(const TSharedRef<SWidget>& Widget, bool bParentEnabled, TArray<FString>& OutTexts, TArray<FLabelledButton>& OutButtons)
	{
		if (!Widget->GetVisibility().IsVisible())
			return;
		const bool bEnabled = bParentEnabled && Widget->IsEnabled(); // a disabled panel disables what it holds
		const FName Type = Widget->GetType();
		if (Type == ButtonType || Type == PrimaryButtonType)
		{
			TArray<FString> Texts;
			TArray<FLabelledButton> Nested;
			WalkChildren(Widget, bEnabled, Texts, Nested);
			if (!Texts.IsEmpty())
				OutButtons.Add({ StaticCastSharedRef<SButton>(Widget), FString::Join(Texts, TEXT(" ")), bEnabled });
			return;
		}
		if (Type == CheckBoxType || Type == HeaderRowType)
			return; // "Apply to All", a list's sortable column headers: not part of the question or its answers
		const FString Text = OwnText(Widget);
		if (!Text.IsEmpty())
			OutTexts.Add(Text);
		WalkChildren(Widget, bEnabled, OutTexts, OutButtons);
	}
}

FUnrealMcpDialogControl::FUnrealMcpDialogControl(FString InDirectory, uint32 InPid)
	: Directory(InDirectory.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UnrealMCP") / TEXT("Dialogs")) : MoveTemp(InDirectory))
	, Pid(InPid != 0 ? InPid : FPlatformProcess::GetCurrentProcessId())
	, DialogPath(File(TEXT(".json")))
	, PressPath(File(TEXT(".press.json")))
	, ResultPath(File(TEXT(".result.json")))
{
}

FUnrealMcpDialogControl::FDescription FUnrealMcpDialogControl::Describe(const TSharedRef<SWindow>& Window)
{
	FDescription Out;
	Out.Title = Window->GetTitle().ToString();
	TArray<FString> Texts;
	TArray<UnrealMcpDialogControlLocal::FLabelledButton> Buttons;
	UnrealMcpDialogControlLocal::Walk(Window, /*bParentEnabled*/ true, Texts, Buttons);
	Texts.RemoveAll([&Out](const FString& Text) { return Text == Out.Title; }); // the title bar repeats it
	Out.Message = FString::Join(Texts, TEXT("\n"));
	if (Out.Message.Len() > UnrealMcpDialogControlLocal::MaxMessageChars)
		Out.Message = Out.Message.Left(UnrealMcpDialogControlLocal::MaxMessageChars) + TEXT("...");
	for (const UnrealMcpDialogControlLocal::FLabelledButton& Button : Buttons)
		Out.Buttons.Add(Button.Label);
	return Out;
}

TFunction<void()> FUnrealMcpDialogControl::FindPress(const TSharedRef<SWindow>& Window, const FString& Label, FString* OutPressed)
{
	TArray<FString> Texts;
	TArray<UnrealMcpDialogControlLocal::FLabelledButton> Buttons;
	UnrealMcpDialogControlLocal::Walk(Window, /*bParentEnabled*/ true, Texts, Buttons);
	const FString Wanted = Label.TrimStartAndEnd();
	const UnrealMcpDialogControlLocal::FLabelledButton* Found = Buttons.FindByPredicate(
		[&Wanted](const UnrealMcpDialogControlLocal::FLabelledButton& Button)
		{
			return Button.bEnabled && Button.Label.Equals(Wanted, ESearchCase::IgnoreCase);
		});
	if (!Found)
		return nullptr;
	if (OutPressed)
		*OutPressed = Found->Label;
	// What a click runs (OnClicked), without moving the mouse or needing the window in front (non-Shipping API).
	return [Button = Found->Button]() { Button->SimulateClick(); };
}

bool FUnrealMcpDialogControl::Press(const TSharedRef<SWindow>& Window, const FString& Label, FString* OutPressed)
{
	const TFunction<void()> Click = FindPress(Window, Label, OutPressed);
	if (!Click)
		return false;
	Click();
	return true;
}

FString FUnrealMcpDialogControl::File(const TCHAR* Suffix) const
{
	return Directory / FString::Printf(TEXT("%u%s"), Pid, Suffix);
}

void FUnrealMcpDialogControl::WriteJson(const FString& Path, const TSharedRef<FJsonObject>& Object) const
{
	FString Text;
	FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
	// Written aside and moved over the old file, so a reader never sees half a file (it may briefly see none). No retries:
	// this runs on the game thread, inside the modal loop.
	const FString Temp = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(*Path, *Temp, /*Replace*/ true, /*EvenIfReadOnly*/ true, /*Attributes*/ false, /*bDoNotRetryOrError*/ true))
	{
		IFileManager::Get().Delete(*Temp, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] Could not write %s: unreal-mcp-cli dialog cannot see or answer the dialog."), *Path);
	}
}

void FUnrealMcpDialogControl::Publish(const FDescription& Dialog, int32 Id)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("pid"), Pid);
	Object->SetStringField(TEXT("id"), FString::FromInt(Id));
	Object->SetStringField(TEXT("title"), Dialog.Title);
	Object->SetStringField(TEXT("message"), Dialog.Message);
	TArray<TSharedPtr<FJsonValue>> Buttons;
	for (const FString& Button : Dialog.Buttons)
		Buttons.Add(MakeShared<FJsonValueString>(Button));
	Object->SetArrayField(TEXT("buttons"), Buttons);
	WriteJson(DialogFile(), Object);
}

void FUnrealMcpDialogControl::PollPress(const TSharedPtr<SWindow>& Window, int32 Id)
{
	const FString& RequestFile = PressFile();
	FString Text;
	if (!FPaths::FileExists(RequestFile) || !FFileHelper::LoadFileToString(Text, *RequestFile))
		return;
	IFileManager::Get().Delete(*RequestFile, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);

	TSharedPtr<FJsonObject> Request;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Request);
	FString RequestId, Label, WantedId;
	if (Request.IsValid())
	{
		Request->TryGetStringField(TEXT("request"), RequestId);
		Request->TryGetStringField(TEXT("button"), Label);
		Request->TryGetStringField(TEXT("id"), WantedId);
	}

	FString Message;
	FString Pressed = Label;
	TFunction<void()> Click;
	if (!Window.IsValid())
		Message = TEXT("No modal dialog is open.");
	else if (Label.IsEmpty())
		Message = TEXT("The request names no button.");
	else if (!WantedId.IsEmpty() && WantedId != FString::FromInt(Id))
		Message = FString::Printf(TEXT("Dialog %s is no longer open; the dialog now open is %d \"%s\"."), *WantedId, Id, *Window->GetTitle().ToString());
	else
	{
		Click = FindPress(Window.ToSharedRef(), Label, &Pressed);
		if (!Click)
		{
			Message = FString::Printf(TEXT("The dialog \"%s\" has no enabled \"%s\" button (buttons: %s)."), *Window->GetTitle().ToString(), *Label,
				*FString::Join(Describe(Window.ToSharedRef()).Buttons, TEXT(", ")));
		}
	}

	const TSharedRef<FJsonObject> Answer = MakeShared<FJsonObject>();
	Answer->SetStringField(TEXT("request"), RequestId);
	Answer->SetStringField(TEXT("result"), Click ? TEXT("pressed") : TEXT("not-found"));
	Answer->SetStringField(TEXT("button"), Pressed);
	Answer->SetStringField(TEXT("message"), Message);
	WriteJson(ResultFile(), Answer);

	// Answered before the click: a button that opens another modal dialog runs that dialog's loop inside the click.
	if (Click)
	{
		UE_LOG(LogUnrealMcp, Display, TEXT("[Unreal-MCP] Pressed \"%s\" on the modal dialog \"%s\" (unreal-mcp-cli dialog)."), *Pressed, *Window->GetTitle().ToString());
		Click();
	}
}

void FUnrealMcpDialogControl::Clear()
{
	IFileManager& Files = IFileManager::Get();
	Files.Delete(*DialogFile(), /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
	// A request that arrived after the dialog closed gets its answer here, rather than waiting out its timeout.
	PollPress(nullptr, 0);
}
