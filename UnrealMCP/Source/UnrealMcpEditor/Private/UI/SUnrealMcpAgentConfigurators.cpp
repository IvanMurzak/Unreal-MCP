// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/SUnrealMcpAgentConfigurators.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UI/SUnrealMcpAgentWidgets.h"
#include "UnrealMcpLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Styling/CoreStyle.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

void SUnrealMcpAgentConfigurators::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;
	ConnectionInfoProvider = InArgs._ConnectionInfoProvider;
	ProjectRootProvider = InArgs._ProjectRootProvider;
	SendRequest = InArgs._SendRequest;

	if (!ProjectRootProvider)
		ProjectRootProvider = []() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()); };

	SelectedAgentId = IsViewModelValid() ? ViewModel->GetSelectedAgentId() : FString();

	// Mirror the prior panel's frame: a header ROW ("AI agent" + the agent dropdown), then the per-agent panel.
	// The dropdown's item source starts empty and is populated from the agents-list result.
	ChildSlot
	[
		UnrealMcpStyleWidgets::StyledCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[ UnrealMcpStyleWidgets::SectionHeader(LOCTEXT("AgentSectionHeader", "AI agent")) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(AgentCombo, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&AgentNameItems)
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
					{
						return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewItem, ESelectInfo::Type SelectInfo)
					{
						if (!NewItem.IsValid() || SelectInfo == ESelectInfo::Direct)
							return; // Direct = our own programmatic set; ignore to avoid a request loop
						const int32 Index = AgentNameItems.IndexOfByPredicate(
							[&NewItem](const TSharedPtr<FString>& I) { return I == NewItem; });
						if (AgentIds.IsValidIndex(Index))
						{
							SetSelectedAgentId(AgentIds[Index]);
							RequestSelectedStatus();
						}
					})
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return bHaveDescription && !SelectedDescription.AgentName.IsEmpty()
								? FText::FromString(SelectedDescription.AgentName)
								: LOCTEXT("NoAgent", "Select an agent");
						})
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SAssignNew(AgentPanelContainer, SVerticalBox)
			])
	];

	// Subscribe to connection-setting changes so the panel re-queries when the user edits the connection mode /
	// host / auth / token — without reselecting the agent (the §7 analog of Unity's InvalidateAndReloadAgentUI).
	if (IsViewModelValid())
		ConnectionChangedHandle = ViewModel->OnConnectionSettingsChanged.AddSP(this, &SUnrealMcpAgentConfigurators::Refresh);

	RebuildAgentPanel();
	// Query the agent set + the persisted selection's status on open. Degrades gracefully when the sidecar is down.
	Refresh();
}

SUnrealMcpAgentConfigurators::~SUnrealMcpAgentConfigurators()
{
	if (ViewModel.IsValid() && ConnectionChangedHandle.IsValid())
		ViewModel->OnConnectionSettingsChanged.Remove(ConnectionChangedHandle);
}

// --- Request building / sending --------------------------------------------------------------------

FString SUnrealMcpAgentConfigurators::NewRequestId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FString SUnrealMcpAgentConfigurators::EffectiveTransportString() const
{
	const EUnrealMcpTransportMethod Transport = IsViewModelValid()
		? ViewModel->GetEffectiveTransport()
		: EUnrealMcpTransportMethod::Http;
	return Transport == EUnrealMcpTransportMethod::Stdio ? TEXT("stdio") : TEXT("streamableHttp");
}

