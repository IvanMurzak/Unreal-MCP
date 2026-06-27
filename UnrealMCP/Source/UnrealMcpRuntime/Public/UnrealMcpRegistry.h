// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"
#include "UnrealMcpEnablementFilter.h"
#include "UnrealMcpDescriptorHash.h"
#include "UnrealMcpLog.h"

/**
 * Outcome of registering one extension provider's entries (docs/ARCHITECTURE.md §5). Shared verbatim by the
 * tool / prompt / resource registries — the `ToolsRegistered` count means "entries registered" for the prompt
 * and resource kinds (the field name predates the generic template). Lives here, in the shared registry
 * header, so all three registries (and the extension manager) see one definition; the per-kind registry
 * headers re-expose it by including this header.
 */
struct UNREALMCPRUNTIME_API FUnrealMcpExtensionRegistrationResult
{
	/** Number of entries that passed validation + dedup and were committed under the extension's id. */
	int32 ToolsRegistered = 0;
	/** One human-readable line per dropped (invalid) or rejected (duplicate) entry; empty when healthy. */
	TArray<FString> Errors;
};

/**
 * Generic plugin-owned registry template for the tool / prompt / resource registries
 * (docs/ARCHITECTURE.md §2.2, §5, §8). The three registries were the SAME class authored three times —
 * keyed `TMap<FString, TEntry>` + a monotonic `Revision` + a retained §7/§8 `FUnrealMcpEnablementFilter`,
 * with identical commit/dedup, extension-scope registration, manifest build, sorted-keys, enablement
 * recompute, and schema-hash logic — differing only in:
 *   - the ENTRY type (`FUnrealMcpRegisteredTool` / `...Prompt` / `...Resource`),
 *   - the KEY accessor (`Name` for tool/prompt vs `Uri` for resource),
 *   - the manifest `type` string + array field name (`tool-manifest`/`tools`, etc.),
 *   - the kind NOUN used in collision-rejection messages + log lines,
 *   - the dispatch surface (`Execute(name, call)` vs `Read(uri)`), which stays on each concrete class
 *     because its argument + result types differ.
 *
 * This template factors out everything BUT those genuinely kind-specific bits. The §169 extractions are
 * reused, not re-extracted: enablement → `FUnrealMcpEnablementFilter`; schema-hash →
 * `UnrealMcpComputeDescriptorHash`; the provider-rebuild template lives in the extension manager.
 *
 * CRTP (`TDerived`): the derived concrete registry passes itself so `RegisterExtension` hands the
 * extension's `RegisterFn` a reference of the CONCRETE registry type (`FUnrealMcpToolRegistry&` etc.),
 * keeping the extension-manager call sites + the public API byte-for-byte stable.
 *
 * A `TTraits` policy supplies the kind-specific statics (key accessor, validation, manifest strings,
 * noun). `TEntry` must expose: `bool bEnabled`, `FString ExtensionId`, `FString SchemaHash`, and
 * `TSharedPtr<FJsonObject> ToDescriptorJson() const`.
 *
 * Threading contract (unchanged from the originals): NOT thread-safe. All mutation happens at startup
 * before the bridge accepts, so the bridge may read `BuildManifestJson()` from its IPC reader thread on
 * handshake without a race. Any DYNAMIC re-registration (the §2.2 hot-reload path) MUST marshal both the
 * mutation and the manifest read through the game-thread dispatcher (§4).
 */
template <typename TDerived, typename TEntry, typename TTraits>
class TUnrealMcpRegistry
{
public:
	// --- Lookup / counts ---------------------------------------------------------------------------

	/** True iff @p Key is registered. */
	bool Has(const FString& Key) const { return Entries.Contains(Key); }
	/** Number of registered entries (enabled or not). */
	int32 Num() const { return Entries.Num(); }
	/** Find a registered entry by key, or null. */
	const TEntry* Find(const FString& Key) const { return Entries.Find(Key); }

	/** Current manifest revision (bumps on every registry mutation). */
	int32 GetRevision() const { return Revision; }

	/** Every registered key, sorted (the §7 UI enumerates the full set, enabled or not). */
	TArray<FString> GetKeysSorted() const
	{
		TArray<FString> Keys;
		Entries.GetKeys(Keys);
		Keys.Sort();
		return Keys;
	}

