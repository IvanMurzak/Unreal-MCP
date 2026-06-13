// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

#include "Agents/AiAgentConfigurator.h"
#include "Agents/AiAgentConfiguratorRegistry.h"
#include "Agents/UnrealMcpSkillFileGenerator.h"
#include "Agents/Impl/ClaudeCodeConfigurator.h"
#include "Agents/Impl/CursorConfigurator.h"
#include "Agents/Impl/ClaudeDesktopConfigurator.h"
#include "Agents/Impl/UnityAiConfigurator.h"
#include "Agents/Impl/CustomConfigurator.h"
#include "Config/UnrealMcpConfig.h"
#include "UI/UnrealMcpEditorViewModel.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpCoreTools.h"

/**
 * AI Agent Configurators — Skills generation specs (docs/ARCHITECTURE.md §7, issue #53 Phase C). Covers the
 * per-agent SkillsPath / SupportsSkills metadata + ResolveAbsoluteSkillsPath, the config + view-model persistence
 * of the per-agent auto-generate toggle and the Custom skills path, and the SKILL.md writer (markdown shape, the
 * secret-free invariant, idempotent regeneration, full-set generation from the core registry).
 *
 * Names are unique under the UnrealMcp. filter prefix; helpers are spec members (the unity-build ODR rule). Helper
 * names are skill-spec-unique to avoid colliding with the agent-configurator spec in the same unity TU.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpAgentSkillsSpec, "UnrealMcp.AgentConfigurators.Skills",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// A unique temp skills-root path per call (cleaned up by the spec when done).
	FString SkillSpecMakeTempRoot() const
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcpSkillSpec"), FGuid::NewGuid().ToString(), TEXT("skills"));
	}

	void SkillSpecDeleteTree(const FString& Root) const
	{
		// Delete the GUID parent so the per-call temp tree is fully removed.
		const FString GuidDir = FPaths::GetPath(Root);
		if (!GuidDir.IsEmpty())
			IFileManager::Get().DeleteDirectory(*GuidDir, /*RequireExists*/ false, /*Tree*/ true);
	}

	FString SkillSpecReadAllText(const FString& Path) const
	{
		FString Out;
		FFileHelper::LoadFileToString(Out, *Path);
		return Out;
	}

	// Build a single hand-made tool with one required + one optional param and a description containing a literal
	// token-shaped value, so the no-secret assertion is meaningful (the generator must never emit a real token).
	FUnrealMcpRegisteredTool SkillSpecMakeTool() const
	{
		FUnrealMcpRegisteredTool Tool;
		Tool.Name = TEXT("demo-tool");
		Tool.Title = TEXT("Demo Tool");
		Tool.Description = TEXT("A demo tool for the skills spec.\nIt spans two lines.");
		Tool.bReadOnlyHint = true;
		Tool.bIdempotentHint = true;

		FUnrealMcpParamSpec P1;
		P1.Name = TEXT("name");
		P1.JsonType = TEXT("string");
		P1.Description = TEXT("The name to use.");
		P1.Requirement = EUnrealMcpParamRequirement::Required;
		Tool.Params.Add(P1);

		FUnrealMcpParamSpec P2;
		P2.Name = TEXT("count");
		P2.JsonType = TEXT("integer");
		P2.Description = TEXT("How many | with a pipe.");
		P2.Requirement = EUnrealMcpParamRequirement::Optional;
		Tool.Params.Add(P2);

		return Tool;
	}

END_DEFINE_SPEC(FUnrealMcpAgentSkillsSpec)

