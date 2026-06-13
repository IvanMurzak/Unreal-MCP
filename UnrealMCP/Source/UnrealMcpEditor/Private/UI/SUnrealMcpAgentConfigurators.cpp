// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpAgentConfigurators.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "Agents/AiAgentConfiguratorRegistry.h"
#include "Agents/JsonAiAgentConfig.h"
#include "UnrealMcpLog.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Styling/AppStyle.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformApplicationMisc.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

void SUnrealMcpAgentConfigurators::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;
	ConnectionInfoProvider = InArgs._ConnectionInfoProvider;
	ProjectRootProvider = InArgs._ProjectRootProvider;

	if (!ProjectRootProvider)
		ProjectRootProvider = []() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()); };

	Registry = MakeShared<FAiAgentConfiguratorRegistry>();

	// Build the dropdown's item source (agent display names in registry order).
	AgentNameItems.Reset();
	for (const FString& Name : Registry->GetAgentNames())
		AgentNameItems.Add(MakeShared<FString>(Name));

	// Resolve the initial selection from the persisted SelectedAgentId (clamped to a valid index).
	const FString PersistedId = IsViewModelValid() ? ViewModel->GetSelectedAgentId() : FString();
	int32 InitialIndex = Registry->GetIndexByAgentId(PersistedId);
	if (InitialIndex == INDEX_NONE)
		InitialIndex = Registry->Num() > 0 ? 0 : INDEX_NONE;

	TSharedPtr<FString> InitialItem = AgentNameItems.IsValidIndex(InitialIndex) ? AgentNameItems[InitialIndex] : nullptr;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("AgentConfigHeader", "AI Agent Configurators"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("AgentConfigDesc", "Pick an AI agent / MCP client and configure it to connect to this project's MCP server."))
			]
			// Agent dropdown.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SAssignNew(AgentCombo, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&AgentNameItems)
				.InitiallySelectedItem(InitialItem)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
				{
					return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewItem, ESelectInfo::Type)
				{
					if (!NewItem.IsValid())
						return;
					const int32 Index = AgentNameItems.IndexOfByPredicate(
						[&NewItem](const TSharedPtr<FString>& I) { return I == NewItem; });
					TSharedPtr<FAiAgentConfigurator> Configurator = Registry->GetByIndex(Index);
					if (Configurator.IsValid() && IsViewModelValid())
						ViewModel->SetSelectedAgentId(Configurator->GetAgentId());
					bRevealToken = false; // reset reveal on agent switch (§8)
					RefreshSelectedConfigurator();
				})
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return Selected.IsValid() ? FText::FromString(Selected->GetAgentName()) : LOCTEXT("NoAgent", "Select an agent");
					})
				]
			]
			// Per-agent panel container (rebuilt on selection change).
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SAssignNew(AgentPanelContainer, SVerticalBox)
			]
		]
	];

	RefreshSelectedConfigurator();
}

void SUnrealMcpAgentConfigurators::RefreshSelectedConfigurator()
{
	const FString PersistedId = IsViewModelValid() ? ViewModel->GetSelectedAgentId() : FString();
	Selected = Registry->GetByAgentId(PersistedId);
	if (!Selected.IsValid())
		Selected = Registry->GetByIndex(0);

	BindSelected();
	RebuildAgentPanel();
}

void SUnrealMcpAgentConfigurators::BindSelected()
{
	if (!Selected.IsValid())
		return;
	const FAiAgentConnectionInfo Connection = ConnectionInfoProvider ? ConnectionInfoProvider() : FAiAgentConnectionInfo();
	const FString ProjectRoot = ProjectRootProvider ? ProjectRootProvider() : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Selected->Initialize(Connection, ProjectRoot);
}

FString SUnrealMcpAgentConfigurators::MaskTokenInSnippet(const FString& Snippet, const FString& Token, bool bReveal)
{
	if (bReveal || Token.IsEmpty())
		return Snippet;
	// Replace every literal occurrence of the raw token with a fixed mask (length not leaked). The token shows up
	// in the HTTP "Bearer <token>" header and the STDIO "token=<token>" arg; replacing the bare value covers both.
	return Snippet.Replace(*Token, TEXT("********"), ESearchCase::CaseSensitive);
}

FString SUnrealMcpAgentConfigurators::BuildSnippetPreview(bool bStdio) const
{
	if (!Selected.IsValid())
		return FString();
	FAiAgentConfig& Config = bStdio ? Selected->GetConfigStdio() : Selected->GetConfigHttp();
	const FString Snippet = Config.GetExpectedFileContent();
	const FAiAgentConnectionInfo Connection = ConnectionInfoProvider ? ConnectionInfoProvider() : FAiAgentConnectionInfo();
	return MaskTokenInSnippet(Snippet, Connection.Token, bRevealToken);
}