TSharedPtr<FJsonObject> SUnrealMcpAgentConfigurators::BuildSettings() const
{
	const FAiAgentConnectionInfo Connection = ConnectionInfoProvider ? ConnectionInfoProvider() : FAiAgentConnectionInfo();
	const FString ProjectRoot = ProjectRootProvider ? ProjectRootProvider() : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// The mode string mirrors the sidecar's expectation (case-insensitive). Cloud forces auth; otherwise the
	// resolved bAuthRequired carries the Custom AuthOption.
	const bool bCloud = IsViewModelValid() && ViewModel->GetConnectionMode() == EUnrealMcpConnectionMode::Cloud;

	TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetStringField(TEXT("projectRootPath"), ProjectRoot);
	Settings->SetStringField(TEXT("executableFullPath"), Connection.ServerPath);
	Settings->SetNumberField(TEXT("port"), Connection.Port);
	Settings->SetNumberField(TEXT("timeoutMs"), 30000);
	Settings->SetStringField(TEXT("host"), Connection.HttpUrl);
	// mcp-authorize PR 5 (design 06, D11): the DEFAULT path is native MCP OAuth — the sidecar writes a
	// credential-free config, so DON'T forward the bearer there ("HTTP configs: stop embedding tokens"). The REAL
	// bearer is forwarded ONLY on the "Advanced: use access token" escape hatch (Flow C), where the sidecar injects
	// it into the legacy Bearer shape. It is NEVER logged here.
	Settings->SetStringField(TEXT("token"), Connection.bUseAccessToken ? Connection.Token : FString());
	Settings->SetStringField(TEXT("connectionMode"), bCloud ? TEXT("Cloud") : TEXT("Local"));
	Settings->SetBoolField(TEXT("authRequired"), Connection.bAuthRequired);
	// The explicit escape-hatch opt-in — the sidecar keys the HttpCredentialMode.AccessToken path off this.
	Settings->SetBoolField(TEXT("useAccessToken"), Connection.bUseAccessToken);
	return Settings;
}

bool SUnrealMcpAgentConfigurators::SendTracked(const TSharedPtr<FJsonObject>& Request, const FString& RequestType, EPhase InFlightPhase)
{
	const FString RequestId = NewRequestId();
	Request->SetStringField(TEXT("type"), RequestType);
	Request->SetStringField(TEXT("requestId"), RequestId);

	const bool bSent = SendRequest ? SendRequest(Request) : false;
	if (!bSent)
	{
		// Graceful degrade (§7 / PR #100 lesson): no sidecar to serve the request. Show the clear "bridge not
		// connected" state and clear any stale in-flight tracking; Refresh() runs again on the next handshake.
		Phase = EPhase::Disconnected;
		PendingRequestId.Reset();
		PendingRequestType.Reset();
		RebuildAgentPanel();
		return false;
	}

	Phase = InFlightPhase;
	PendingRequestId = RequestId;
	PendingRequestType = RequestType;
	LastError.Reset();
	RebuildAgentPanel();
	return true;
}

void SUnrealMcpAgentConfigurators::RequestAgentsList()
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("transport"), EffectiveTransportString());
	Request->SetObjectField(TEXT("settings"), BuildSettings());
	SendTracked(Request, TEXT("agents-list"), EPhase::Loading);
}

void SUnrealMcpAgentConfigurators::RequestSelectedStatus()
{
	if (SelectedAgentId.IsEmpty())
		return;
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("agentId"), SelectedAgentId);
	Request->SetStringField(TEXT("transport"), EffectiveTransportString());
	Request->SetObjectField(TEXT("settings"), BuildSettings());
	SendTracked(Request, TEXT("agent-status"), EPhase::Loading);
}

void SUnrealMcpAgentConfigurators::RequestConfigure()
{
	if (SelectedAgentId.IsEmpty())
		return;
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("agentId"), SelectedAgentId);
	Request->SetStringField(TEXT("transport"), EffectiveTransportString());
	Request->SetObjectField(TEXT("settings"), BuildSettings());
	SendTracked(Request, TEXT("agent-configure"), EPhase::Pending);
}

void SUnrealMcpAgentConfigurators::RequestRemove()
{
	if (SelectedAgentId.IsEmpty())
		return;
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("agentId"), SelectedAgentId);
	Request->SetStringField(TEXT("transport"), EffectiveTransportString());
	Request->SetObjectField(TEXT("settings"), BuildSettings());
	SendTracked(Request, TEXT("agent-remove"), EPhase::Pending);
}

void SUnrealMcpAgentConfigurators::RequestGenerateSkills()
{
	if (SelectedAgentId.IsEmpty())
		return;
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("agentId"), SelectedAgentId);
	// The Custom agent's skills path is user-overridable + persisted; forward it so the sidecar writes there.
	if (IsViewModelValid() && SelectedAgentId == TEXT("other-custom"))
		Request->SetStringField(TEXT("customSkillsPath"), ViewModel->GetSkillsPath());
	Request->SetObjectField(TEXT("settings"), BuildSettings());
	SendTracked(Request, TEXT("agent-generate-skills"), EPhase::Pending);
}

