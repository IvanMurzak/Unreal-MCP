// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Agents/AiAgentConfigurator.h"
#include "Misc/Paths.h"

/**
 * Claude Code configurator (docs/ARCHITECTURE.md §7). Writes the project-level `.mcp.json` at the project root,
 * nesting under "mcpServers" — exactly the cli's claude-code agent def (cli/src/lib/setup-mcp.ts) and Unity's
 * ClaudeCodeConfigurator. Phase-A reference agent #1.
 */
class FClaudeCodeConfigurator : public FAiAgentConfigurator
{
public:
	virtual FString GetAgentName() const override { return TEXT("Claude Code"); }
	virtual FString GetAgentId() const override { return TEXT("claude-code"); }
	virtual FString GetDownloadUrl() const override { return TEXT("https://docs.anthropic.com/en/docs/claude-code/overview"); }
	virtual FString GetIconFileName() const override { return TEXT("claude-64.png"); }

	virtual FString GetConfigFilePath(const FString& InProjectRoot) const override
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(InProjectRoot, TEXT(".mcp.json")));
	}
	virtual FString GetBodyPath() const override { return TEXT("mcpServers"); }
	virtual FString GetTutorialUrl() const override { return TEXT("https://youtu.be/Sknh2p12W8c"); }
	// Per-agent skills folder (Phase C, issue #53) — mirrors Unity's ClaudeCodeConfigurator.SkillsPath.
	virtual FString GetSkillsPath(const FString& /*InProjectRoot*/) const override { return TEXT(".claude/skills"); }

	/**
	 * Per-agent rich content (issue #59) — the 1:1 Slate-data port of Unity's ClaudeCodeConfigurator.OnUICreated:
	 * a "Start" foldout (cd to project root + `claude`), a "Manual Configuration Steps" foldout (the `claude mcp add`
	 * command for the active transport, with the bearer only when auth is required), and a "Troubleshooting" foldout.
	 * The view renders these with the reusable widget templates. Pure data — no Slate, no disk — so it is spec-driven.
	 */
	virtual TArray<FAiAgentRichContentSection> BuildRichContent(bool bStdio) const override
	{
		using FItem = FAiAgentRichContentItem;
		const FAiAgentConnectionInfo& Conn = GetConnection();
		const FString Root = GetProjectRoot();
		const FString ServerName = TEXT("Unreal-MCP");
		// Match Unity: show the real token when present + auth required, else a "<token>" placeholder.
		const FString Token = (Conn.bAuthRequired && !Conn.Token.IsEmpty()) ? Conn.Token : TEXT("<token>");

		TArray<FAiAgentRichContentSection> Sections;

		// "Start" foldout (opens first) — identical to Unity for both transports.
		FAiAgentRichContentSection Start;
		Start.Heading = TEXT("Start");
		Start.bExpandedFirst = true;
		Start.Items.Add(FItem::Description(TEXT("Navigate to project root")));
		Start.Items.Add(FItem::ReadOnlyField(FString::Printf(TEXT("cd \"%s\""), *Root)));
		Start.Items.Add(FItem::Description(TEXT("Launch Claude Code")));
		Start.Items.Add(FItem::ReadOnlyField(TEXT("claude")));
		Sections.Add(MoveTemp(Start));

		// "Manual Configuration Steps" foldout — the add-mcp-server command for the active transport.
		FAiAgentRichContentSection Manual;
		Manual.Heading = TEXT("Manual Configuration Steps");
		Manual.Items.Add(FItem::Description(TEXT("Run the following command in the folder of the Unreal project to configure Claude Code")));
		if (bStdio)
		{
			const FString AuthArgs = Conn.bAuthRequired
				? FString::Printf(TEXT(" authorization=required token=%s"), *Token)
				: FString();
			const FString Command = Conn.ServerPath.Replace(TEXT("\\"), TEXT("/"));
			Manual.Items.Add(FItem::ReadOnlyField(FString::Printf(
				TEXT("claude mcp add %s \"%s\" port=%d client-transport=stdio%s"),
				*ServerName, *Command, Conn.Port, *AuthArgs)));
		}
		else
		{
			const FString AuthHeader = Conn.bAuthRequired
				? FString::Printf(TEXT(" --header \"Authorization: Bearer %s\""), *Token)
				: FString();
			Manual.Items.Add(FItem::ReadOnlyField(FString::Printf(
				TEXT("claude mcp add --transport http %s %s%s"),
				*ServerName, *Conn.HttpUrl, *AuthHeader)));
		}
		Manual.Items.Add(FItem::Description(TEXT("Restart or start Claude Code to apply the configuration")));
		Manual.Items.Add(FItem::ReadOnlyField(TEXT("claude")));
		Sections.Add(MoveTemp(Manual));

		// "Troubleshooting" foldout — same hints as Unity.
		FAiAgentRichContentSection Trouble;
		Trouble.Heading = TEXT("Troubleshooting");
		Trouble.Items.Add(FItem::Description(TEXT("- Ensure Claude Code CLI is installed and accessible from the terminal")));
		Trouble.Items.Add(FItem::Description(TEXT("- Ensure Claude Code CLI is started in the folder where the Unreal project is located")));
		Trouble.Items.Add(FItem::Description(TEXT("- Ensure Claude Code is configured with the same port as the plugin shows right now")));
		Trouble.Items.Add(FItem::Description(TEXT("- Check that the configuration file .mcp.json exists")));
		Trouble.Items.Add(FItem::Description(TEXT("- Restart Claude Code after configuration changes")));
		Sections.Add(MoveTemp(Trouble));

		return Sections;
	}
};
