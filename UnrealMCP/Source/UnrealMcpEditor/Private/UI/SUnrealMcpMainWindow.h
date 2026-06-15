// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "Agents/AiAgentConfigurator.h"

/**
 * The "AI Game Developer" main window (docs/ARCHITECTURE.md §7), a pure-Slate compound widget — NO UMG /
 * editor-utility dependency. Hosted in the nomad dockable tab registered by FUnrealMcpMainWindowTab. Every
 * widget binds (via TAttribute lambdas) to the shared FUnrealMcpEditorViewModel, which owns all state; this
 * widget is a thin view. Sections, top to bottom: header/settings, connection panel, connection-mode toggle,
 * Cloud device-code auth, Custom server auth, bridge status, AI agents, footer.
 *
 * Lifetime: the view-model outlives the widget (it is owned by the runtime); the widget keeps only a weak-ish
 * shared ref and reads it under IsValid guards. The status feed is marshalled to the game thread by the
 * runtime before it reaches the view-model, so every Slate read here is already game-thread-safe.
 */
class SUnrealMcpMainWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpMainWindow) {}
		/** The shared view-model (owned by the runtime). Required. */
		SLATE_ARGUMENT(TSharedPtr<FUnrealMcpEditorViewModel>, ViewModel)
		/** Optional: plugin version string for the header. */
		SLATE_ARGUMENT(FString, PluginVersion)
		/** Optional: restart-bridge action (§7 item 1/7). */
		SLATE_EVENT(FSimpleDelegate, OnRestartBridge)
		/** Optional: a provider for the one-line bridge status string (§7 item 7). */
		SLATE_ARGUMENT(TFunction<FString()>, BridgeStatusProvider)
		/** Optional: yields the live connection facts for the AI Agent Configurators panel (§7/§8). */
		SLATE_ARGUMENT(TFunction<FAiAgentConnectionInfo()>, ConnectionInfoProvider)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	FString PluginVersion;
	FSimpleDelegate OnRestartBridge;
	TFunction<FString()> BridgeStatusProvider;
	TFunction<FAiAgentConnectionInfo()> ConnectionInfoProvider;

	// Whether the masked Custom-mode token field is currently revealed (reveal-on-hold, §8).
	bool bRevealToken = false;

	// Section builders (each returns a Slate widget; kept separate so §7's ordering reads top-to-bottom).
	// The window now mirrors the Unity-MCP reference (issue #78): a header card with base config (Log Level /
	// Timeout / Version) + the AI-cube logo, a unified Connection timeline card (Custom/Cloud segmented in the
	// header row, status dots + connecting line + underlined labels, teal Start / red Revoke / masked token +
	// New, stdio/http + none/required segmented), the AI Agent Configurators panel, an Extensions card, and a
	// styled footer (Discord / GitHub bug-report / gold GitHub Star).
	TSharedRef<SWidget> BuildHeaderSection();
	TSharedRef<SWidget> BuildConnectionSection();
	// The Cloud auth controls (masked token + Revoke/Authorize), shown inline in the Connection card in Cloud mode.
	TSharedRef<SWidget> BuildCloudAuthRow();
	// The Custom auth + MCP-server sub-card (Server URL, Start, Transport, none/required, masked token + New).
	TSharedRef<SWidget> BuildCustomServerSection();
	// The Custom-mode transport selector (stdio/http) segmented control.
	TSharedRef<SWidget> BuildTransportSelector();
	// The Custom-mode authorization selector (none/required) + masked token + New.
	TSharedRef<SWidget> BuildCustomAuthSelector();
	TSharedRef<SWidget> BuildAgentConfiguratorsSection();
	TSharedRef<SWidget> BuildExtensionsSection();
	TSharedRef<SWidget> BuildFooterSection();

	// The AI-cube logo brush, loaded once at Construct from the plugin's Resources dir
	// (Resources/ai-cube-logo.png) via the style set. Held as a member so the brush outlives the SImage.
	const FSlateBrush* LogoBrush = nullptr;

	// A label with a thin underline beneath it — the reference's underlined timeline labels (Slate has no
	// first-class text-underline, so the underline is a 1px separator drawn under the text).
	static TSharedRef<SWidget> UnderlinedLabel(const TAttribute<FText>& Text);

	// Common handlers.
	FReply OnConnectClicked();
	bool IsViewModelValid() const { return ViewModel.IsValid(); }
};
