// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

/**
 * The shared §7/§8 enable-state predicate for the tool / prompt / resource registries
 * (docs/ARCHITECTURE.md §2.2, §8). Each registry decides whether an entry is "served" from two retained sets:
 *
 *   - Whitelist (§8, the env `UNREAL_MCP_TOOLS` / `Config.EnabledTools` dimension): when EMPTY there is no filter
 *     (the common case — every entry passes the whitelist gate); when non-empty only listed keys pass.
 *   - Blocklist (§7, the persisted per-entry map the MCP window writes): for prompts / resources a listed key is
 *     disabled; for TOOLS it is the per-tool Visible toggle (`hiddenTools`) — a listed tool is only hidden from
 *     tools/list and still runs when called by name (the tool registry's traits set `bBlocklistHides`).
 *
 * The combined effective-served rule is `(whitelist empty OR member) AND NOT blocklisted` (tools: whitelist only). All three registries
 * carried verbatim copies of these two TSet<FString> members plus the `PassesWhitelist` / `ShouldBeEnabled`
 * predicates; this one small value type replaces the triplication. It is RETAINED on the registry so a later
 * re-registration (extension hot-reload) and every blocklist re-apply re-evaluate against both sets — a blocklist
 * re-apply can never clobber the whitelist, and vice versa.
 *
 * Pure data + inline predicates — the per-registry `RecomputeEnablement()` (which iterates that registry's own
 * map and bumps its revision) stays in each registry and calls these after mutating the filter.
 */
struct UNREALMCPRUNTIME_API FUnrealMcpEnablementFilter
{
	/** §8 env whitelist; EMPTY means "no filter" (every key passes the whitelist gate). */
	TSet<FString> Whitelist;
	/** §7 persisted blocklist (prompts / resources: disabled; tools: the `hiddenTools` map, hidden from the list only). */
	TSet<FString> Blocklist;

	/** The STATIC §8 whitelist dimension alone: passes iff the whitelist is empty OR contains @p Key. */
	bool PassesWhitelist(const FString& Key) const
	{
		return Whitelist.IsEmpty() || Whitelist.Contains(Key);
	}

	/** True iff @p Key is in the §7 blocklist. */
	bool IsBlocked(const FString& Key) const
	{
		return Blocklist.Contains(Key);
	}

	/** The combined effective-served predicate: served iff (whitelist empty OR member) AND NOT blocklisted. */
	bool ShouldBeEnabled(const FString& Key) const
	{
		return PassesWhitelist(Key) && !Blocklist.Contains(Key);
	}

	/** Replace the §8 whitelist from a list (RETAINED). The caller recomputes enablement afterward. */
	void SetWhitelist(const TArray<FString>& InWhitelist)
	{
		Whitelist = TSet<FString>(InWhitelist);
	}

	/** Replace the §7 blocklist from a list (RETAINED). The caller recomputes enablement afterward. */
	void SetBlocklist(const TArray<FString>& InBlocklist)
	{
		Blocklist = TSet<FString>(InBlocklist);
	}
};
