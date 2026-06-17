// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "UI/UnrealMcpAgentConfigModels.h"

class FUnrealMcpEditorViewModel;
class FJsonObject;
class SVerticalBox;
template <typename T> class SComboBox;

/**
 * The "AI Agent Configurators" Slate panel (docs/ARCHITECTURE.md §7) — a THIN VIEW over the C# sidecar's
 * AI-agent configuration service (issue #101). All configurator LOGIC (which agents exist, where each writes its
 * config, configure/remove/status, the per-transport UI content) lives in the shared
 * <c>com.IvanMurzak.McpPlugin.AgentConfig</c> library and is served by the sidecar over IPC. This panel only:
 *
 *   - sends requests (`agents-list` / `agent-status` / `agent-configure` / `agent-remove` / `agent-skills-path`)
 *     over IPC via the injected SendRequest sink (wired to FUnrealMcpBridgeServer::SendAgentConfigMessage);
 *   - parses the `agent-config-result` DTO into FUnrealMcpAgentDescription / FAiAgentRichContentSection and
 *     renders it with the existing reusable widget templates (SUnrealMcpAgentWidgets);
 *   - drives an async pending/refresh state machine: Configure/Remove enter a pending state, the sidecar performs
 *     the write and returns the refreshed status, the panel re-renders; status is (re)queried on panel open and
 *     after each action, and whenever the connection settings change;
 *   - renders the EditableField kind (the Custom agent's editable config/skills path) and writes a committed
 *     value back over IPC;
 *   - degrades gracefully when the sidecar is not connected (the SendRequest sink returns false): it shows a
 *     clear "bridge not connected" state and re-requests on the next handshake, instead of erroring.
 *
 * NO C++ configurator subsystem (Agents/) is referenced — it was removed in #101. The panel keeps its own thin
 * UI shell only. Agent ICONS stay plugin-side conceptually (the DTO carries the icon NAME only; the plugin would
 * resolve the name to a brush) — there are no agent-icon brush assets shipped today, so the name is carried in
 * the model for forward compatibility but no icon is drawn (parity with the prior panel, which drew none).
 *
 * Threading (M9b): SendRequest is invoked on the game thread; OnAgentConfigResult MUST be called on the game
 * thread by the caller (the runtime marshals the IPC reader-thread `agent-config-result` onto the game thread
 * before delivering it here).
 */
class SUnrealMcpAgentConfigurators : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpAgentConfigurators) {}
		/** The shared view-model (owned by the runtime). Used to persist the dropdown selection + read the skills path. */
		SLATE_ARGUMENT(TSharedPtr<FUnrealMcpEditorViewModel>, ViewModel)
		/** Yields the live connection facts forwarded to the sidecar as the per-request settings. Required for correct output. */
		SLATE_ARGUMENT(TFunction<FAiAgentConnectionInfo()>, ConnectionInfoProvider)
		/** Yields the live project root for the settings DTO. Defaults to the project dir. */
		SLATE_ARGUMENT(TFunction<FString()>, ProjectRootProvider)
		/**
		 * Sends an agent-config request JSON object to the sidecar over IPC. Returns true when the frame reached a
		 * connected/handshaken sidecar; false when there is no sidecar (the panel then shows the graceful
		 * "bridge not connected" state and retries on the next handshake). Wired by the runtime to
		 * FUnrealMcpBridgeServer::SendAgentConfigMessage; a spec injects a recording stub.
		 */
		SLATE_ARGUMENT(TFunction<bool(const TSharedPtr<FJsonObject>&)>, SendRequest)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SUnrealMcpAgentConfigurators() override;

	/**
	 * Deliver a sidecar `agent-config-result` (§7) to the panel. Called ON THE GAME THREAD by the runtime after it
	 * marshals the IPC message off the reader thread. Correlates by the requestId the panel issued; an unknown /
	 * stale requestId is ignored. Updates the agent list / selected-agent description / skills path and re-renders.
	 * Public so a spec drives the async state machine directly with a synthetic result.
	 */
	UNREALMCPEDITOR_API void OnAgentConfigResult(const TSharedPtr<FJsonObject>& Result);

	/**
	 * Re-query the agent set + the selected agent's status from the sidecar (panel open / handshake / a connection
	 * setting changed). Idempotent and safe to call when the sidecar is down — it just enters the graceful state.
	 */
	UNREALMCPEDITOR_API void Refresh();

