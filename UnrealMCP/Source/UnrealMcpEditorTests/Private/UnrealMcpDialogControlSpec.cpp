// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Session/UnrealMcpDialogControl.h"
#include "Session/UnrealMcpModalWatch.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

/**
 * unreal-mcp-cli dialog reads and answers a modal dialog through files the modal loop serves (Windows UI Automation
 * sees no buttons in a Slate modal window). Built from a window that is never shown: a real modal loop would block the
 * spec, and the watch's handlers are what the loop calls.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpDialogControlSpec, "UnrealMcp.Session.DialogControl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	FString Dir;
	int32 YesClicks = 0;
	int32 NoClicks = 0;

	static FText DialogSpecText(const TCHAR* Text)
	{
		return FText::FromString(Text);
	}

	/**
	 * "Delete Assets": its title repeated, a question, a hidden text, a check box, a list header, Yes / No / a disabled
	 * button, an icon button.
	 */
	TSharedRef<SWindow> MakeDialog()
	{
		return SNew(SWindow)
			.Title(DialogSpecText(TEXT("Delete Assets")))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()[SNew(STextBlock).Text(DialogSpecText(TEXT("Delete Assets")))]
				+ SVerticalBox::Slot()[SNew(STextBlock).Text(DialogSpecText(TEXT("Delete 3 assets?")))]
				+ SVerticalBox::Slot()[SNew(STextBlock).Text(DialogSpecText(TEXT("Hidden"))).Visibility(EVisibility::Collapsed)]
				+ SVerticalBox::Slot()[SNew(SCheckBox)[SNew(STextBlock).Text(DialogSpecText(TEXT("Apply to All")))]]
				+ SVerticalBox::Slot()[SNew(SHeaderRow) + SHeaderRow::Column(TEXT("Asset")).DefaultLabel(DialogSpecText(TEXT("Asset")))]
				+ SVerticalBox::Slot()
				[
					SNew(SButton)
					.OnClicked_Lambda([this]() { ++YesClicks; return FReply::Handled(); })
					[
						SNew(STextBlock).Text(DialogSpecText(TEXT("Yes")))
					]
				]
				+ SVerticalBox::Slot()[SNew(SButton).Text(DialogSpecText(TEXT("No"))).OnClicked_Lambda([this]() { ++NoClicks; return FReply::Handled(); })]
				+ SVerticalBox::Slot()[SNew(SButton).Text(DialogSpecText(TEXT("Later"))).IsEnabled(false)]
				+ SVerticalBox::Slot()[SNew(SButton)[SNew(SImage)]]
			];
	}

	TSharedPtr<FJsonObject> ReadDialogSpecJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (FFileHelper::LoadFileToString(Text, *Path))
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		return Object;
	}

	void RequestPress(const FUnrealMcpDialogControl& Control, const TCHAR* Request, const TCHAR* Button, const TCHAR* Id)
	{
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"request\":\"%s\",\"button\":\"%s\",\"id\":\"%s\"}"), Request, Button, Id),
			*Control.PressFile());
	}

END_DEFINE_SPEC(FUnrealMcpDialogControlSpec)

