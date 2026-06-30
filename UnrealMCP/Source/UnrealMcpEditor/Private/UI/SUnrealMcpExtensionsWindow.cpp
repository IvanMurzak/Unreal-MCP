// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpExtensionsWindow.h"
#include "Extensions/UnrealMcpExtensionManager.h" // FUnrealMcpExtensionRecord (runtime module)
#include "UnrealMcpLog.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

void SUnrealMcpExtensionsWindow::Construct(const FArguments& InArgs)
{
	bEmbedded = InArgs._bEmbedded;
	ProjectDir = InArgs._ProjectDir;
	Catalog = InArgs._InitialCatalog;
	InstalledProvider = InArgs._InstalledProvider;
	CatalogFetcher = InArgs._CatalogFetcher;
	OnSetEnabled = InArgs._OnSetEnabled;
	OnInstall = InArgs._OnInstall;
	OnTriggerLiveCompile = InArgs._OnTriggerLiveCompile;

	StatusMessage = Catalog.Num() > 0
		? LOCTEXT("ExtReadyCatalog", "Showing installed extensions and the catalog.").ToString()
		: LOCTEXT("ExtReadyNoCatalog", "Showing installed extensions. Click Refresh to load the available-extensions catalog.").ToString();

	ListContainer = SNew(SVerticalBox);

	// The Refresh button is identical in both modes. In embedded mode it is the only header element (the host
	// main window's section header already says "Extensions"); standalone, it sits to the right of the bold title.
	const TSharedRef<SWidget> RefreshButton =
		SNew(SButton)
		.Text(LOCTEXT("ExtRefresh", "Refresh catalog"))
		.ToolTipText(LOCTEXT("ExtRefreshTip", "Fetch the latest available-extensions catalog over HTTP."))
		.OnClicked(this, &SUnrealMcpExtensionsWindow::OnRefreshClicked);

	// Embedded: the host section header already says "Extensions", so the header row is just the Refresh button,
	// right-aligned at the slot level. Standalone: a FillWidth bold title pushes Refresh to the right.
	TSharedRef<SHorizontalBox> HeaderRow = SNew(SHorizontalBox);
	if (!bEmbedded)
	{
		HeaderRow->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ExtHeading", "Extensions"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
		];
	}
	HeaderRow->AddSlot().AutoWidth().VAlign(VAlign_Center)[ RefreshButton ];

	// Embedded composes into the host's padded scroll area, so it carries no outer padding of its own.
	const FMargin HeaderPad = bEmbedded ? FMargin(0, 0, 0, 4) : FMargin(8, 8, 8, 4);
	const FMargin LinePad   = bEmbedded ? FMargin(0, 0, 0, 4) : FMargin(8, 0, 8, 4);

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		// Header: (embedded) Refresh only, right-aligned · (standalone) bold title + Refresh.
		+ SVerticalBox::Slot().AutoHeight().HAlign(bEmbedded ? HAlign_Right : HAlign_Fill).Padding(HeaderPad)[ HeaderRow ]
		// Status line.
		+ SVerticalBox::Slot().AutoHeight().Padding(LinePad)
		[
			SNew(STextBlock)
			.Text(this, &SUnrealMcpExtensionsWindow::GetStatusText)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		// Compile-on-install hint (the §5 key UX risk): visible only after an install that needs a rebuild.
		+ SVerticalBox::Slot().AutoHeight().Padding(LinePad)
		[
			SNew(SBorder)
			.Visibility(this, &SUnrealMcpExtensionsWindow::GetRestartHintVisibility)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ExtRebuildHint",
						"An installed extension ships as C++ source — restart the editor to finish compiling, or trigger Live Coding now."))
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("ExtCompile", "Compile (Live Coding)"))
					.OnClicked(this, &SUnrealMcpExtensionsWindow::OnCompileClicked)
				]
			]
		];

	if (bEmbedded)
	{
		// The host main window already wraps everything in an SScrollBox, so the list is an AutoHeight block —
		// a nested FillHeight scroll box here would collapse to zero height inside the host's AutoHeight slot.
		Body->AddSlot().AutoHeight().Padding(0, 4, 0, 0)[ ListContainer.ToSharedRef() ];
	}
	else
	{
		Body->AddSlot().AutoHeight()[ SNew(SSeparator) ];
		Body->AddSlot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(8.0f)[ ListContainer.ToSharedRef() ]
		];
	}

	ChildSlot[ Body ];

	RebuildList();
}

