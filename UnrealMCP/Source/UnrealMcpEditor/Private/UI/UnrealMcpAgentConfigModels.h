// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Config/UnrealMcpConfig.h"

class FJsonObject;

/**
 * Plugin-side data models for the §7 AI-agent configurators (docs/ARCHITECTURE.md §7), AFTER the configurator
 * LOGIC moved to the C# sidecar (issue #101). The shared, engine-agnostic
 * <c>com.IvanMurzak.McpPlugin.AgentConfig</c> library (MCP-Plugin-dotnet 6.8.0) now owns the configurator
 * decisions; the sidecar serves them over IPC and emits the <c>AgentConfiguratorDescription</c> DTO. These C++
 * structs are the parse target: the thin Slate panel deserializes the JSON DTO into them and renders them with
 * the existing reusable widget templates (SUnrealMcpAgentWidgets). NO configurator logic lives here — only the
 * connection facts the plugin resolves for the request, plus the rendered-content view models.
 */

/**
 * The resolved connection facts the plugin forwards to the sidecar so it can assemble an agent's STDIO + HTTP
 * MCP-client entries (docs/ARCHITECTURE.md §8). Sourced from the live plugin config — NOT from any Unity/Godot
 * path. This struct used to live alongside the C++ configurators (Agents/AiAgentConfigurator.h); it survives the
 * Agents/ removal because the runtime's ConnectionInfoProvider still resolves it (from the live config + the
 * bridge's bound port) and the panel maps it into the per-request settings DTO.
 *
 *  - ServerPath  — absolute path to the local `gamedev-mcp-server` binary the STDIO form launches (§6). May be
 *                  empty before the cli has downloaded it.
 *  - Port        — the deterministic IPC port for this project (§1.1).
 *  - HttpUrl     — the resolved MCP-client URL the HTTP form points at: <effective host>/mcp.
 *  - bAuthRequired / Token — whether a bearer is sent and its real value (the REAL bearer — masking is the UI's job).
 */
struct FAiAgentConnectionInfo
{
	FString ServerPath;
	int32 Port = 0;
	FString HttpUrl;
	bool bAuthRequired = false;
	FString Token;

	/** Resolve the connection facts from the live plugin config (the same builder the panel/runtime used pre-#101). */
	static UNREALMCPEDITOR_API FAiAgentConnectionInfo FromPluginConfig(const FUnrealMcpConfig& Config, const FString& InServerPath, int32 InPort);
};

/**
 * One element inside a per-agent rich-content section (docs/ARCHITECTURE.md §7) — the data model the thin Slate
 * panel renders with the reusable widget templates (SUnrealMcpAgentWidgets). 1:1 with the shared library's
 * <c>ConfigurationItemKind</c> emitted in the DTO: Description→label, Warning→orange label, Alert→red label,
 * ReadOnlyField→read-only copy field, EditableField→an editable field whose committed value is written back to
 * the sidecar over IPC (the Custom agent's editable config/skills path), Link→a clickable hyperlink that opens
 * <see cref="Url"/> (MCP-Plugin-dotnet 6.9.0 — download / tutorial / docs links carried in the top-level Links
 * collection).
 */
struct FAiAgentRichContentItem
{
	enum class EKind : uint8
	{
		Description,    // a dimmed wrapped description line
		Warning,        // an orange wrapped warning line
		Alert,          // a red wrapped alert line
		ReadOnlyField,  // a read-only, copyable command field
		EditableField,  // an editable field; the committed value is written back over IPC (Custom agent)
		Link            // a clickable hyperlink that opens Url (6.9.0: download / tutorial / docs)
	};

	EKind Kind = EKind::Description;
	FString Text;
	// The open-URL target for a Link-kind item (6.9.0); empty for every other kind.
	FString Url;