private:
	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	TFunction<FAiAgentConnectionInfo()> ConnectionInfoProvider;
	TFunction<FString()> ProjectRootProvider;
	TFunction<bool(const TSharedPtr<FJsonObject>&)> SendRequest;

	// Subscription to the view-model's OnConnectionSettingsChanged, removed in the destructor.
	FDelegateHandle ConnectionChangedHandle;

	// The dropdown's item source (agent display names, sidecar registry order). Rebuilt from the agents-list result.
	TArray<TSharedPtr<FString>> AgentNameItems;
	// Parallel agent ids (same order as AgentNameItems) — the id sent in per-agent requests.
	TArray<FString> AgentIds;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> AgentCombo;
	// The container holding the currently-selected agent's per-agent panel (rebuilt on selection change / result).
	TSharedPtr<SVerticalBox> AgentPanelContainer;

	// The selected agent id (persisted via the view-model) and its last-known description from the sidecar.
	FString SelectedAgentId;
	FUnrealMcpAgentDescription SelectedDescription;
	bool bHaveDescription = false;

	/** The async UI phase for the selected agent's status/action round-trip. */
	enum class EPhase : uint8
	{
		Idle,         // a description is shown (or none requested yet)
		Loading,      // a status/list request is in flight
		Pending,      // a configure/remove action is in flight (buttons disabled, "Working…")
		Disconnected, // the sidecar is not connected (SendRequest returned false) — graceful degrade
	};
	EPhase Phase = EPhase::Idle;
	// A short human-readable error from the last failed result (shown in the panel; never a secret).
	FString LastError;

	// In-flight request correlation: the requestId the panel issued and what operation it was (so a stale/late
	// result for a superseded request is ignored). Empty Pending/Loading requestId ⇒ nothing in flight.
	FString PendingRequestId;
	FString PendingRequestType;

	bool IsViewModelValid() const { return ViewModel.IsValid(); }

	// --- Request builders / senders (game thread). ---

	/** Build the per-request settings DTO from the live connection info + project root. */
	TSharedPtr<FJsonObject> BuildSettings() const;
	/** The effective transport string the sidecar should compute status for ("stdio"/"streamableHttp"). */
	FString EffectiveTransportString() const;
	/** Generate a fresh correlation id for a request. */
	static FString NewRequestId();
	/**
	 * Send a request object (sets type/requestId), tracking it as the in-flight request of @p Phase. Returns true
	 * when it reached the sidecar; on false the panel enters the Disconnected graceful state and re-renders.
	 */
	bool SendTracked(const TSharedPtr<FJsonObject>& Request, const FString& RequestType, EPhase InFlightPhase);

	void RequestAgentsList();
	void RequestSelectedStatus();
	void RequestConfigure();
	void RequestRemove();
	void WriteBackEditableValue(const FString& NewValue);
	/** Ask the sidecar to GENERATE the per-tool SKILL.md files for the selected agent (§7 skills section). */
	void RequestGenerateSkills();

	// --- Rendering. ---

	void RebuildAgentPanel();
	TSharedRef<SWidget> MakeStatusRow();
	/** The skills section (generate button + resolved path + last-run status), shown only when the agent supports skills. */
	TSharedRef<SWidget> MakeSkillsSection();
	TSharedRef<SWidget> MakeRichContentFoldout(const FAiAgentRichContentSection& Section);
	TSharedRef<SWidget> MakeItemWidget(const FAiAgentRichContentItem& Item);
	void SetSelectedAgentId(const FString& InAgentId);
	int32 IndexOfAgentId(const FString& InAgentId) const;

	// The last skill-generation outcome for the selected agent (shown in the skills section; reset on selection change).
	FString LastSkillsStatus;   // e.g. "Generated 62 file(s)" / a failure reason (empty = none run yet)
	FString LastSkillsPath;     // the resolved absolute folder the sidecar wrote under (from the result)
};