void SUnrealMcpExtensionsWindow::RebuildList()
{
	if (!ListContainer.IsValid())
		return;
	ListContainer->ClearChildren();

	TArray<FUnrealMcpExtensionRecord> Loaded;
	if (InstalledProvider)
		Loaded = InstalledProvider();
	const TArray<FUnrealMcpInstalledOnDisk> OnDisk = FUnrealMcpExtensionInstaller::ScanInstalledPlugins(ProjectDir);

	const TArray<FUnrealMcpExtensionRow> Rows = FUnrealMcpExtensionInstaller::BuildRows(Catalog, Loaded, OnDisk);
	if (Rows.Num() == 0)
	{
		ListContainer->AddSlot().AutoHeight().Padding(4.0f)
		[
			SNew(STextBlock).Text(LOCTEXT("ExtEmpty",
				"No extensions installed and no catalog loaded. Click Refresh to load the available-extensions catalog."))
			.AutoWrapText(true)
		];
		return;
	}

	for (const FUnrealMcpExtensionRow& Row : Rows)
		ListContainer->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[ BuildRow(Row) ];
}

TSharedRef<SWidget> SUnrealMcpExtensionsWindow::BuildRow(const FUnrealMcpExtensionRow& Row)
{
	// State badge text + the action affordance depend on the install/loaded state.
	FText StateBadge;
	if (Row.bLoaded)
		StateBadge = Row.bEnabled ? LOCTEXT("ExtStateEnabled", "Installed · enabled")
								  : LOCTEXT("ExtStateDisabled", "Installed · disabled");
	else if (Row.bInstalled)
		StateBadge = LOCTEXT("ExtStateNeedsCompile", "Installed · needs editor restart to compile");
	else
		StateBadge = LOCTEXT("ExtStateAvailable", "Available");

	const FString ExtensionId = Row.ExtensionId;
	const FText Title = Row.DisplayName.IsEmpty()
		? FText::FromString(Row.PluginName)
		: FText::FromString(Row.DisplayName);
	const FString VersionLabel = Row.Version.IsEmpty() ? FString() : FString::Printf(TEXT("v%s"), *FUnrealMcpExtensionInstaller::StripLeadingV(Row.Version));

	TSharedRef<SHorizontalBox> ActionRow = SNew(SHorizontalBox);

	// Enable/disable checkbox — only meaningful for a loaded provider (the §5 hot-load toggle).
	if (Row.bLoaded && OnSetEnabled)
	{
		ActionRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(SCheckBox)
			.IsChecked(Row.bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
			.ToolTipText(LOCTEXT("ExtToggleTip", "Enable or disable this extension's tools live (no editor restart)."))
			.OnCheckStateChanged_Lambda([this, ExtensionId](ECheckBoxState NewState)
			{
				OnEnabledChanged(ExtensionId, NewState == ECheckBoxState::Checked);
			})
		];
	}

	// Install / Update button — for catalog rows that are installable (have a repo or are available).
	if (Row.bInCatalog && OnInstall)
	{
		if (!Row.bInstalled)
		{
			ActionRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ExtInstall", "Install"))
				.OnClicked(this, &SUnrealMcpExtensionsWindow::OnInstallClicked, ExtensionId, false)
			];
		}
		else if (Row.bUpdateAvailable)
		{
			ActionRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ExtUpdate", "Update"))
				.OnClicked(this, &SUnrealMcpExtensionsWindow::OnInstallClicked, ExtensionId, true)
			];
		}
	}

	TSharedRef<SVerticalBox> TextCol = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Title).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				SNew(STextBlock).Text(FText::FromString(VersionLabel)).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				SNew(STextBlock).Text(StateBadge).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Row.Description))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];

	// Per-extension error badge — the registry surfaces invalid/duplicate-id / per-tool validation failures.
	if (Row.bHasError)
	{
		TextCol->AddSlot().AutoHeight().Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("ExtError", "Error: {0}"), FText::FromString(Row.Error)))
			.AutoWrapText(true)
			.ColorAndOpacity(FLinearColor(0.9f, 0.4f, 0.4f))
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ TextCol ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)[ ActionRow ]
		];
}

