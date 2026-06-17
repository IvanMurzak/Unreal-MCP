// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UI/UnrealMcpAgentConfigModels.h"
#include "Config/UnrealMcpConfig.h"
#include "Bridge/UnrealMcpBridgeServer.h"

/**
 * Specs for the §7 AI-agent-configurator PLUGIN-SIDE models (docs/ARCHITECTURE.md §7), after the configurator
 * LOGIC moved to the C# sidecar (issue #101). The thin Slate panel is windowed (not headless-verifiable), so
 * the spec coverage is the pure data layer it relies on: parsing the sidecar's <c>AgentConfiguratorDescription</c>
 * DTO into FUnrealMcpAgentDescription / FAiAgentRichContentSection (including the new EditableField kind), the
 * kind-string mapping, and the kept FAiAgentConnectionInfo::FromPluginConfig connection-fact resolution. No live
 * bridge, sidecar, editor world, or files — the JSON is hand-built exactly as the sidecar frames it.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpAgentConfigModelsSpec, "UnrealMcp.AgentConfigModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Family-unique helper (unity-build ODR rule, CLAUDE.md): build a DTO JSON object the way the sidecar does.
	static TSharedPtr<FJsonObject> AgentConfigModelsParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

END_DEFINE_SPEC(FUnrealMcpAgentConfigModelsSpec)

void FUnrealMcpAgentConfigModelsSpec::Define()
{
	Describe("ParseKind", [this]()
	{
		It("maps every shared-library ConfigurationItemKind name", [this]()
		{
			TestEqual("Description", FAiAgentRichContentItem::ParseKind(TEXT("Description")), FAiAgentRichContentItem::EKind::Description);
			TestEqual("Warning", FAiAgentRichContentItem::ParseKind(TEXT("Warning")), FAiAgentRichContentItem::EKind::Warning);
			TestEqual("Alert", FAiAgentRichContentItem::ParseKind(TEXT("Alert")), FAiAgentRichContentItem::EKind::Alert);
			TestEqual("ReadOnlyField", FAiAgentRichContentItem::ParseKind(TEXT("ReadOnlyField")), FAiAgentRichContentItem::EKind::ReadOnlyField);
			TestEqual("EditableField", FAiAgentRichContentItem::ParseKind(TEXT("EditableField")), FAiAgentRichContentItem::EKind::EditableField);
		});

		It("degrades an unknown kind to Description", [this]()
		{
			TestEqual("unknown", FAiAgentRichContentItem::ParseKind(TEXT("Bogus")), FAiAgentRichContentItem::EKind::Description);
		});
	});

	Describe("FromJson", [this]()
	{
		It("parses identity, status booleans, urls, and ordered sections/items", [this]()
		{
			const FString Json = TEXT(R"JSON(
			{
				"agentName": "Claude Code",
				"agentId": "claude-code",
				"iconName": "claude-64.png",
				"isConfigured": true,
				"isInstalled": false,
				"supportsSkills": true,
				"downloadUrl": "https://example.com/dl",
				"tutorialUrl": "https://example.com/tut",
				"sections":
				[
					{
						"heading": "Start",
						"expandedFirst": true,
						"items":
						[
							{ "kind": "Description", "text": "do this" },
							{ "kind": "ReadOnlyField", "text": "claude mcp add ..." }
						]
					},
					{
						"heading": "Custom",
						"expandedFirst": false,
						"items":
						[
							{ "kind": "EditableField", "text": ".claude/skills" }
						]
					}
				]
			})JSON");

			const FUnrealMcpAgentDescription Desc = FUnrealMcpAgentDescription::FromJson(AgentConfigModelsParseObject(Json));

			TestEqual("agentId", Desc.AgentId, FString(TEXT("claude-code")));
			TestEqual("agentName", Desc.AgentName, FString(TEXT("Claude Code")));
			TestEqual("iconName", Desc.IconName, FString(TEXT("claude-64.png")));
			TestTrue("isConfigured", Desc.bIsConfigured);
			TestFalse("isInstalled", Desc.bIsInstalled);
			TestTrue("supportsSkills", Desc.bSupportsSkills);
			TestEqual("downloadUrl", Desc.DownloadUrl, FString(TEXT("https://example.com/dl")));
			TestEqual("tutorialUrl", Desc.TutorialUrl, FString(TEXT("https://example.com/tut")));

			TestEqual("section count", Desc.Sections.Num(), 2);
			TestEqual("first heading", Desc.Sections[0].Heading, FString(TEXT("Start")));
			TestTrue("first expandedFirst", Desc.Sections[0].bExpandedFirst);
			TestEqual("first items", Desc.Sections[0].Items.Num(), 2);
			TestEqual("item0 kind", Desc.Sections[0].Items[0].Kind, FAiAgentRichContentItem::EKind::Description);
			TestEqual("item1 kind", Desc.Sections[0].Items[1].Kind, FAiAgentRichContentItem::EKind::ReadOnlyField);
			TestEqual("item1 text", Desc.Sections[0].Items[1].Text, FString(TEXT("claude mcp add ...")));

			// The EditableField kind (the Custom agent's editable config/skills path) round-trips into the model.
			TestEqual("second items", Desc.Sections[1].Items.Num(), 1);
			TestEqual("editable kind", Desc.Sections[1].Items[0].Kind, FAiAgentRichContentItem::EKind::EditableField);
			TestEqual("editable text", Desc.Sections[1].Items[0].Text, FString(TEXT(".claude/skills")));
		});

		It("returns an empty description for a null/invalid object", [this]()
		{
			const FUnrealMcpAgentDescription Desc = FUnrealMcpAgentDescription::FromJson(nullptr);
			TestTrue("empty agentId", Desc.AgentId.IsEmpty());
			TestEqual("no sections", Desc.Sections.Num(), 0);
		});
	});

	Describe("SendAgentConfigMessage verb allow-list", [this]()
	{
		// Regression for issue #101: the §7 send-verb allow-list (FUnrealMcpBridgeServer::IsValidAgentConfigVerb)
		// must accept EVERY plugin→sidecar request verb the thin Slate panel issues — including
		// `agent-generate-skills`, whose omission silently refused the Generate button's send and flipped the panel
		// to Disconnected. Asserting the pure predicate directly avoids standing up a live socket; both an unknown
		// verb and a disconnected send return false through SendAgentConfigMessage, so only the predicate can prove
		// ACCEPTANCE of a valid verb.
		It("accepts every valid agent-config request verb", [this]()
		{
			TestTrue("agents-list", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(TEXT("agents-list")));
			TestTrue("agent-status", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(TEXT("agent-status")));
			TestTrue("agent-configure", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(TEXT("agent-configure")));
			TestTrue("agent-remove", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(TEXT("agent-remove")));
			TestTrue("agent-skills-path", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(TEXT("agent-skills-path")));
			// The verb that was missing from the allow-list — the Generate button sends exactly this string.
			TestTrue("agent-generate-skills", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(TEXT("agent-generate-skills")));
		});

		It("rejects an unknown verb", [this]()
		{
			TestFalse("bogus verb", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(TEXT("agent-bogus")));
			TestFalse("empty verb", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(FString()));
			// The sidecar→plugin RESULT verb is not a send-side request verb — it must not be acceptable to send.
			TestFalse("agent-config-result", FUnrealMcpBridgeServer::IsValidAgentConfigVerb(TEXT("agent-config-result")));
		});
	});

	Describe("FromPluginConfig", [this]()
	{
		It("resolves the Custom-mode HTTP url + port + no-auth from the live config", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:8080");
			Config.AuthOption = EUnrealMcpAuthOption::None;

			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, /*ServerPath*/ TEXT("C:/srv.exe"), /*Port*/ 31234);
			TestEqual("port", Info.Port, 31234);
			TestEqual("serverPath", Info.ServerPath, FString(TEXT("C:/srv.exe")));
			TestEqual("httpUrl", Info.HttpUrl, FString(TEXT("http://localhost:8080/mcp")));
			TestFalse("authRequired", Info.bAuthRequired);
		});

		It("forces auth in Cloud mode", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Cloud;
			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 0);
			TestTrue("cloud requires auth", Info.bAuthRequired);
			TestTrue("httpUrl ends with /mcp", Info.HttpUrl.EndsWith(TEXT("/mcp")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