	/** Count of entries whose bEnabled flag is set (backs the boot-time enable-map log line). */
	int32 NumEnabled() const
	{
		int32 Count = 0;
		for (const TPair<FString, TEntry>& Pair : Entries)
		{
			if (Pair.Value.bEnabled)
				++Count;
		}
		return Count;
	}

	// --- Schema hash -------------------------------------------------------------------------------

	/** Compute the stable schema hash for a descriptor (§2.2 — excludes the mutable enabled flag). */
	static FString ComputeSchemaHash(const TEntry& InEntry)
	{
		// Hash the canonicalized descriptor MINUS the enabled flag (§2.2): the shared helper builds the hash off
		// the descriptor JSON directly (it strips enabled/schemaHash itself), so no struct deep-copy is needed.
		return UnrealMcpComputeDescriptorHash(InEntry.ToDescriptorJson());
	}

	// --- Commit / extension scope ------------------------------------------------------------------

	/**
	 * Commit a fully-built entry (called by the concrete builder's terminal Handle()/Read()).
	 * - Core path (default): replaces any same-key entry — core families are trusted.
	 * - Extension scope (inside RegisterExtension): the entry is stamped with the scope's ExtensionId,
	 *   validated (§5), and deduped against already-registered entries. An invalid entry is DROPPED and a
	 *   duplicate is REJECTED (first-wins), each recording a line in the scope's error list; neither
	 *   affects other entries or extensions.
	 */
	void Commit(TEntry&& InEntry)
	{
		if (bExtensionScope)
		{
			// Extensions are untrusted: stamp the owning id, validate, and dedup (§5). A dropped/rejected
			// entry never touches the committed set, so other entries and extensions are unaffected.
			InEntry.ExtensionId = ScopeExtensionId;

			FString Error;
			if (!TTraits::Validate(InEntry, Error))
			{
				ScopeErrors.Add(Error);
				UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension '%s': %s — entry dropped."), *ScopeExtensionId, *Error);
				return;
			}

			const FString& IncomingKey = TTraits::KeyOf(InEntry);
			if (const TEntry* Existing = Entries.Find(IncomingKey))
			{
				// Tailor the reason to the kind of collision so the recorded error is not misleading: a
				// core clash, a clash with a different extension (first-wins by ExtensionId sort), or two
				// entries sharing a key WITHIN this same provider (an authoring error, no sort involved).
				const TCHAR* Noun = TTraits::KindNoun();      // "tool" | "prompt" | "resource"
				const TCHAR* KeyWord = TTraits::KeyNoun();    // "name" | "uri"
				FString Rejection;
				if (Existing->ExtensionId == TEXT("core"))
				{
					Rejection = FString::Printf(
						TEXT("%s '%s' rejected: %s collides with a built-in core %s (extensions may not shadow core)"),
						Noun, *IncomingKey, KeyWord, Noun);
				}
				else if (Existing->ExtensionId == ScopeExtensionId)
				{
					Rejection = FString::Printf(
						TEXT("%s '%s' rejected: this extension already declared a %s with that %s (duplicate within the extension)"),
						Noun, *IncomingKey, Noun, KeyWord);
				}
				else
				{
					Rejection = FString::Printf(
						TEXT("%s '%s' rejected: %s already registered by extension '%s' (first-wins by ExtensionId sort)"),
						Noun, *IncomingKey, KeyWord, *Existing->ExtensionId);
				}
				ScopeErrors.Add(Rejection);
				UE_LOG(LogUnrealMcp, Warning, TEXT("[Unreal-MCP] extension '%s': %s"), *ScopeExtensionId, *Rejection);
				return;
			}

			++ScopeEntriesRegistered;
		}

		InEntry.SchemaHash = ComputeSchemaHash(InEntry);
		const FString Key = TTraits::KeyOf(InEntry);
		// §7/§8 retention: a (re-)registered entry inherits the retained whitelist/blocklist immediately, so an
		// extension hot-reload (remove + re-register FRESH, bEnabled defaulting true) cannot resurrect an entry
		// the user disabled, and a non-empty §8 whitelist still hides a non-whitelisted entry a rebuild re-adds.
		// The schema hash above excludes the enabled flag (§2.2), so flipping bEnabled here does not perturb it.
		InEntry.bEnabled = ShouldBeEnabled(Key);
		Entries.Add(Key, MoveTemp(InEntry));
		++Revision;
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] registered %s '%s' (revision %d)."), TTraits::KindNoun(), *Key, Revision);
	}

	/**
	 * Register an extension provider's entries (docs/ARCHITECTURE.md §5). Opens an "extension scope": every
	 * Commit during @p RegisterFn is stamped with @p ExtensionId, validated, and deduped, with invalid/
	 * duplicate entries dropped/rejected and recorded in the returned result. Scopes never nest. The
	 * RegisterFn receives the CONCRETE registry (CRTP) so existing extension call sites are unchanged.
	 */
	FUnrealMcpExtensionRegistrationResult RegisterExtension(const FString& ExtensionId, TFunctionRef<void(TDerived&)> RegisterFn)
	{
		checkf(!bExtensionScope, TEXT("TUnrealMcpRegistry::RegisterExtension does not support nested scopes."));

		bExtensionScope = true;
		ScopeExtensionId = ExtensionId;
		ScopeEntriesRegistered = 0;
		ScopeErrors.Reset();

		RegisterFn(static_cast<TDerived&>(*this));

		FUnrealMcpExtensionRegistrationResult Result;
		Result.ToolsRegistered = ScopeEntriesRegistered; // reused struct: count means "entries registered"
		Result.Errors = MoveTemp(ScopeErrors);

		bExtensionScope = false;
		ScopeExtensionId.Empty();
		ScopeEntriesRegistered = 0;
		ScopeErrors.Reset();
		return Result;
	}

	/** Remove every entry stamped with @p ExtensionId (hot-unload / rebuild, §5). Bumps revision if any removed. Returns count removed. */
	int32 RemoveForExtension(const FString& ExtensionId)
	{
		TArray<FString> ToRemove;
		for (const TPair<FString, TEntry>& Pair : Entries)
		{
			if (Pair.Value.ExtensionId == ExtensionId)
				ToRemove.Add(Pair.Key);
		}
		for (const FString& Key : ToRemove)
			Entries.Remove(Key);
		if (ToRemove.Num() > 0)
			++Revision;
		return ToRemove.Num();
	}

	// --- Manifest ----------------------------------------------------------------------------------

	/** Build the full manifest message body (revision + entries[]) for the bridge (§2.2 / §A.1). Disabled entries excluded, sorted by key. */
	TSharedPtr<FJsonObject> BuildManifestJson() const
	{
		TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("type"), TTraits::ManifestType());
		Manifest->SetNumberField(TEXT("revision"), Revision);

		TArray<TSharedPtr<FJsonValue>> Array;
		// Deterministic ordering by key keeps the manifest stable across runs (§5 ordering principle).
		const TArray<FString> Keys = GetKeysSorted();
		for (const FString& Key : Keys)
		{
			// §7 enable-map: a disabled entry is EXCLUDED from the served manifest entirely (not merely flagged),
			// so the sidecar mirrors no Proxy* for it and it never appears in the corresponding list (§2.2).
			const TEntry& Entry = Entries[Key];
			if (!Entry.bEnabled)
				continue;
			Array.Add(MakeShared<FJsonValueObject>(Entry.ToDescriptorJson()));
		}

		Manifest->SetArrayField(TTraits::ManifestArrayKey(), Array);
		return Manifest;
	}

	// --- §7/§8 enablement --------------------------------------------------------------------------

	/**
	 * Set one entry's enabled flag directly — a GRANULAR helper, NOT the production §7 toggle. Does NOT
	 * update the retained whitelist/blocklist, so a later filter recompute can override the flag it set.
	 * Disabled entries are EXCLUDED from BuildManifestJson so the sidecar never exposes them (§2.2). Bumps
	 * the revision on a real change. Unknown key / no-op change: returns false, no revision bump. Game-thread only.
	 */
	bool SetEnabled(const FString& Key, bool bEnabled)
	{
		TEntry* Found = Entries.Find(Key);
		if (Found == nullptr || Found->bEnabled == bEnabled)
			return false;
		Found->bEnabled = bEnabled;
		++Revision;
		UE_LOG(LogUnrealMcp, Verbose, TEXT("[Unreal-MCP] %s '%s' %s (revision %d)."),
			TTraits::KindNoun(), *Key, bEnabled ? TEXT("enabled") : TEXT("disabled"), Revision);
		return true;
	}

	/**
	 * True iff @p Key passes the §8 whitelist gate (the whitelist is empty, or the key is listed) — the
	 * STATIC env-driven dimension of the served predicate, independent of the §7 blocklist.
	 */
	bool PassesWhitelist(const FString& Key) const { return Enablement.PassesWhitelist(Key); }

	/**
	 * Set the §8 env whitelist. An entry passes the whitelist gate iff the whitelist is EMPTY (no filter —
	 * the common case) or the key is listed. RETAINED, so a later re-registration (extension hot-reload) and
	 * every blocklist re-apply re-evaluate it. Recomputes enablement now and bumps the revision once if any
	 * flag changed. Combined effective rule (§8): served iff (whitelist empty OR member) AND NOT blocklisted.
	 * Game-thread only.
	 */
	void SetWhitelistFilter(const TArray<FString>& Whitelist)
	{
		Enablement.SetWhitelist(Whitelist);
		RecomputeEnablement();
	}

	/**
	 * Apply the persisted §7 per-entry enable-map (blocklist): every entry whose key is in @p BlockedKeys is
	 * disabled, the rest follow the §8 whitelist gate. RETAINED, so an extension hot-reload that re-registers
	 * entries FRESH re-applies the blocklist rather than resurrecting a disabled one. Recomputes against BOTH
	 * the retained whitelist and this blocklist, so re-applying the blocklist never clobbers the whitelist.
	 * Idempotent; bumps the revision once if any flag changed. Game-thread only.
	 */
	void ApplyBlocklist(const TArray<FString>& BlockedKeys)
	{
		Enablement.SetBlocklist(BlockedKeys);
		RecomputeEnablement();
	}

