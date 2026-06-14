// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpAgentConfigurators.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UI/SUnrealMcpAgentWidgets.h"
#include "Agents/AiAgentConfiguratorRegistry.h"
#include "Agents/JsonAiAgentConfig.h"
#include "Agents/UnrealMcpSkillFileGenerator.h"
#include "Agents/Impl/CustomConfigurator.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpLog.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
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

	// Subscribe to connection-setting changes so the panel re-resolves + rebuilds when the user edits the
	// connection mode / host / auth / token in the main window — without reselecting the agent (§7; the C++/Slate
	// analog of Unity's InvalidateAndReloadAgentUI). The handle is removed in the destructor.
	if (IsViewModelValid())
		ConnectionChangedHandle = ViewModel->OnConnectionSettingsChanged.AddSP(this, &SUnrealMcpAgentConfigurators::OnConnectionSettingsChanged);

	RefreshSelectedConfigurator();
}

SUnrealMcpAgentConfigurators::~SUnrealMcpAgentConfigurators()
{
	if (ViewModel.IsValid() && ConnectionChangedHandle.IsValid())
		ViewModel->OnConnectionSettingsChanged.Remove(ConnectionChangedHandle);
}

void SUnrealMcpAgentConfigurators::OnConnectionSettingsChanged()
{
	// A connection setting changed. Re-bind the selected configurator to the now-current connection info (BindSelected
	// calls Initialize, which invalidates the cached STDIO/HTTP configs) and rebuild the panel so the snippet preview,
	// the per-transport status, and what Configure() will write all reflect the new settings.
	BindSelected();
	RebuildAgentPanel();
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

	// The Custom configurator's skills path is user-overridable and persisted (mirrors Unity's editable SkillsPath).
	// Push the persisted value into the live Custom configurator so its GetSkillsPath()/SupportsSkills() reflect it.
	if (IsViewModelValid())
	{
		if (FCustomConfigurator* Custom = static_cast<FCustomConfigurator*>(Selected->GetAgentId() == TEXT("other-custom") ? Selected.Get() : nullptr))
			Custom->SetCustomSkillsPath(ViewModel->GetSkillsPath());
	}
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

	// The raw config/snippet text field is noise for built-in agents (their Configure button writes the file
	// directly); only the snippet-only Custom agent — which has no config path, so the user pastes the snippet by
	// hand — keeps showing it (§7, issue #56). Computed once here; the panel is rebuilt on every selection change.
	// Selected is guaranteed valid here — RebuildAgentPanel early-returns above when it is not.
	const bool bShowSnippet = Selected->GetAgentId() == TEXT("other-custom");

	// Issue #59: render ONLY the user-selected (effective) transport, not both. Cloud locks this to Http; Custom uses
	// the stored stdio/http choice. The whole panel rebuilds via OnConnectionSettingsChanged when the user flips the
	// transport selector in the main window, so this single-transport view always tracks the live selection.
	const EUnrealMcpTransportMethod EffectiveTransport = IsViewModelValid()
		? ViewModel->GetEffectiveTransport()
		: EUnrealMcpTransportMethod::Http;
	const bool bStdio = EffectiveTransport == EUnrealMcpTransportMethod::Stdio;
	const FText TransportHeading = bStdio
		? LOCTEXT("StdioHeader", "STDIO (launch the server)")
		: LOCTEXT("HttpHeader", "HTTP (connect to URL)");

	AgentPanelContainer->AddSlot().AutoHeight()[ LinksRow ];

	// Config path line (skip for the snippet-only Custom agent, which has no on-disk file).
	if (!ConfigPath.IsEmpty())
	{
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(FText::Format(LOCTEXT("ConfigPathFmt", "Config file: {0}"), FText::FromString(ConfigPath)))
		];
	}

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

	// Active-transport heading.
	AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
	[
		SNew(STextBlock).Text(TransportHeading).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
	];

	// Configuration-status row (Unity's ConfigurationElements): "Configured (transport)" / "Not configured" +
	// Configure/Reconfigure + Remove, shown only for agents with an on-disk config file. The snippet-only Custom
	// agent has no file to detect, so its status row is meaningless — it relies on the snippet preview + Copy below.
	if (!bShowSnippet)
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 4, 0, 0)[ MakeConfigurationStatusRow(bStdio) ];

	// Per-agent rich content (issue #59): the configurator's foldout sections for the active transport (the 1:1
	// data port of Unity's per-agent OnUICreated foldouts). Rendered with the reusable widget templates.
	for (const FAiAgentRichContentSection& RichSection : Selected->BuildRichContent(bStdio))
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 4, 0, 0)[ MakeRichContentFoldout(RichSection) ];

	// Snippet preview (read-only, token-masked) + Copy — shown ONLY for the Custom agent (§7, issue #56). Built-in
	// agents write their config file via the status-row Configure button, so the raw snippet text is noise there.
	if (bShowSnippet)
	{
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SMultiLineEditableTextBox)
			.IsReadOnly(true)
			.AlwaysShowScrollbars(false)
			.Text_Lambda([this, bStdio]() { return FText::FromString(BuildSnippetPreview(bStdio)); })
		];
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("CopySnippet", "Copy snippet"))
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
		];
	}

	// Skills section (§7/§53 Phase C) — shown ONLY when the selected agent supports skills.
	{
		const FString ProjectRoot = ProjectRootProvider ? ProjectRootProvider() : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		if (Selected->SupportsSkills(ProjectRoot))
		{
			AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 4, 0, 0)[ SNew(SSeparator) ];
			AgentPanelContainer->AddSlot().AutoHeight()[ BuildSkillsSection() ];
		}
	}
	// NOTE: Remove now lives inside the configuration-status row (MakeConfigurationStatusRow) for built-in agents
	// (Unity's ConfigurationElements). The snippet-only Custom agent has no on-disk file to remove, so no extra
	// bottom Remove button is added.
}