void FUnrealMcpDialogControlSpec::Define()
{
	BeforeEach([this]()
	{
		YesClicks = 0;
		NoClicks = 0;
		Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("UnrealMcpTests") / TEXT("Dialogs"));
		IFileManager::Get().DeleteDirectory(*Dir, /*bRequireExists*/ false, /*bTree*/ true);
		IFileManager::Get().MakeDirectory(*Dir, /*bTree*/ true);
	});

	AfterEach([this]() { IFileManager::Get().DeleteDirectory(*Dir, /*bRequireExists*/ false, /*bTree*/ true); });

	It("describes the question and the labelled buttons, not the title, hidden texts, options, list headers or icon buttons", [this]()
	{
		const FUnrealMcpDialogControl::FDescription Dialog = FUnrealMcpDialogControl::Describe(MakeDialog());
		TestEqual(TEXT("title"), Dialog.Title, FString(TEXT("Delete Assets")));
		TestEqual(TEXT("message"), Dialog.Message, FString(TEXT("Delete 3 assets?")));
		TestEqual(TEXT("buttons"), Dialog.Buttons, TArray<FString>{ TEXT("Yes"), TEXT("No"), TEXT("Later") });
	});

	It("presses an enabled button by its label, in any case", [this]()
	{
		const TSharedRef<SWindow> Window = MakeDialog();
		FString Pressed;
		TestTrue(TEXT("yes"), FUnrealMcpDialogControl::Press(Window, TEXT(" yes "), &Pressed));
		TestEqual(TEXT("Yes clicked"), YesClicks, 1);
		TestEqual(TEXT("as the dialog shows it"), Pressed, FString(TEXT("Yes")));
		TestFalse(TEXT("a disabled button"), FUnrealMcpDialogControl::Press(Window, TEXT("Later")));
		TestFalse(TEXT("no such button"), FUnrealMcpDialogControl::Press(Window, TEXT("Maybe")));
		TestFalse(TEXT("a check box is no button"), FUnrealMcpDialogControl::Press(Window, TEXT("Apply to All")));
		TestEqual(TEXT("No untouched"), NoClicks, 0);
	});

	It("publishes the dialog, answers a press request through files, and clears them", [this]()
	{
		FUnrealMcpDialogControl Control(Dir, 4242);
		const TSharedRef<SWindow> Window = MakeDialog();
		Control.Publish(FUnrealMcpDialogControl::Describe(Window), 7);
		TestEqual(TEXT("one file per editor process"), FPaths::GetCleanFilename(Control.DialogFile()), FString(TEXT("4242.json")));
		const TSharedPtr<FJsonObject> Published = ReadDialogSpecJson(Control.DialogFile());
		if (!TestTrue(TEXT("published"), Published.IsValid()))
			return;
		TestEqual(TEXT("pid"), static_cast<int32>(Published->GetNumberField(TEXT("pid"))), 4242);
		TestEqual(TEXT("id"), Published->GetStringField(TEXT("id")), FString(TEXT("7")));
		TestEqual(TEXT("message"), Published->GetStringField(TEXT("message")), FString(TEXT("Delete 3 assets?")));
		TestEqual(TEXT("buttons"), Published->GetArrayField(TEXT("buttons")).Num(), 3);

		Control.PollPress(Window, 7);
		TestFalse(TEXT("no request, no answer"), FPaths::FileExists(Control.ResultFile()));

		RequestPress(Control, TEXT("r1"), TEXT("no"), TEXT("7"));
		Control.PollPress(Window, 7);
		TestEqual(TEXT("No clicked"), NoClicks, 1);
		TestFalse(TEXT("request taken"), FPaths::FileExists(Control.PressFile()));
		TSharedPtr<FJsonObject> Result = ReadDialogSpecJson(Control.ResultFile());
		if (!TestTrue(TEXT("answered"), Result.IsValid()))
			return;
		TestEqual(TEXT("request"), Result->GetStringField(TEXT("request")), FString(TEXT("r1")));
		TestEqual(TEXT("pressed"), Result->GetStringField(TEXT("result")), FString(TEXT("pressed")));
		TestEqual(TEXT("the label as shown"), Result->GetStringField(TEXT("button")), FString(TEXT("No")));

		RequestPress(Control, TEXT("r2"), TEXT("Yes"), TEXT("6"));
		Control.PollPress(Window, 7);
		Result = ReadDialogSpecJson(Control.ResultFile());
		TestEqual(TEXT("an older dialog id is refused"), Result->GetStringField(TEXT("result")), FString(TEXT("not-found")));
		TestTrue(TEXT("saying which dialog is open"), Result->GetStringField(TEXT("message")).Contains(TEXT("no longer open")));
		TestEqual(TEXT("nothing clicked"), YesClicks, 0);

		RequestPress(Control, TEXT("r3"), TEXT("Maybe"), TEXT(""));
		Control.PollPress(Window, 7);
		Result = ReadDialogSpecJson(Control.ResultFile());
		TestTrue(TEXT("names the buttons there are"), Result->GetStringField(TEXT("message")).Contains(TEXT("Yes, No, Later")));

		RequestPress(Control, TEXT("r4"), TEXT("Yes"), TEXT(""));
		Control.Clear();
		TestFalse(TEXT("dialog file removed"), FPaths::FileExists(Control.DialogFile()));
		Result = ReadDialogSpecJson(Control.ResultFile());
		TestEqual(TEXT("a late request is answered"), Result->GetStringField(TEXT("request")), FString(TEXT("r4")));
		TestEqual(TEXT("as not found"), Result->GetStringField(TEXT("result")), FString(TEXT("not-found")));
		TestEqual(TEXT("and not pressed"), YesClicks, 0);
	});

	It("is driven by the modal watch: published on the first modal tick, pressed on a later one, cleared on close", [this]()
	{
		FUnrealMcpModalWatch Watch([](const FString&) { return 0; }, Dir);
		const FUnrealMcpDialogControl& Control = Watch.GetDialogControl();
		const TSharedRef<SWindow> Window = MakeDialog();
		Watch.HandleModalTick(Window, 0.0);
		TestEqual(TEXT("first dialog"), Watch.GetDialogId(), 1);
		TestTrue(TEXT("published"), FPaths::FileExists(Control.DialogFile()));

		RequestPress(Control, TEXT("w1"), TEXT("yes"), TEXT("1"));
		Watch.HandleModalTick(Window, FUnrealMcpModalWatch::PressPollSeconds / 2);
		TestEqual(TEXT("not before the next poll"), YesClicks, 0);
		Watch.HandleModalTick(Window, FUnrealMcpModalWatch::PressPollSeconds * 2);
		TestEqual(TEXT("pressed"), YesClicks, 1);

		Watch.HandleModalClosed(/*bAnyModalLeft*/ false);
		TestFalse(TEXT("cleared"), FPaths::FileExists(Control.DialogFile()));
		Watch.HandleModalTick(MakeDialog(), 10.0);
		TestEqual(TEXT("a new dialog has a new id"), Watch.GetDialogId(), 2);
	});

	It("names the buttons and the CLI in the reason a blocked call ends with", [this]()
	{
		const FString Reason = FUnrealMcpModalWatch::BlockedReason(TEXT("Save Content"), 3.0, { TEXT("Save Selected"), TEXT("Don't Save") });
		TestTrue(TEXT("the CLI's prefix"), Reason.StartsWith(TEXT("The editor is showing the modal dialog \"Save Content\" (open for")));
		TestTrue(TEXT("how to answer"), Reason.Contains(TEXT("unreal-mcp-cli dialog --press")));
		TestTrue(TEXT("the buttons"), Reason.Contains(TEXT("Save Selected, Don't Save")));
		TestFalse(TEXT("no buttons, no CLI hint"), FUnrealMcpModalWatch::BlockedReason(TEXT("X"), 3.0).Contains(TEXT("--press")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
