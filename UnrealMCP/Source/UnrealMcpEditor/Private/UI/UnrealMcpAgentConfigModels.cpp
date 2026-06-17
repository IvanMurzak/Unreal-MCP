// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UI/UnrealMcpAgentConfigModels.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FAiAgentConnectionInfo FAiAgentConnectionInfo::FromPluginConfig(const FUnrealMcpConfig& Config, const FString& InServerPath, int32 InPort)
{
	FAiAgentConnectionInfo Info;
	Info.ServerPath = InServerPath;
	Info.Port = InPort;

	const bool bCloud = Config.ConnectionMode == EUnrealMcpConnectionMode::Cloud;

	// The MCP-client URL is the effective host + "/mcp" (mirrors the cli's `${url}/mcp`). Cloud uses the cloud
	// base URL; Custom uses the user's host. ResolveCloudBaseUrl/ResolveCustomHost already trim a trailing slash.
	const FString BaseUrl = bCloud ? Config.ResolveCloudBaseUrl() : Config.ResolveCustomHost();
	Info.HttpUrl = BaseUrl + TEXT("/mcp");

	// Auth: Cloud always requires it (the cloud enforces it); Custom follows AuthOption. The token is the
	// mode-resolved effective bearer (Cloud→cloudToken, Custom+Required→token, else empty).
	Info.bAuthRequired = bCloud || Config.AuthOption == EUnrealMcpAuthOption::Required;
	Info.Token = Config.ResolveEffectiveToken();

	return Info;
}

FAiAgentRichContentItem::EKind FAiAgentRichContentItem::ParseKind(const FString& Raw)
{
	// Match the shared library's ConfigurationItemKind names (sent verbatim in the DTO). Unknown → Description
	// so a future kind degrades to a plain label rather than rendering nothing.
	if (Raw == TEXT("Warning"))       return EKind::Warning;
	if (Raw == TEXT("Alert"))         return EKind::Alert;
	if (Raw == TEXT("ReadOnlyField")) return EKind::ReadOnlyField;
	if (Raw == TEXT("EditableField")) return EKind::EditableField;
	if (Raw == TEXT("Link"))          return EKind::Link;
	return EKind::Description;
}

FAiAgentRichContentItem FAiAgentRichContentItem::FromJson(const TSharedPtr<FJsonObject>& Json)
{
	FAiAgentRichContentItem Item;
	if (!Json.IsValid())
		return Item;

	FString Kind;
	Json->TryGetStringField(TEXT("kind"), Kind);
	Item.Kind = ParseKind(Kind);
	Json->TryGetStringField(TEXT("text"), Item.Text);
	Json->TryGetStringField(TEXT("url"), Item.Url); // 6.9.0: populated only for Link-kind items
	return Item;
}

EAiAgentConfiguratorStatus FUnrealMcpAgentDescription::ParseStatus(const FString& Raw)
{
	// Match the shared library's ConfiguratorStatus names. Unknown / absent → NotConfigured (the safe default that
	// labels the action button "Configure" rather than falsely "Reconfigure").
	if (Raw == TEXT("Configured"))        return EAiAgentConfiguratorStatus::Configured;
	if (Raw == TEXT("ReconfigureNeeded")) return EAiAgentConfiguratorStatus::ReconfigureNeeded;
	return EAiAgentConfiguratorStatus::NotConfigured;
}

FUnrealMcpAgentDescription FUnrealMcpAgentDescription::FromJson(const TSharedPtr<FJsonObject>& Json)
{
	FUnrealMcpAgentDescription Desc;
	if (!Json.IsValid())
		return Desc;

	Json->TryGetStringField(TEXT("agentId"), Desc.AgentId);
	Json->TryGetStringField(TEXT("agentName"), Desc.AgentName);
	Json->TryGetStringField(TEXT("iconName"), Desc.IconName);
	Json->TryGetBoolField(TEXT("isConfigured"), Desc.bIsConfigured);
	Json->TryGetBoolField(TEXT("isInstalled"), Desc.bIsInstalled);
	Json->TryGetBoolField(TEXT("supportsSkills"), Desc.bSupportsSkills);
	Json->TryGetStringField(TEXT("downloadUrl"), Desc.DownloadUrl);
	Json->TryGetStringField(TEXT("tutorialUrl"), Desc.TutorialUrl);

	// 6.9.0: the tri-state status. Absent in an older sidecar → NotConfigured (back-compat).
	FString StatusRaw;
	Json->TryGetStringField(TEXT("status"), StatusRaw);
	Desc.Status = ParseStatus(StatusRaw);

	// 6.9.0: the top-level Link items (each a Link-kind item with text + url). Absent in an older sidecar → empty,
	// and the legacy downloadUrl/tutorialUrl fall-back renders instead (RebuildAgentPanel handles the choice).
	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	if (Json->TryGetArrayField(TEXT("links"), Links) && Links)
	{
		for (const TSharedPtr<FJsonValue>& LinkValue : *Links)
		{
			const TSharedPtr<FJsonObject>* LinkObj = nullptr;
			if (!LinkValue.IsValid() || !LinkValue->TryGetObject(LinkObj) || !LinkObj)
				continue;
			Desc.Links.Add(FAiAgentRichContentItem::FromJson(*LinkObj));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (Json->TryGetArrayField(TEXT("sections"), Sections) && Sections)
	{
		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj)
				continue;

			FAiAgentRichContentSection Section;
			(*SectionObj)->TryGetStringField(TEXT("heading"), Section.Heading);
			(*SectionObj)->TryGetBoolField(TEXT("expandedFirst"), Section.bExpandedFirst);

			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if ((*SectionObj)->TryGetArrayField(TEXT("items"), Items) && Items)
			{
				for (const TSharedPtr<FJsonValue>& ItemValue : *Items)
				{
					const TSharedPtr<FJsonObject>* ItemObj = nullptr;
					if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObj) || !ItemObj)
						continue;

					Section.Items.Add(FAiAgentRichContentItem::FromJson(*ItemObj));
				}
			}
			Desc.Sections.Add(MoveTemp(Section));
		}
	}

	return Desc;
}