void SUnrealMcpAgentConfigurators::WriteBackEditableValue(const FString& NewValue)
{
	if (SelectedAgentId.IsEmpty())
		return;
	// The Custom agent's editable config/skills path. Persist it view-model-side (so it survives a restart and the
	// settings DTO carries it), then re-query status carrying the edited value so the rendered snippet updates.
	if (IsViewModelValid() && SelectedAgentId == TEXT("other-custom"))
		ViewModel->SetSkillsPath(NewValue);

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("agentId"), SelectedAgentId);
	Request->SetStringField(TEXT("transport"), EffectiveTransportString());
	Request->SetStringField(TEXT("editableValue"), NewValue);
	Request->SetObjectField(TEXT("settings"), BuildSettings());
	// A write-back re-describes the agent (the Custom agent has no file to write, so this is effectively a status
	// refresh that echoes the edited value into the snippet). Use agent-configure so a writable agent also persists.
	SendTracked(Request, TEXT("agent-configure"), EPhase::Pending);
}

void SUnrealMcpAgentConfigurators::Refresh()
{
	// Always re-list (the set is cheap + lets the dropdown populate) and, when an agent is selected, its status.
	// agents-list is the in-flight request we track; the selected-status request follows when the list returns.
	RequestAgentsList();
}

// --- Result handling -------------------------------------------------------------------------------

