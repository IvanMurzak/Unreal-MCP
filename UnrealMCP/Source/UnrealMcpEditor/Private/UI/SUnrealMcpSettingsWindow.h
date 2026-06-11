// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class FUnrealMcpEditorViewModel;

/**
 * The "Settings" page (docs/ARCHITECTURE.md §7, Unity's UnityMcpProjectSettingsProvider analog), a pure-Slate
 * compound widget — NO UMG. Reads and writes the §8 config (connection mode, Custom-mode server host, auth
 * option + masked token) through the shared FUnrealMcpEditorViewModel, which persists every edit to the §8
 * store and pushes it over IPC. The bound IPC port is surfaced read-only via a provider (the port is
 * deterministic per project / chosen at bind time — not a user-editable field). The SAME widget instance is
 * hosted by both the nomad "Settings" tab AND the "Project → Plugins → AI Game Developer" ISettingsModule
 * section, so UE users find it where they expect (§7).
 */
class SUnrealMcpSettingsWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUnrealMcpSettingsWindow) {}
		/** The shared view-model (owned by the runtime). Required — drives reads + writes + persistence. */
		SLATE_ARGUMENT(TSharedPtr<FUnrealMcpEditorViewModel>, ViewModel)
		/** A read-only provider for the bound IPC port line (e.g. "IPC bridge port: 31234"). Optional. */
		SLATE_ARGUMENT(TFunction<FString()>, PortStatusProvider)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedPtr<FUnrealMcpEditorViewModel> ViewModel;
	TFunction<FString()> PortStatusProvider;

	bool bRevealToken = false;

	TSharedRef<SWidget> BuildConnectionModeRow();
	TSharedRef<SWidget> BuildHostRow();
	TSharedRef<SWidget> BuildAuthRow();
	TSharedRef<SWidget> BuildPortsRow();

	bool IsViewModelValid() const { return ViewModel.IsValid(); }
};