void SUnrealMcpAgentConfigurators::RebuildAgentPanel()
{
	if (!AgentPanelContainer.IsValid())
		return;

	AgentPanelContainer->ClearChildren();

	if (!Selected.IsValid())
	{
		AgentPanelContainer->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(LOCTEXT("NoAgentSelected", "No agent selected."))
		];
		return;
	}

	const FString DownloadUrl = Selected->GetDownloadUrl();
	const FString TutorialUrl = Selected->GetTutorialUrl();
	const FString ConfigPath = Selected->GetConfigFilePath(ProjectRootProvider ? ProjectRootProvider() : FPaths::ProjectDir());

	// Links row (download + optional tutorial).
	TSharedRef<SHorizontalBox> LinksRow = SNew(SHorizontalBox);
	LinksRow->AddSlot().AutoWidth().Padding(0, 0, 6, 0)
	[
		SNew(SButton)
		.Text(LOCTEXT("DownloadAgent", "Download / Docs"))
		.OnClicked_Lambda([DownloadUrl]()
		{
			if (!DownloadUrl.IsEmpty())
				FPlatformProcess::LaunchURL(*DownloadUrl, nullptr, nullptr);
			return FReply::Handled();
		})
	];
	if (!TutorialUrl.IsEmpty())
	{
		LinksRow->AddSlot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("TutorialAgent", "Tutorial"))
			.OnClicked_Lambda([TutorialUrl]()
			{
				FPlatformProcess::LaunchURL(*TutorialUrl, nullptr, nullptr);
				return FReply::Handled();
			})
		];
	}

	// A reusable transport section builder (STDIO and HTTP share the same shape).
	auto BuildTransportSection = [this](const FText& Heading, bool bStdio) -> TSharedRef<SWidget>
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(STextBlock).Text(Heading).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			// Status line.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
				.Text_Lambda([this, bStdio]()
				{
					if (!Selected.IsValid())
						return FText::GetEmpty();
					return Selected->IsConfigured(bStdio)
						? LOCTEXT("StatusConfigured", "Status: configured")
						: (Selected->IsAnyDetected()
							? LOCTEXT("StatusDetectedOutdated", "Status: detected (needs reconfigure)")
							: LOCTEXT("StatusNotConfigured", "Status: not configured"));
				})
				.ColorAndOpacity_Lambda([this, bStdio]()
				{
					if (Selected.IsValid() && Selected->IsConfigured(bStdio))
						return FSlateColor(FLinearColor(0.16f, 0.74f, 0.30f));
					return FSlateColor(FLinearColor(0.85f, 0.65f, 0.20f));
				})
			]
			// Snippet preview (read-only, token-masked).
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AlwaysShowScrollbars(false)
				.Text_Lambda([this, bStdio]()
				{
					return FText::FromString(BuildSnippetPreview(bStdio));
				})
			]
			// Action buttons.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text_Lambda([this, bStdio]()
					{
						return (Selected.IsValid() && Selected->IsConfigured(bStdio))
							? LOCTEXT("Reconfigure", "Reconfigure")
							: LOCTEXT("Configure", "Configure");
					})
					.OnClicked_Lambda([this, bStdio]()
					{
						if (Selected.IsValid())
						{
							const bool bOk = Selected->Configure(bStdio);
							UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] agent '%s' %s configure %s."),
								*Selected->GetAgentName(), bStdio ? TEXT("STDIO") : TEXT("HTTP"),
								bOk ? TEXT("succeeded") : TEXT("failed"));
							BindSelected(); // rebuild configs after the write
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopySnippet", "Copy"))
					.ToolTipText(LOCTEXT("CopySnippetHint", "Copy the config snippet (with the real token) to the clipboard."))
					.OnClicked_Lambda([this, bStdio]()
					{
						if (Selected.IsValid())
						{
							// Copy the REAL snippet (unmasked) — the user is pasting it into their own config.
							FAiAgentConfig& Config = bStdio ? Selected->GetConfigStdio() : Selected->GetConfigHttp();
							FPlatformApplicationMisc::ClipboardCopy(*Config.GetExpectedFileContent());
						}
						return FReply::Handled();
					})
				]
			];
	};

	AgentPanelContainer->AddSlot().AutoHeight()[ LinksRow ];

	// Config path line.
	AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Text(FText::Format(LOCTEXT("ConfigPathFmt", "Config file: {0}"), FText::FromString(ConfigPath)))
	];

	// Reveal-token toggle (per-agent; default masked, §8).
	AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this]() { return bRevealToken ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
		{
			bRevealToken = (NewState == ECheckBoxState::Checked);
		})
		[
			SNew(STextBlock).Text(LOCTEXT("RevealToken", "Reveal token in preview"))
		]
	];

	// STDIO + HTTP sections.
	AgentPanelContainer->AddSlot().AutoHeight()[ BuildTransportSection(LOCTEXT("StdioHeader", "STDIO (launch the server)"), /*bStdio*/ true) ];
	AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 4, 0, 0)[ SNew(SSeparator) ];
	AgentPanelContainer->AddSlot().AutoHeight()[ BuildTransportSection(LOCTEXT("HttpHeader", "HTTP (connect to URL)"), /*bStdio*/ false) ];

	// Remove (clears both transports).
	AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
	[
		SNew(SButton)
		.Text(LOCTEXT("RemoveAgent", "Remove"))
		.IsEnabled_Lambda([this]() { return Selected.IsValid() && Selected->IsAnyDetected(); })
		.OnClicked_Lambda([this]()
		{
			if (Selected.IsValid())
			{
				const bool bOk = Selected->RemoveAll();
				UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] agent '%s' remove %s."),
					*Selected->GetAgentName(), bOk ? TEXT("succeeded") : TEXT("(nothing to remove)"));
				BindSelected();
			}
			return FReply::Handled();
		})
	];
}

#undef LOCTEXT_NAMESPACE