void FUnrealMcpAgentSkillsSpec::Define()
{
	Describe("SkillsPath / SupportsSkills metadata (per-agent gating)", [this]()
	{
		It("skills-supporting agents declare their per-agent skills path matching Unity", [this]()
		{
			const FString Root = TEXT("C:/proj");

			FClaudeCodeConfigurator Claude;
			TestEqual(TEXT("claude-code skills path"), Claude.GetSkillsPath(Root), FString(TEXT(".claude/skills")));
			TestTrue(TEXT("claude-code supports skills"), Claude.SupportsSkills(Root));

			FCursorConfigurator Cursor;
			TestEqual(TEXT("cursor skills path"), Cursor.GetSkillsPath(Root), FString(TEXT(".cursor/skills")));
			TestTrue(TEXT("cursor supports skills"), Cursor.SupportsSkills(Root));
		});

		It("agents Unity leaves null do NOT support skills (no skills section)", [this]()
		{
			const FString Root = TEXT("C:/proj");

			FClaudeDesktopConfigurator Desktop;
			TestTrue(TEXT("claude-desktop skills path empty"), Desktop.GetSkillsPath(Root).IsEmpty());
			TestFalse(TEXT("claude-desktop no skills"), Desktop.SupportsSkills(Root));

			FUnityAiConfigurator UnityAi;
			TestTrue(TEXT("unity-ai skills path empty"), UnityAi.GetSkillsPath(Root).IsEmpty());
			TestFalse(TEXT("unity-ai no skills"), UnityAi.SupportsSkills(Root));
		});

		It("every registry agent's SupportsSkills matches a non-empty skills path", [this]()
		{
			const FString Root = TEXT("C:/proj");
			FAiAgentConfiguratorRegistry Registry;
			int32 SupportingCount = 0;
			for (const TSharedRef<FAiAgentConfigurator>& Agent : Registry.All())
			{
				const bool bSupports = Agent->SupportsSkills(Root);
				const bool bHasPath = !Agent->GetSkillsPath(Root).IsEmpty();
				TestEqual(FString::Printf(TEXT("%s: SupportsSkills == has path"), *Agent->GetAgentId()), bSupports, bHasPath);
				if (bSupports)
					++SupportingCount;
			}
			// 13 built-in skills agents + the Custom agent (which always supports skills) = 14; claude-desktop and
			// unity-ai are the two that stay null (matches Unity's per-agent table).
			TestEqual(TEXT("14 of 16 agents support skills"), SupportingCount, 14);
		});

		It("Custom agent supports skills via its user-overridable path (default, and after override)", [this]()
		{
			const FString Root = TEXT("C:/proj");
			FCustomConfigurator Custom;
			TestTrue(TEXT("custom supports skills by default"), Custom.SupportsSkills(Root));
			TestEqual(TEXT("custom default path"), Custom.GetSkillsPath(Root), FString(TEXT(".claude/skills")));

			Custom.SetCustomSkillsPath(TEXT(".my/skills"));
			TestEqual(TEXT("custom overridden path"), Custom.GetSkillsPath(Root), FString(TEXT(".my/skills")));

			Custom.SetCustomSkillsPath(FString()); // empty resets to default (never disables)
			TestEqual(TEXT("custom empty resets to default"), Custom.GetSkillsPath(Root), FString(TEXT(".claude/skills")));
		});
	});

	Describe("ResolveAbsoluteSkillsPath", [this]()
	{
		It("resolves a project-relative path under the project root with forward slashes", [this]()
		{
			FClaudeCodeConfigurator Claude;
			const FString Resolved = Claude.ResolveAbsoluteSkillsPath(TEXT("C:/proj"));
			TestFalse(TEXT("not backslashed"), Resolved.Contains(TEXT("\\")));
			TestTrue(TEXT("ends with .claude/skills"), Resolved.EndsWith(TEXT(".claude/skills")));
			TestTrue(TEXT("rooted under project"), Resolved.Contains(TEXT("/proj/")));
		});

		It("returns empty for a non-skills agent", [this]()
		{
			FClaudeDesktopConfigurator Desktop;
			TestTrue(TEXT("empty resolved path"), Desktop.ResolveAbsoluteSkillsPath(TEXT("C:/proj")).IsEmpty());
		});
	});

	Describe("Config + view-model persistence (auto-generate toggle + Custom skills path)", [this]()
	{
		It("config round-trips skillAutoGenerateAgents and skillsPath", [this]()
		{
			FUnrealMcpConfig Config;
			TestEqual(TEXT("default skillsPath"), Config.SkillsPath, FString(TEXT(".claude/skills")));
			TestEqual(TEXT("default no auto-generate agents"), Config.SkillAutoGenerateAgents.Num(), 0);

			Config.SkillAutoGenerateAgents.Add(TEXT("claude-code"));
			Config.SkillAutoGenerateAgents.Add(TEXT("cursor"));
			Config.SkillsPath = TEXT(".custom/skills");

			const TSharedPtr<FJsonObject> Json = Config.ToJson();
			TestTrue(TEXT("json has skillAutoGenerateAgents"), Json->HasField(TEXT("skillAutoGenerateAgents")));
			TestTrue(TEXT("json has skillsPath"), Json->HasField(TEXT("skillsPath")));

			FUnrealMcpConfig Loaded;
			Loaded.LoadFromJson(Json);
			TestEqual(TEXT("round-tripped agents count"), Loaded.SkillAutoGenerateAgents.Num(), 2);
			TestTrue(TEXT("claude-code present"), Loaded.SkillAutoGenerateAgents.Contains(TEXT("claude-code")));
			TestEqual(TEXT("round-tripped skillsPath"), Loaded.SkillsPath, FString(TEXT(".custom/skills")));
		});

		It("view-model toggles auto-generate per agent, persisting (not pushing) and round-trips", [this]()
		{
			FUnrealMcpEditorViewModel ViewModel;
			int32 PersistCount = 0;
			int32 PushCount = 0;
			ViewModel.OnPersistConfig = [&PersistCount](const FUnrealMcpConfig&) { ++PersistCount; };
			ViewModel.OnPushConfig = [&PushCount](const FUnrealMcpConfig&) { ++PushCount; };

			TestFalse(TEXT("initially off"), ViewModel.IsAutoGenerateSkills(TEXT("claude-code")));

			ViewModel.SetAutoGenerateSkills(TEXT("claude-code"), true);
			TestTrue(TEXT("now on"), ViewModel.IsAutoGenerateSkills(TEXT("claude-code")));
			TestEqual(TEXT("persisted once"), PersistCount, 1);
			TestEqual(TEXT("never pushed"), PushCount, 0);

			ViewModel.SetAutoGenerateSkills(TEXT("claude-code"), true); // no-op
			TestEqual(TEXT("no-op did not persist"), PersistCount, 1);

			ViewModel.SetAutoGenerateSkills(TEXT("claude-code"), false);
			TestFalse(TEXT("off again"), ViewModel.IsAutoGenerateSkills(TEXT("claude-code")));
			TestEqual(TEXT("persisted again"), PersistCount, 2);
		});

		It("view-model SetSkillsPath persists, resets empty to default, never pushes", [this]()
		{
			FUnrealMcpEditorViewModel ViewModel;
			int32 PersistCount = 0;
			int32 PushCount = 0;
			ViewModel.OnPersistConfig = [&PersistCount](const FUnrealMcpConfig&) { ++PersistCount; };
			ViewModel.OnPushConfig = [&PushCount](const FUnrealMcpConfig&) { ++PushCount; };

			ViewModel.SetSkillsPath(TEXT(".abc/skills"));
			TestEqual(TEXT("set value"), ViewModel.GetSkillsPath(), FString(TEXT(".abc/skills")));
			TestEqual(TEXT("persisted once"), PersistCount, 1);

			ViewModel.SetSkillsPath(FString()); // empty resets to default
			TestEqual(TEXT("reset to default"), ViewModel.GetSkillsPath(), FString(TEXT(".claude/skills")));
			TestEqual(TEXT("never pushed"), PushCount, 0);
		});
	});

	Describe("SKILL.md markdown builder (shape + secret-free)", [this]()
	{
		It("emits YAML front-matter, title, param table, JSON schema, and the unreal-mcp-cli call form", [this]()
		{
			const FUnrealMcpRegisteredTool Tool = SkillSpecMakeTool();
			const FString Md = FUnrealMcpSkillFileGenerator::BuildSkillMarkdown(Tool);

			TestTrue(TEXT("starts with front-matter"), Md.StartsWith(TEXT("---")));
			TestTrue(TEXT("has name field"), Md.Contains(TEXT("name: demo-tool")));
			TestTrue(TEXT("has description field"), Md.Contains(TEXT("description: \"")));
			TestTrue(TEXT("has title heading"), Md.Contains(TEXT("# Demo Tool")));
			TestTrue(TEXT("has Input section"), Md.Contains(TEXT("## Input")));
			TestTrue(TEXT("param table header"), Md.Contains(TEXT("| Name | Type | Required | Description |")));
			TestTrue(TEXT("required param row"), Md.Contains(TEXT("| `name` | string | yes |")));
			TestTrue(TEXT("optional param row"), Md.Contains(TEXT("| `count` | integer | no |")));
			TestTrue(TEXT("escaped pipe in desc"), Md.Contains(TEXT("How many \\| with a pipe.")));
			TestTrue(TEXT("input json schema fence"), Md.Contains(TEXT("### Input JSON Schema")));
			TestTrue(TEXT("has hints line"), Md.Contains(TEXT("**Hints:** read-only, idempotent")));
			TestTrue(TEXT("how-to-call form"), Md.Contains(TEXT("unreal-mcp-cli run-tool demo-tool")));
		});

		It("a no-param tool says so and shows no table", [this]()
		{
			FUnrealMcpRegisteredTool Tool;
			Tool.Name = TEXT("ping");
			Tool.Title = TEXT("Ping");
			Tool.Description = TEXT("A probe.");
			const FString Md = FUnrealMcpSkillFileGenerator::BuildSkillMarkdown(Tool);
			TestTrue(TEXT("no-params note"), Md.Contains(TEXT("This tool takes no parameters.")));
			TestFalse(TEXT("no table"), Md.Contains(TEXT("| Name | Type")));
		});

		It("never writes a real token even when a token-shaped string is present elsewhere", [this]()
		{
			// The generator only ever reads tool metadata (name/title/description/params/schema). It cannot see a
			// connection token. Assert the call-form uses a placeholder, never a 'Bearer <secret>' header.
			const FUnrealMcpRegisteredTool Tool = SkillSpecMakeTool();
			const FString Md = FUnrealMcpSkillFileGenerator::BuildSkillMarkdown(Tool);
			TestFalse(TEXT("no Bearer header"), Md.Contains(TEXT("Bearer ")));
			TestFalse(TEXT("no token= arg"), Md.Contains(TEXT("token=")));
			TestFalse(TEXT("no Authorization"), Md.Contains(TEXT("Authorization")));
		});
	});

	Describe("Generation output (idempotent, full core set)", [this]()
	{
		It("writes one SKILL.md per tool and is idempotent on re-run", [this]()
		{
			const FString Root = SkillSpecMakeTempRoot();

			FUnrealMcpToolRegistry Registry;
			FUnrealMcpSkillFileGenerator::PopulateCoreRegistry(Registry);
			const int32 ToolCount = Registry.Num();
			TestTrue(TEXT("core registry has tools"), ToolCount > 0);

			const FUnrealMcpSkillFileGenerator::FResult First = FUnrealMcpSkillFileGenerator::Generate(Registry, Root);
			TestTrue(TEXT("first run succeeded"), First.bSuccess);
			TestEqual(TEXT("wrote one file per tool"), First.FilesWritten, ToolCount);

			// A known tool's SKILL.md exists and contains its call form.
			const FString PingPath = FPaths::Combine(Root, TEXT("ping"), TEXT("SKILL.md"));
			TestTrue(TEXT("ping SKILL.md exists"), IFileManager::Get().FileExists(*PingPath));
			const FString PingMd = SkillSpecReadAllText(PingPath);
			TestTrue(TEXT("ping doc has call form"), PingMd.Contains(TEXT("unreal-mcp-cli run-tool ping")));

			// Re-run: same file count, clean overwrite (idempotent).
			const FUnrealMcpSkillFileGenerator::FResult Second = FUnrealMcpSkillFileGenerator::Generate(Registry, Root);
			TestTrue(TEXT("second run succeeded"), Second.bSuccess);
			TestEqual(TEXT("idempotent file count"), Second.FilesWritten, ToolCount);
			const FString PingMd2 = SkillSpecReadAllText(PingPath);
			TestEqual(TEXT("identical content on re-run"), PingMd2, PingMd);

			SkillSpecDeleteTree(Root);
		});

		It("prunes stale generator-owned folders for removed tools but keeps survivors and user content", [this]()
		{
			const FString Root = SkillSpecMakeTempRoot();
			IFileManager& FileManager = IFileManager::Get();

			// Set A: two tools (Commit outside an extension scope adds the descriptor directly — no handler needed).
			FUnrealMcpToolRegistry RegA;
			{
				FUnrealMcpRegisteredTool Keep;
				Keep.Name = TEXT("keep-tool");
				Keep.Title = TEXT("Keep");
				Keep.Description = TEXT("Stays across runs.");
				RegA.Commit(MoveTemp(Keep));

				FUnrealMcpRegisteredTool Drop;
				Drop.Name = TEXT("drop-tool");
				Drop.Title = TEXT("Drop");
				Drop.Description = TEXT("Removed in set B.");
				RegA.Commit(MoveTemp(Drop));
			}
			const FUnrealMcpSkillFileGenerator::FResult A = FUnrealMcpSkillFileGenerator::Generate(RegA, Root);
			TestTrue(TEXT("set A succeeded"), A.bSuccess);
			TestEqual(TEXT("set A wrote two"), A.FilesWritten, 2);
			TestEqual(TEXT("set A pruned nothing"), A.FilesPruned, 0);

			// Drop an unrelated user folder (no SKILL.md) — pruning must never touch it.
			const FString UserDir = FPaths::Combine(Root, TEXT("my-notes"));
			FFileHelper::SaveStringToFile(TEXT("hello"), *FPaths::Combine(UserDir, TEXT("README.md")));

			// Set B: only keep-tool remains.
			FUnrealMcpToolRegistry RegB;
			{
				FUnrealMcpRegisteredTool Keep;
				Keep.Name = TEXT("keep-tool");
				Keep.Title = TEXT("Keep");
				Keep.Description = TEXT("Stays across runs.");
				RegB.Commit(MoveTemp(Keep));
			}
			const FUnrealMcpSkillFileGenerator::FResult B = FUnrealMcpSkillFileGenerator::Generate(RegB, Root);
			TestTrue(TEXT("set B succeeded"), B.bSuccess);
			TestEqual(TEXT("set B wrote one"), B.FilesWritten, 1);
			TestEqual(TEXT("set B pruned the removed tool"), B.FilesPruned, 1);

			TestTrue(TEXT("survivor SKILL.md kept"), FileManager.FileExists(*FPaths::Combine(Root, TEXT("keep-tool"), TEXT("SKILL.md"))));
			TestFalse(TEXT("removed tool's SKILL.md gone"), FileManager.FileExists(*FPaths::Combine(Root, TEXT("drop-tool"), TEXT("SKILL.md"))));
			TestTrue(TEXT("unrelated user folder untouched"), FileManager.FileExists(*FPaths::Combine(UserDir, TEXT("README.md"))));

			SkillSpecDeleteTree(Root);
		});

		It("fails cleanly on an empty skills root", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			FUnrealMcpSkillFileGenerator::PopulateCoreRegistry(Registry);
			const FUnrealMcpSkillFileGenerator::FResult Result = FUnrealMcpSkillFileGenerator::Generate(Registry, FString());
			TestFalse(TEXT("empty root fails"), Result.bSuccess);
			TestFalse(TEXT("error reported"), Result.Error.IsEmpty());
		});

		It("sanitizes odd tool names into safe folder names", [this]()
		{
			TestEqual(TEXT("kebab passthrough"), FUnrealMcpSkillFileGenerator::SanitizeSkillFolderName(TEXT("actor-create")), FString(TEXT("actor-create")));
			TestEqual(TEXT("collapses junk"), FUnrealMcpSkillFileGenerator::SanitizeSkillFolderName(TEXT("Foo__Bar!!Baz")), FString(TEXT("foo-bar-baz")));
			TestEqual(TEXT("empty -> tool"), FUnrealMcpSkillFileGenerator::SanitizeSkillFolderName(TEXT("!!!")), FString(TEXT("tool")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