TSharedRef<SWidget> SUnrealMcpAgentConfigurators::MakeConfigurationStatusRow(bool bStdio)
{
	// The Slate analog of Unity's ConfigurationElements: a status label ("Configured (transport)" / "Not
	// configured") + a Configure/Reconfigure button + a Remove button (shown only when something is detected). The
	// transport word matches the active transport. After any write, BindSelected() rebuilds the cached configs so
	// the next status read reflects the file (the lambdas re-read Selected on each paint, so the row self-updates).
	const FString TransportWord = bStdio ? TEXT("stdio") : TEXT("http");

	return SNew(SHorizontalBox)
		// Status label.
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text_Lambda([this, bStdio, TransportWord]()
			{
				if (!Selected.IsValid())
					return FText::GetEmpty();
				return Selected->IsConfigured(bStdio)
					? FText::Format(LOCTEXT("StatusConfiguredFmt", "Configured ({0})"), FText::FromString(TransportWord))
					: LOCTEXT("StatusNotConfigured", "Not configured");
			})
			.ColorAndOpacity_Lambda([this, bStdio]()
			{
				return (Selected.IsValid() && Selected->IsConfigured(bStdio))
					? FSlateColor(UnrealMcpAgentWidgets::ConfiguredColor())
					: FSlateColor(UnrealMcpAgentWidgets::PendingColor());
			})
		]
		// Configure / Reconfigure.
		+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
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
					BindSelected(); // rebuild configs after the write so the status row re-reads the file
				}
				return FReply::Handled();
			})
		]
		// Remove (clears BOTH transports' entries; visible only when something is detected, per Unity).
		+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(LOCTEXT("RemoveAgent", "Remove"))
			.Visibility_Lambda([this]()
			{
				return (Selected.IsValid() && Selected->IsAnyDetected()) ? EVisibility::Visible : EVisibility::Collapsed;
			})
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

TSharedRef<SWidget> SUnrealMcpAgentConfigurators::MakeRichContentFoldout(const FAiAgentRichContentSection& Section)
{
	// Build the foldout body from the section's items, each mapped to its widget template (the data→view mapping
	// described on FAiAgentRichContentItem). Then wrap in a collapsible area (expanded if the section is the "first").
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
	for (const FAiAgentRichContentItem& Item : Section.Items)
	{
		TSharedRef<SWidget> ItemWidget = SNullWidget::NullWidget;
		switch (Item.Kind)
		{
			case FAiAgentRichContentItem::EKind::Description:
				ItemWidget = UnrealMcpAgentWidgets::TemplateLabelDescription(FText::FromString(Item.Text));
				break;
			case FAiAgentRichContentItem::EKind::Warning:
				ItemWidget = UnrealMcpAgentWidgets::TemplateWarningLabel(FText::FromString(Item.Text));
				break;
			case FAiAgentRichContentItem::EKind::Alert:
				ItemWidget = UnrealMcpAgentWidgets::TemplateAlertLabel(FText::FromString(Item.Text));
				break;
			case FAiAgentRichContentItem::EKind::ReadOnlyField:
				ItemWidget = UnrealMcpAgentWidgets::TemplateTextFieldReadOnly(Item.Text);
				break;
		}
		Body->AddSlot().AutoHeight().Padding(0, 2, 0, 0)[ ItemWidget ];
	}

	return UnrealMcpAgentWidgets::TemplateFoldout(
		FText::FromString(Section.Heading), Body, Section.bExpandedFirst);
}

FString SUnrealMcpAgentConfigurators::ResolveSelectedSkillsPath() const
{
	if (!Selected.IsValid())
		return FString();
	const FString ProjectRoot = ProjectRootProvider ? ProjectRootProvider() : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	return Selected->ResolveAbsoluteSkillsPath(ProjectRoot);
}

