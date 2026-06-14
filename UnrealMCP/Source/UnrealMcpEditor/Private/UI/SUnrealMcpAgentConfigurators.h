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
	virtual ~SUnrealMcpAgentConfigurators() override;

private:
	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	TFunction<FAiAgentConnectionInfo()> ConnectionInfoProvider;
	TFunction<FString()> ProjectRootProvider;

	// Handle for the view-model's OnConnectionSettingsChanged subscription, removed in the destructor so a
	// connection-setting change after this widget is gone never invokes a dangling handler.
	FDelegateHandle ConnectionChangedHandle;

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
	// Handler for FUnrealMcpEditorViewModel::OnConnectionSettingsChanged: invalidate the active configurator's
	// cached config (re-resolved from fresh connection info) and rebuild the panel, so the shown snippet/status
	// AND what Configure() writes track the new connection settings without the user reselecting the agent.
	void OnConnectionSettingsChanged();
	// (Re)build the per-agent panel for the current Selected configurator into AgentPanelContainer.
	void RebuildAgentPanel();
	// Bind the selected configurator to the live connection info + project root.
	void BindSelected();

	// The configuration-status row for the active transport (Unity's ConfigurationElements): a
	// "Configured (transport)" / "Not configured" label + Configure/Reconfigure + Remove (#59).
	TSharedRef<SWidget> MakeConfigurationStatusRow(bool bStdio);
	// A collapsible foldout rendering one per-agent rich-content section with the reusable widget templates (#59).
	TSharedRef<SWidget> MakeRichContentFoldout(const struct FAiAgentRichContentSection& Section);

	// The snippet preview text for a transport, masking the token unless bRevealToken (§8).
	FString BuildSnippetPreview(bool bStdio) const;
	// Mask the bearer token inside an assembled snippet string unless revealed.
	static FString MaskTokenInSnippet(const FString& Snippet, const FString& Token, bool bReveal);

	// --- Skills section (issue #53 Phase C). Shown ONLY when the selected agent SupportsSkills. ---

	/** Build the per-agent skills section (toggle + resolved output path + Generate button, plus the Custom edit field). */
	TSharedRef<SWidget> BuildSkillsSection();
	/** Resolve the selected agent's absolute skills folder (project-relative resolved under the live project root). */
	FString ResolveSelectedSkillsPath() const;
	/**
	 * Convert an absolute filesystem path to a project-root-relative display string (forward slashes, no leading
	 * "./"); mirrors Unity's ToDisplayPath and the FPaths::MakePathRelativeTo pattern in UnrealMcpSourceTools.
	 * A path already outside / not under the project root, or an empty input, is returned unchanged (still useful
	 * as a fallback display). The full absolute path stays available as the widget's tooltip.
	 */
	FString MakeDisplayPath(const FString& AbsolutePath) const;
	/** Generate the SKILL.md files for the selected agent into its resolved skills folder (idempotent). Logs the result. */
	void GenerateSkillsForSelected();
};
