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
			// 6.9.0: the Link kind (clickable open-URL items).
			TestEqual("Link", FAiAgentRichContentItem::ParseKind(TEXT("Link")), FAiAgentRichContentItem::EKind::Link);
		});

		It("degrades an unknown kind to Description", [this]()
		{
			TestEqual("unknown", FAiAgentRichContentItem::ParseKind(TEXT("Bogus")), FAiAgentRichContentItem::EKind::Description);
		});
	});

	Describe("ParseStatus", [this]()
	{
		It("maps every shared-library ConfiguratorStatus name", [this]()
		{
			TestEqual("NotConfigured", FUnrealMcpAgentDescription::ParseStatus(TEXT("NotConfigured")), EAiAgentConfiguratorStatus::NotConfigured);
			TestEqual("Configured", FUnrealMcpAgentDescription::ParseStatus(TEXT("Configured")), EAiAgentConfiguratorStatus::Configured);
			TestEqual("ReconfigureNeeded", FUnrealMcpAgentDescription::ParseStatus(TEXT("ReconfigureNeeded")), EAiAgentConfiguratorStatus::ReconfigureNeeded);
		});

		It("degrades an unknown/absent status to NotConfigured", [this]()
		{
			TestEqual("unknown", FUnrealMcpAgentDescription::ParseStatus(TEXT("Bogus")), EAiAgentConfiguratorStatus::NotConfigured);
			TestEqual("empty", FUnrealMcpAgentDescription::ParseStatus(FString()), EAiAgentConfiguratorStatus::NotConfigured);
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

		It("parses the 6.9.0 status, top-level links, and per-item link urls", [this]()
		{
			const FString Json = TEXT(R"JSON(
			{
				"agentId": "claude-code",
				"agentName": "Claude Code",
				"status": "ReconfigureNeeded",
				"isConfigured": true,
				"links":
				[
					{ "kind": "Link", "text": "Download", "url": "https://example.com/dl" },
					{ "kind": "Link", "text": "YouTube Tutorial", "url": "https://youtu.be/abc" }
				],
				"sections":
				[
					{
						"heading": "Reconfiguration Required",
						"expandedFirst": true,
						"items":
						[
							{ "kind": "Alert", "text": "Connection settings have changed." }
						]
					}
				]
			})JSON");

			const FUnrealMcpAgentDescription Desc = FUnrealMcpAgentDescription::FromJson(AgentConfigModelsParseObject(Json));

			// Tri-state status drives the Reconfigure button label.
			TestEqual("status", Desc.Status, EAiAgentConfiguratorStatus::ReconfigureNeeded);

			// The top-level Links collection: Link-kind items carrying both text and url.
			TestEqual("link count", Desc.Links.Num(), 2);
			TestEqual("link0 kind", Desc.Links[0].Kind, FAiAgentRichContentItem::EKind::Link);
			TestEqual("link0 text", Desc.Links[0].Text, FString(TEXT("Download")));
			TestEqual("link0 url", Desc.Links[0].Url, FString(TEXT("https://example.com/dl")));
			TestEqual("link1 url", Desc.Links[1].Url, FString(TEXT("https://youtu.be/abc")));

			// The prepended "Reconfiguration Required" Alert section round-trips (already an Alert-kind renderer).
			TestEqual("section count", Desc.Sections.Num(), 1);
			TestEqual("reconfig heading", Desc.Sections[0].Heading, FString(TEXT("Reconfiguration Required")));
			TestEqual("alert kind", Desc.Sections[0].Items[0].Kind, FAiAgentRichContentItem::EKind::Alert);
		});

		It("defaults status to NotConfigured and links to empty when the fields are absent", [this]()
		{
			// An older sidecar (no status/links fields) must degrade safely, not mislabel or crash.
			const FString Json = TEXT(R"JSON({ "agentId": "claude-code", "agentName": "Claude Code" })JSON");
			const FUnrealMcpAgentDescription Desc = FUnrealMcpAgentDescription::FromJson(AgentConfigModelsParseObject(Json));
			TestEqual("default status", Desc.Status, EAiAgentConfiguratorStatus::NotConfigured);
			TestEqual("no links", Desc.Links.Num(), 0);
			// mcp-authorize PR 5: supportsOAuth absent → default true (assume OAuth-capable; no escape hatch surfaced).
			TestTrue("default supportsOAuth", Desc.bSupportsOAuth);
		});

		It("parses the mcp-authorize PR 5 supportsOAuth capability flag", [this]()
		{
			// A SupportsOAuth == false agent is the signal for the editor to offer the "Advanced: use access token"
			// escape hatch. The plugin-side model must carry it verbatim from the DTO.
			const FString JsonFalse = TEXT(R"JSON({ "agentId": "legacy-client", "agentName": "Legacy", "supportsOAuth": false })JSON");
			const FUnrealMcpAgentDescription DescFalse = FUnrealMcpAgentDescription::FromJson(AgentConfigModelsParseObject(JsonFalse));
			TestFalse("supportsOAuth false round-trips", DescFalse.bSupportsOAuth);

			const FString JsonTrue = TEXT(R"JSON({ "agentId": "claude-code", "agentName": "Claude Code", "supportsOAuth": true })JSON");
			const FUnrealMcpAgentDescription DescTrue = FUnrealMcpAgentDescription::FromJson(AgentConfigModelsParseObject(JsonTrue));
			TestTrue("supportsOAuth true round-trips", DescTrue.bSupportsOAuth);
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

		// mcp-authorize PR 5 (design 06, Flow C): bUseAccessToken drives the "Advanced: use access token" escape hatch.
		// It is true ONLY for a Custom-mode Required-auth WITH a non-empty token — the default path (and Cloud, whose
		// auth is server-enforced native OAuth, never a user PAT) stays on the credential-free OAuth path.
		It("does not opt into the access-token escape hatch on the default Custom path", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:8080");
			Config.AuthOption = EUnrealMcpAuthOption::None;
			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 31234);
			TestFalse("no escape hatch on default path", Info.bUseAccessToken);
		});

		It("opts into the access-token escape hatch for Custom + Required + a token", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.CustomHost = TEXT("http://localhost:8080");
			Config.AuthOption = EUnrealMcpAuthOption::Required;
			Config.CustomToken = TEXT("pat-secret-value");
			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 31234);
			TestTrue("escape hatch active", Info.bUseAccessToken);
			TestEqual("token forwarded", Info.Token, FString(TEXT("pat-secret-value")));
		});

		It("does not opt in for Custom + Required but an EMPTY token", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Custom;
			Config.AuthOption = EUnrealMcpAuthOption::Required;
			Config.CustomToken = FString();
			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 31234);
			TestFalse("no token → no escape hatch", Info.bUseAccessToken);
		});

		It("never opts into the escape hatch in Cloud mode (native OAuth, not a user PAT)", [this]()
		{
			FUnrealMcpConfig Config;
			Config.ConnectionMode = EUnrealMcpConnectionMode::Cloud;
			Config.CloudToken = TEXT("cloud-bearer-value");
			const FAiAgentConnectionInfo Info = FAiAgentConnectionInfo::FromPluginConfig(Config, FString(), 0);
			TestFalse("cloud never uses the PAT escape hatch", Info.bUseAccessToken);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