void SUnrealMcpAgentConfigurators::OnAgentConfigResult(const TSharedPtr<FJsonObject>& Result)
{
	if (!Result.IsValid())
		return;

	FString RequestId, Op;
	Result->TryGetStringField(TEXT("requestId"), RequestId);
	Result->TryGetStringField(TEXT("op"), Op);

	// Ignore a stale/late result for a request we no longer await (the user superseded it). An empty PendingRequestId
	// means nothing is in flight — but a list/status result can still arrive from a Refresh race, so accept results
	// whose requestId matches OR when we are not tracking anything (best-effort late delivery is harmless to render).
	if (!PendingRequestId.IsEmpty() && RequestId != PendingRequestId)
		return;

	bool bOk = false;
	Result->TryGetBoolField(TEXT("ok"), bOk);
	FString Error;
	Result->TryGetStringField(TEXT("error"), Error);

	// Clear the in-flight tracking now that its result arrived.
	const FString CompletedType = PendingRequestType;
	PendingRequestId.Reset();
	PendingRequestType.Reset();
	LastError = bOk ? FString() : Error;

	if (Op == TEXT("agents-list"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Agents = nullptr;
		if (bOk && Result->TryGetArrayField(TEXT("agents"), Agents) && Agents)
		{
			AgentNameItems.Reset();
			AgentIds.Reset();
			for (const TSharedPtr<FJsonValue>& AgentValue : *Agents)
			{
				const TSharedPtr<FJsonObject>* AgentObj = nullptr;
				if (!AgentValue.IsValid() || !AgentValue->TryGetObject(AgentObj) || !AgentObj)
					continue;
				FString Id, Name;
				(*AgentObj)->TryGetStringField(TEXT("agentId"), Id);
				(*AgentObj)->TryGetStringField(TEXT("agentName"), Name);
				AgentIds.Add(Id);
				AgentNameItems.Add(MakeShared<FString>(Name));
			}
			if (AgentCombo.IsValid())
				AgentCombo->RefreshOptions();

			// Resolve the selection: keep the persisted one if present, else default to the first agent.
			if (IndexOfAgentId(SelectedAgentId) == INDEX_NONE && AgentIds.Num() > 0)
				SetSelectedAgentId(AgentIds[0]);

			Phase = EPhase::Idle;
			// Now fetch the selected agent's detailed status (its sections/buttons).
			RequestSelectedStatus();
			return; // RequestSelectedStatus already rebuilt the panel into Loading
		}
		Phase = EPhase::Idle;
		RebuildAgentPanel();
		return;
	}

	if (Op == TEXT("agent-status") || Op == TEXT("agent-configure") || Op == TEXT("agent-remove"))
	{
		const TSharedPtr<FJsonObject>* DescObj = nullptr;
		if (Result->TryGetObjectField(TEXT("description"), DescObj) && DescObj)
		{
			SelectedDescription = FUnrealMcpAgentDescription::FromJson(*DescObj);
			bHaveDescription = true;
		}
		// A configure/remove that returned ok==false (e.g. the Custom agent has no writable file) still surfaces
		// its reason via LastError; the description (if any) re-renders the current state.
		Phase = EPhase::Idle;
		RebuildAgentPanel();
		return;
	}

	if (Op == TEXT("agent-generate-skills"))
	{
		Result->TryGetStringField(TEXT("skillsPath"), LastSkillsPath);
		if (bOk)
		{
			int32 Written = 0, Pruned = 0;
			Result->TryGetNumberField(TEXT("filesWritten"), Written);
			Result->TryGetNumberField(TEXT("filesPruned"), Pruned);
			LastSkillsStatus = FString::Printf(TEXT("Generated %d skill file(s)%s."),
				Written, Pruned > 0 ? *FString::Printf(TEXT(" (pruned %d stale)"), Pruned) : TEXT(""));
			LastError.Reset();
		}
		else
		{
			LastSkillsStatus = Error.IsEmpty() ? TEXT("Skill generation failed.") : Error;
		}
		Phase = EPhase::Idle;
		RebuildAgentPanel();
		return;
	}

	// Other ops (e.g. agent-skills-path) — currently the panel does not issue them from here; ignore safely.
	Phase = EPhase::Idle;
	RebuildAgentPanel();
}

// --- Selection helpers -----------------------------------------------------------------------------

int32 SUnrealMcpAgentConfigurators::IndexOfAgentId(const FString& InAgentId) const
{
	return AgentIds.IndexOfByKey(InAgentId);
}

void SUnrealMcpAgentConfigurators::SetSelectedAgentId(const FString& InAgentId)
{
	SelectedAgentId = InAgentId;
	bHaveDescription = false;
	SelectedDescription = FUnrealMcpAgentDescription();
	LastSkillsStatus.Reset();   // a prior agent's generation outcome is meaningless for the new selection
	LastSkillsPath.Reset();
	if (IsViewModelValid())
		ViewModel->SetSelectedAgentId(InAgentId);

	// Keep the combo's visible selection in sync (Direct select-info is ignored by the handler, so no loop).
	const int32 Index = IndexOfAgentId(InAgentId);
	if (AgentCombo.IsValid() && AgentNameItems.IsValidIndex(Index))
		AgentCombo->SetSelectedItem(AgentNameItems[Index]);
}

// --- Rendering -------------------------------------------------------------------------------------

void SUnrealMcpAgentConfigurators::RebuildAgentPanel()
{
	if (!AgentPanelContainer.IsValid())
		return;

	AgentPanelContainer->ClearChildren();

	if (Phase == EPhase::Disconnected)
	{
		AgentPanelContainer->AddSlot().AutoHeight()
		[
			UnrealMcpAgentWidgets::TemplateWarningLabel(LOCTEXT("BridgeDown",
				"The MCP bridge is not connected. Connect (or start the bridge) to configure AI agents — this panel will refresh automatically."))
		];
		return;
	}

	if (Phase == EPhase::Loading && !bHaveDescription)
	{
		AgentPanelContainer->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(LOCTEXT("LoadingAgents", "Loading AI agents…"))
		];
		return;
	}

	if (!bHaveDescription)
	{
		AgentPanelContainer->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(LOCTEXT("NoAgentSelected", "No agent selected."))
		];
		return;
	}

	// Links row — the 6.9.0 top-level Link items (download / tutorial / docs) as clickable hyperlinks, with a
	// graceful fallback to the legacy DownloadUrl/TutorialUrl buttons when an older sidecar sends no Links.
	AgentPanelContainer->AddSlot().AutoHeight()[ MakeLinksRow() ];

	// Configuration-status row (Configured / Not configured + Configure/Reconfigure + Remove). The Custom agent has
	// no detectable on-disk config (sidecar reports IsConfigured=false, configure returns a clear non-success), so
	// only its rich-content snippet + EditableField drive it — still show the row, it self-disables sensibly.
	AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 6, 0, 0)[ MakeStatusRow() ];

	// A pending/working line while a configure/remove is in flight.
	if (Phase == EPhase::Pending)
	{
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor(UnrealMcpAgentWidgets::PendingColor()))
			.Text(LOCTEXT("Working", "Working…"))
		];
	}

	// A last-error line (e.g. the Custom agent's "no writable config file" reason).
	if (!LastError.IsEmpty())
	{
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			UnrealMcpAgentWidgets::TemplateAlertLabel(FText::FromString(LastError))
		];
	}

	// Per-agent rich content: the sidecar's DTO sections, rendered with the existing reusable widget templates.
	for (const FAiAgentRichContentSection& Section : SelectedDescription.Sections)
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 4, 0, 0)[ MakeRichContentFoldout(Section) ];

	// Skills section (§7) — shown only when the sidecar reports the agent supports skills. A thin view: the
	// Generate button sends the agent-generate-skills IPC request; the sidecar resolves the path + writes the
	// SKILL.md files (sourced from the tool manifest). No C++ generation logic here.
	if (SelectedDescription.bSupportsSkills)
	{
		AgentPanelContainer->AddSlot().AutoHeight().Padding(0, 6, 0, 0)[ SNew(SSeparator) ];
		AgentPanelContainer->AddSlot().AutoHeight()[ MakeSkillsSection() ];
	}
}