FString SUnrealMcpAgentConfigurators::MakeDisplayPath(const FString& AbsolutePath) const
{
	if (AbsolutePath.IsEmpty())
		return AbsolutePath;

	FString ProjectRoot = ProjectRootProvider ? ProjectRootProvider() : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	ProjectRoot = FPaths::ConvertRelativePathToFull(ProjectRoot);
	if (ProjectRoot.IsEmpty())
		return AbsolutePath;

	// MakePathRelativeTo mutates its first arg in place and needs a trailing slash on the base (the pattern used in
	// Tools/UnrealMcpSourceTools.cpp). When the path is not under the root it leaves a "../"-style result; fall back
	// to the absolute path in that case rather than show a confusing climb-out.
	FString Relative = AbsolutePath;
	FPaths::MakePathRelativeTo(Relative, *(ProjectRoot / TEXT("")));
	if (Relative.IsEmpty() || Relative.StartsWith(TEXT("..")))
		return AbsolutePath;
	return Relative;
}

void SUnrealMcpAgentConfigurators::GenerateSkillsForSelected()
{
	const FString SkillsRoot = ResolveSelectedSkillsPath();
	if (SkillsRoot.IsEmpty())
	{
		UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] cannot generate skills: selected agent has no skills path."));
		return;
	}

	// Enumerate the full core tool set from a bare registry (headless-safe; the writer sources docs from the C++ tool
	// registry, NOT a live bridge). Token discipline (§8): skill files carry only tool docs — no token is read/written.
	FUnrealMcpToolRegistry ToolRegistry;
	FUnrealMcpSkillFileGenerator::PopulateCoreRegistry(ToolRegistry);
	const FUnrealMcpSkillFileGenerator::FResult Result = FUnrealMcpSkillFileGenerator::Generate(ToolRegistry, SkillsRoot);

	UE_LOG(LogUnrealMcp, Log, TEXT("[Unreal-MCP] agent '%s' skills generate %s (%d file(s) -> %s)."),
		Selected.IsValid() ? *Selected->GetAgentName() : TEXT("?"),
		Result.bSuccess ? TEXT("succeeded") : TEXT("failed"), Result.FilesWritten, *SkillsRoot);
}

TSharedRef<SWidget> SUnrealMcpAgentConfigurators::BuildSkillsSection()
{
	const bool bIsCustom = Selected.IsValid() && Selected->GetAgentId() == TEXT("other-custom");
	const FString AgentId = Selected.IsValid() ? Selected->GetAgentId() : FString();
	const FString AbsolutePath = ResolveSelectedSkillsPath();

	TSharedRef<SVerticalBox> Section = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SkillsHeader", "Skills")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT("SkillsDesc", "Generate per-tool skill documentation (one SKILL.md per Unreal MCP tool) for this agent."))
		]
		// Auto-generate toggle (per-agent; persisted via the view-model). Turning it on generates immediately.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, AgentId]()
			{
				return (IsViewModelValid() && ViewModel->IsAutoGenerateSkills(AgentId))
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, AgentId](ECheckBoxState NewState)
			{
				const bool bEnabled = (NewState == ECheckBoxState::Checked);
				if (IsViewModelValid())
					ViewModel->SetAutoGenerateSkills(AgentId, bEnabled);
				if (bEnabled)
					GenerateSkillsForSelected(); // auto-generate on enable (mirrors Unity's toggle callback)
			})
			[
				SNew(STextBlock).Text(LOCTEXT("AutoGenSkills", "Auto-generate skills"))
			]
		];

	// Output path: a read-only display for built-in agents; an editable field for the Custom agent (its skills path
	// is user-overridable + persisted, mirroring Unity's editable Custom SkillsPath).
	if (bIsCustom)
	{
		Section->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("SkillsPathLabel", "Skills folder:"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.Text_Lambda([this]() { return IsViewModelValid() ? FText::FromString(ViewModel->GetSkillsPath()) : FText::GetEmpty(); })
				.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
				{
					if (IsViewModelValid())
						ViewModel->SetSkillsPath(NewText.ToString().TrimStartAndEnd());
					BindSelected();      // re-sync the Custom configurator's path
					RebuildAgentPanel(); // refresh the resolved-path display
				})
			]
		];
	}

	// The resolved output path, shown project-root-relative (e.g. ".claude/skills") for readability (§7, issue #56).
	// The full absolute path stays available as the tooltip so the user can still see exactly where files land.
	const FString DisplayPath = MakeDisplayPath(AbsolutePath);
	Section->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Text(FText::Format(LOCTEXT("SkillsOutFmt", "Output: {0}"), FText::FromString(DisplayPath)))
		.ToolTipText(FText::FromString(AbsolutePath))
	];

	// Generate button (explicit regeneration; idempotent).
	Section->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
	[
		SNew(SButton)
		.Text(LOCTEXT("GenerateSkills", "Generate"))
		.ToolTipText(LOCTEXT("GenerateSkillsHint", "Write one SKILL.md per Unreal MCP tool into the output folder (overwrites existing)."))
		.OnClicked_Lambda([this]()
		{
			GenerateSkillsForSelected();
			return FReply::Handled();
		})
	];

	return Section;
}

#undef LOCTEXT_NAMESPACE