protected:
	/**
	 * Look up an entry for dispatch (Execute/Read), enforcing the §7/§8 gate at the dispatch boundary. The
	 * gate is authoritative here, not merely advisory via manifest exclusion: a disabled entry is dropped from
	 * BuildManifestJson, but a sidecar that has not yet applied a re-pushed manifest (toggle → push race) — or
	 * any non-conforming caller — could still dispatch it by key. Returns the entry only when it exists, has a
	 * bound handler, AND is enabled; otherwise null with @p OutError set to the SAME message the originals
	 * produced ("Unknown <noun> '<key>'." / "<Noun> '<key>' is disabled."). The concrete class wraps the
	 * result into its typed result struct so the dispatch surface (argument + result types) stays kind-specific.
	 */
	const TEntry* FindForDispatch(const FString& Key, FString& OutError) const
	{
		const TEntry* Found = Entries.Find(Key);
		if (Found == nullptr || !Found->Handler)
		{
			OutError = FString::Printf(TEXT("Unknown %s '%s'."), TTraits::KindNoun(), *Key);
			return nullptr;
		}
		if (!Found->bEnabled)
		{
			OutError = FString::Printf(TEXT("%s '%s' is disabled."), TTraits::KindNounCapitalized(), *Key);
			return nullptr;
		}
		return Found;
	}

	/**
	 * The combined §8 effective-served predicate: enabled iff (whitelist empty OR member) AND NOT blocklisted.
	 * The retained filter is the single source of truth shared by Commit (per-registration) and
	 * RecomputeEnablement (per-filter-change) — neither can clobber the other's intent.
	 */
	bool ShouldBeEnabled(const FString& Key) const { return Enablement.ShouldBeEnabled(Key); }

	/** Re-evaluate every registered entry's bEnabled against ShouldBeEnabled; bumps the revision once if any changed. */
	void RecomputeEnablement()
	{
		bool bAnyChanged = false;
		for (TPair<FString, TEntry>& Pair : Entries)
		{
			const bool bShouldEnable = ShouldBeEnabled(Pair.Key);
			if (Pair.Value.bEnabled != bShouldEnable)
			{
				Pair.Value.bEnabled = bShouldEnable;
				bAnyChanged = true;
			}
		}
		if (bAnyChanged)
			++Revision;
	}

private:
	TMap<FString, TEntry> Entries;
	int32 Revision = 0;

	// --- Retained §7/§8 enablement filter. Re-applied on every (re-)registration (Commit) so an extension
	//     hot-reload cannot resurrect a disabled entry, and a blocklist re-apply never clobbers the whitelist.
	//     Whitelist = §8 env whitelist (empty = no filter); Blocklist = §7 persisted disable-map. ---
	FUnrealMcpEnablementFilter Enablement;

	// --- Extension-scope state (active only inside RegisterExtension; scopes never nest) ----------
	bool bExtensionScope = false;
	FString ScopeExtensionId;
	int32 ScopeEntriesRegistered = 0;
	TArray<FString> ScopeErrors;
};