TSharedRef<SWidget> SUnrealMcpAgentConfigurators::MakeSkillsSection()
{
	const bool bBusy = Phase == EPhase::Pending || Phase == EPhase::Loading;

	TSharedRef<SVerticalBox> Section = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SkillsHeader", "Skills"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT("SkillsDesc", "Generate per-tool skill documentation (one SKILL.md per Unreal MCP tool) for this agent. The MCP bridge writes the files from the live tool set."))
		]
		// Generate button (sends the IPC request; the sidecar does the writing).
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(SButton)
			.IsEnabled(!bBusy)
			.Text(LOCTEXT("GenerateSkills", "Generate skill files"))
			.ToolTipText(LOCTEXT("GenerateSkillsHint", "Ask the MCP bridge to write one SKILL.md per Unreal MCP tool into this agent's skills folder."))
			.OnClicked_Lambda([this]() { RequestGenerateSkills(); return FReply::Handled(); })
		];

	// The resolved output path (from the last run) + the last-run status line, when present.
	if (!LastSkillsPath.IsEmpty())
	{
		Section->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(FText::Format(LOCTEXT("SkillsOutFmt", "Output: {0}"), FText::FromString(LastSkillsPath)))
		];
	}
	if (!LastSkillsStatus.IsEmpty())
	{
		Section->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(FText::FromString(LastSkillsStatus))
		];
	}

	return Section;
}

TSharedRef<SWidget> SUnrealMcpAgentConfigurators::MakeLinksRow()
{
	TSharedRef<SHorizontalBox> LinksRow = SNew(SHorizontalBox);

	// Preferred (6.9.0): the top-level Link items, each opening its Url. Rendered as inline hyperlinks.
	if (SelectedDescription.Links.Num() > 0)
	{
		bool bFirst = true;
		for (const FAiAgentRichContentItem& Link : SelectedDescription.Links)
		{
			if (Link.Url.IsEmpty() || Link.Url == TEXT("NA"))
				continue; // the Custom agent's "NA" download — nothing to open
			LinksRow->AddSlot().AutoWidth().Padding(bFirst ? FMargin(0) : FMargin(12, 0, 0, 0)).VAlign(VAlign_Center)
			[
				UnrealMcpAgentWidgets::TemplateLink(FText::FromString(Link.Text), Link.Url)
			];
			bFirst = false;
		}
		return LinksRow;
	}

	// Fallback (older sidecar with no Links): the legacy DownloadUrl/TutorialUrl button pair.
	const FString DownloadUrl = SelectedDescription.DownloadUrl;
	const FString TutorialUrl = SelectedDescription.TutorialUrl;
	if (!DownloadUrl.IsEmpty() && DownloadUrl != TEXT("NA"))
	{
		LinksRow->AddSlot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("DownloadAgent", "Download / Docs"))
			.OnClicked(UnrealMcpStyleWidgets::OpenUrlClicked(DownloadUrl))
		];
	}
	if (!TutorialUrl.IsEmpty())
	{
		LinksRow->AddSlot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("TutorialAgent", "Tutorial"))
			.OnClicked(UnrealMcpStyleWidgets::OpenUrlClicked(TutorialUrl))
		];
	}
	return LinksRow;
}