FReply SUnrealMcpExtensionsWindow::OnRefreshClicked()
{
	if (!CatalogFetcher)
	{
		StatusMessage = LOCTEXT("ExtNoFetcher", "Catalog refresh is unavailable in this build.").ToString();
		return FReply::Handled();
	}
	TArray<FUnrealMcpCatalogEntry> Fetched;
	FString Error;
	if (CatalogFetcher(Fetched, Error))
	{
		Catalog = MoveTemp(Fetched);
		StatusMessage = FText::Format(
			LOCTEXT("ExtLoadedCount", "Loaded {0} available extension(s) from the catalog."),
			FText::AsNumber(Catalog.Num())).ToString();
	}
	else
	{
		StatusMessage = FText::Format(
			LOCTEXT("ExtCatalogLoadFailed", "Could not load the catalog: {0}"),
			FText::FromString(Error)).ToString();
	}
	RebuildList();
	return FReply::Handled();
}

FReply SUnrealMcpExtensionsWindow::OnInstallClicked(FString ExtensionId, bool bForce)
{
	if (!OnInstall)
		return FReply::Handled();
	const FUnrealMcpCatalogEntry* Entry = FUnrealMcpExtensionCatalog::FindEntry(Catalog, ExtensionId);
	if (Entry == nullptr)
	{
		StatusMessage = FText::Format(
			LOCTEXT("ExtNotInCatalog", "Extension '{0}' is no longer in the catalog — Refresh and try again."),
			FText::FromString(ExtensionId)).ToString();
		return FReply::Handled();
	}

	const FUnrealMcpInstallResult Result = OnInstall(*Entry, bForce);
	StatusMessage = Result.bSuccess
		? Result.Message
		: FText::Format(LOCTEXT("ExtInstallFailed", "Install failed: {0}"), FText::FromString(Result.Error)).ToString();
	for (const FString& Warning : Result.Warnings)
		StatusMessage += FText::Format(LOCTEXT("ExtInstallWarning", "\nWarning: {0}"), FText::FromString(Warning)).ToString();
	bRestartHintVisible = Result.bSuccess && Result.bRebuildRequired;

	if (Result.bSuccess)
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] extension install: %s"), *Result.Message);
	}
	else
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension install failed: %s"), *Result.Error);
	}

	RebuildList();
	return FReply::Handled();
}

FReply SUnrealMcpExtensionsWindow::OnCompileClicked()
{
	if (OnTriggerLiveCompile)
		StatusMessage = OnTriggerLiveCompile();
	else
		StatusMessage = LOCTEXT("ExtNoLiveCoding", "Live Coding is unavailable — restart the editor to finish compiling.").ToString();
	return FReply::Handled();
}

void SUnrealMcpExtensionsWindow::OnEnabledChanged(FString ExtensionId, bool bEnabled)
{
	if (OnSetEnabled)
		OnSetEnabled(ExtensionId, bEnabled);
	StatusMessage = FText::Format(
		LOCTEXT("ExtToggleStatus", "Extension '{0}' {1}."),
		FText::FromString(ExtensionId),
		bEnabled ? LOCTEXT("ExtEnabledWord", "enabled") : LOCTEXT("ExtDisabledWord", "disabled")).ToString();
	RebuildList();
}

FText SUnrealMcpExtensionsWindow::GetStatusText() const
{
	return FText::FromString(StatusMessage);
}

EVisibility SUnrealMcpExtensionsWindow::GetRestartHintVisibility() const
{
	return bRestartHintVisible ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