	static FAiAgentRichContentItem Description(const FString& InText)   { return { EKind::Description, InText }; }
	static FAiAgentRichContentItem Warning(const FString& InText)       { return { EKind::Warning, InText }; }
	static FAiAgentRichContentItem Alert(const FString& InText)         { return { EKind::Alert, InText }; }
	static FAiAgentRichContentItem ReadOnlyField(const FString& InText) { return { EKind::ReadOnlyField, InText }; }
	static FAiAgentRichContentItem EditableField(const FString& InText) { return { EKind::EditableField, InText }; }
	static FAiAgentRichContentItem Link(const FString& InText, const FString& InUrl) { return { EKind::Link, InText, InUrl }; }

	/** Parse a DTO kind string ("Description"/"Warning"/"Alert"/"ReadOnlyField"/"EditableField"/"Link") into the enum. */
	static UNREALMCPEDITOR_API EKind ParseKind(const FString& Raw);

	/** Parse one DTO item object ("kind"/"text"/"url") into this struct. */
	static UNREALMCPEDITOR_API FAiAgentRichContentItem FromJson(const TSharedPtr<FJsonObject>& Json);
};

/**
 * A collapsible per-agent rich-content section (the shared library's <c>ConfigurationSection</c>). bExpandedFirst
 * marks the section that opens by default. The Slate panel wraps these in an SExpandableArea; the items render in
 * order with the matching widget template.
 */
struct FAiAgentRichContentSection
{
	FString Heading;
	bool bExpandedFirst = false;
	TArray<FAiAgentRichContentItem> Items;
};

/**
 * The tri-state configuration status (MCP-Plugin-dotnet 6.9.0 <c>ConfiguratorStatus</c>): NotConfigured (nothing
 * written), Configured (a valid up-to-date entry exists), ReconfigureNeeded (an entry exists but its connection
 * settings are stale — the sidecar also prepends a "Reconfiguration Required" Alert section). The panel uses it to
 * label the action button (Reconfigure vs Configure).
 */
enum class EAiAgentConfiguratorStatus : uint8
{
	NotConfigured,
	Configured,
	ReconfigureNeeded
};

/**
 * The plugin-side mirror of the sidecar's <c>AgentConfiguratorDescription</c> DTO (§7) — identity + status + the
 * ordered UI sections, parsed from the IPC <c>agent-config-result</c>. The icon is a NAME only (the sidecar never
 * sends bytes); the panel resolves the name to a Slate brush. The thin panel holds one of these per visible agent
 * and rebuilds its Slate tree from it; it carries NO configurator behaviour (Configure/Remove are IPC calls).
 */
struct FUnrealMcpAgentDescription
{
	FString AgentId;
	FString AgentName;
	FString IconName;       // file name only (e.g. "claude-64.png"); empty when no icon
	bool bIsConfigured = false;
	// 6.9.0 tri-state: drives the action-button label (Reconfigure when ReconfigureNeeded). Defaults to NotConfigured
	// so an older/absent field degrades to the Configure label rather than mislabelling.
	EAiAgentConfiguratorStatus Status = EAiAgentConfiguratorStatus::NotConfigured;
	bool bIsInstalled = false;
	bool bSupportsSkills = false;
	FString DownloadUrl;
	FString TutorialUrl;
	// 6.9.0 top-level Link items (download / tutorial / docs) — each a Link-kind item with Text + Url the panel
	// renders as a clickable hyperlink. Supersedes the single Download/Tutorial pair (kept for back-compat).
	TArray<FAiAgentRichContentItem> Links;
	TArray<FAiAgentRichContentSection> Sections;

	/** Parse the DTO status string ("NotConfigured"/"Configured"/"ReconfigureNeeded") into the enum. */
	static UNREALMCPEDITOR_API EAiAgentConfiguratorStatus ParseStatus(const FString& Raw);

	/** Parse one <c>AgentConfiguratorDescriptionDto</c> JSON object (from the IPC result) into this struct. */
	static UNREALMCPEDITOR_API FUnrealMcpAgentDescription FromJson(const TSharedPtr<FJsonObject>& Json);
};