TSharedRef<SWidget> SUnrealMcpAgentConfigurators::MakeStatusRow()
{
	const bool bConfigured = SelectedDescription.bIsConfigured;
	const bool bBusy = Phase == EPhase::Pending || Phase == EPhase::Loading;
	const FString TransportWord = EffectiveTransportString() == TEXT("stdio") ? TEXT("stdio") : TEXT("http");
	// 6.9.0: drive the action-button label off the tri-state status — "Reconfigure" when an entry exists but is
	// stale, else "Configure". (When ReconfigureNeeded the sidecar also prepends a "Reconfiguration Required" Alert
	// section, rendered below as an Alert-kind item.) Falls back to bIsConfigured if Status is absent (older sidecar).
	const bool bReconfigure = SelectedDescription.Status == EAiAgentConfiguratorStatus::ReconfigureNeeded || bConfigured;

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(bConfigured
				? FText::Format(LOCTEXT("StatusConfiguredFmt", "Configured ({0})"), FText::FromString(TransportWord))
				: LOCTEXT("StatusNotConfigured", "Not configured"))
			.ColorAndOpacity(FSlateColor(bConfigured
				? UnrealMcpAgentWidgets::ConfiguredColor()
				: UnrealMcpAgentWidgets::PendingColor()))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
		[
			SNew(SButton)
			.IsEnabled(!bBusy)
			.Text(bReconfigure ? LOCTEXT("Reconfigure", "Reconfigure") : LOCTEXT("Configure", "Configure"))
			.OnClicked_Lambda([this]() { RequestConfigure(); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
		[
			SNew(SButton)
			.IsEnabled(!bBusy)
			.Visibility(bConfigured ? EVisibility::Visible : EVisibility::Collapsed)
			.Text(LOCTEXT("RemoveAgent", "Remove"))
			.OnClicked_Lambda([this]() { RequestRemove(); return FReply::Handled(); })
		];
}

TSharedRef<SWidget> SUnrealMcpAgentConfigurators::MakeItemWidget(const FAiAgentRichContentItem& Item)
{
	switch (Item.Kind)
	{
		case FAiAgentRichContentItem::EKind::Description:
			return UnrealMcpAgentWidgets::TemplateLabelDescription(FText::FromString(Item.Text));
		case FAiAgentRichContentItem::EKind::Warning:
			return UnrealMcpAgentWidgets::TemplateWarningLabel(FText::FromString(Item.Text));
		case FAiAgentRichContentItem::EKind::Alert:
			return UnrealMcpAgentWidgets::TemplateAlertLabel(FText::FromString(Item.Text));
		case FAiAgentRichContentItem::EKind::ReadOnlyField:
			return UnrealMcpAgentWidgets::TemplateTextFieldReadOnly(Item.Text);
		case FAiAgentRichContentItem::EKind::Link:
			// 6.9.0: a clickable hyperlink opening Url (FPlatformProcess::LaunchURL). Used inside a section when the
			// sidecar emits a Link item there; the top-level Links collection renders via MakeLinksRow.
			return UnrealMcpAgentWidgets::TemplateLink(FText::FromString(Item.Text), Item.Url);
		case FAiAgentRichContentItem::EKind::EditableField:
		{
			// An editable value the user can change (the Custom agent's editable config/skills path). On commit,
			// write it back to the sidecar over IPC (and persist it view-model-side). Seed with the current text.
			const FString Initial = Item.Text;
			return SNew(SEditableTextBox)
				.Text(FText::FromString(Initial))
				.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type CommitType)
				{
					if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
						WriteBackEditableValue(NewText.ToString().TrimStartAndEnd());
				});
		}
		default:
			checkNoEntry(); // Exhaustive over EKind — a new kind must add a case here.
			return SNullWidget::NullWidget;
	}
}

TSharedRef<SWidget> SUnrealMcpAgentConfigurators::MakeRichContentFoldout(const FAiAgentRichContentSection& Section)
{
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
	for (const FAiAgentRichContentItem& Item : Section.Items)
		Body->AddSlot().AutoHeight().Padding(0, 2, 0, 0)[ MakeItemWidget(Item) ];

	return UnrealMcpAgentWidgets::TemplateFoldout(
		FText::FromString(Section.Heading), Body, Section.bExpandedFirst);
}

#undef LOCTEXT_NAMESPACE
