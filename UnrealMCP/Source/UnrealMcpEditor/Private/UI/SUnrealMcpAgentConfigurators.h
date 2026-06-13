// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Agents/AiAgentConfigurator.h"

class FUnrealMcpEditorViewModel;
class FAiAgentConfiguratorRegistry;
class FAiAgentConfigurator;
class SVerticalBox;
template <typename T> class SComboBox;

/**
 * The "AI Agent Configurators" Slate panel (docs/ARCHITECTURE.md §7) — the Unreal/Slate analog of Unity's
 * AiAgentConfigurators UI and Godot's AgentConfiguratorsPanel. Lets the user pick an external AI agent / MCP
 * client from a dropdown and auto-configure it to reach this project's MCP server by writing/merging that
 * client's config file (STDIO and HTTP forms), with live status + Configure/Reconfigure/Remove.
 *
 * State sources, kept thin (the view is dumb, the model owns state):
 *  - ViewModel — persists the dropdown selection (SelectedAgentId) and is the single config store.
 *  - ConnectionInfoProvider — yields the live FAiAgentConnectionInfo (server path / port / url / auth / token)
 *    so the assembled STDIO/HTTP snippets always reflect the current connection. Resolving this needs the
 *    runtime's bridge port + the §6 server path, which the panel cannot reach directly — hence a provider sink
 *    (mirrors the aux-windows PortStatusProvider pattern), wired by the runtime; a spec injects a stub.
 *  - ProjectRootProvider — yields the live project root for resolving per-agent config file paths.
 *
 * Token discipline (§8): the assembled snippet preview masks the bearer token by default; a per-agent Reveal
 * toggle shows the raw value only while explicitly enabled. Configure() and Copy write/copy the REAL token; the
 * raw value never reaches a log line. Switching agents resets the reveal toggle.
 *
 * Built with its own registry instance (fresh configurators), so it can be hosted as a section inside the main
 * window without sharing config-cache state with any spec's registry.
 */
class SUnrealMcpAgentConfigurators : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpAgentConfigurators) {}
		/** The shared view-model (owned by the runtime). Required for persistence. */
		SLATE_ARGUMENT(TSharedPtr<FUnrealMcpEditorViewModel>, ViewModel)
		/** Yields the live connection facts used to assemble the STDIO/HTTP snippets. Required for correct output. */
		SLATE_ARGUMENT(TFunction<FAiAgentConnectionInfo()>, ConnectionInfoProvider)
		/** Yields the live project root for per-agent config-file path resolution. Defaults to the project dir. */
		SLATE_ARGUMENT(TFunction<FString()>, ProjectRootProvider)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	TFunction<FAiAgentConnectionInfo()> ConnectionInfoProvider;
	TFunction<FString()> ProjectRootProvider;

	TSharedPtr<FAiAgentConfiguratorRegistry> Registry;
	// The dropdown's item source (agent display names, registry order).
	TArray<TSharedPtr<FString>> AgentNameItems;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> AgentCombo;
	// The container holding the currently-selected agent's per-agent panel (rebuilt on selection change).
	TSharedPtr<SVerticalBox> AgentPanelContainer;

	// The currently-selected configurator (resolved from the view-model's persisted SelectedAgentId, clamped).
	TSharedPtr<FAiAgentConfigurator> Selected;
	// Whether the snippet preview reveals the raw token (per-agent; reset on selection change). Default masked.
	bool bRevealToken = false;

	bool IsViewModelValid() const { return ViewModel.IsValid(); }

	// Re-resolve the selected configurator from the view-model + provider and rebuild its panel.
	void RefreshSelectedConfigurator();
	// (Re)build the per-agent panel for the current Selected configurator into AgentPanelContainer.
	void RebuildAgentPanel();
	// Bind the selected configurator to the live connection info + project root.
	void BindSelected();

	// The snippet preview text for a transport, masking the token unless bRevealToken (§8).
	FString BuildSnippetPreview(bool bStdio) const;
	// Mask the bearer token inside an assembled snippet string unless revealed.
	static FString MaskTokenInSnippet(const FString& Snippet, const FString& Token, bool bReveal);
};
