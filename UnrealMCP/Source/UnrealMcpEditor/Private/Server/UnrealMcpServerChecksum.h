// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"

/**
 * Pure (no engine-process / no live-network) download-integrity logic for the shared
 * `gamedev-mcp-server` binary: the `SHA256SUMS` manifest URL builder, the coreutils-format parser,
 * the exact-key RID digest lookup, the case-insensitive digest compare, and the single fail-closed
 * `VerifyZipChecksum` verdict. `FUnrealMcpServerManager::DownloadBinaryIfNeeded` calls this BETWEEN the
 * zip download and `FZipArchiveReader` / `FPlatformProcess::CreateProc` — so a downloaded server zip is
 * NEVER extracted or launched unless its SHA256 matches the release's published `SHA256SUMS` manifest
 * (issue #155). A compromised release asset or a trusted-CA MITM would otherwise yield arbitrary code
 * execution on the developer's machine.
 *
 * Keeping this logic here — rather than inline in `FUnrealMcpServerManager` (whose methods touch HTTP,
 * the filesystem, and process spawn) — makes every decision unit-testable in an Automation spec with no
 * live download and no running server: each function below is a deterministic string/enum/map transform.
 * The HTTP fetch + SHA256 compute (via UE's native `FSHA256Signature`) that surround this verdict live
 * in the editor-coupled manager. Mirrors Unity-MCP's `McpServerChecksum` (PR #842) and Godot-MCP's
 * `GodotMcpServerView` checksum seam (PR #193); same parser/verify/test shape, adapted to UE types.
 */
class FUnrealMcpServerChecksum
{
public:
	/**
	 * The name of the integrity manifest asset attached to every GameDev-MCP-Server release: a standard
	 * coreutils `sha256sum` output file listing one `<hex>␠␠<filename>` line per per-RID server zip.
	 * LIVE on the pinned `v8.0.0` release (and every future release).
	 */
	static UNREALMCPEDITOR_API const TCHAR* Sha256SumsAssetName;

	/**
	 * The URL of a release's `SHA256SUMS` manifest — the SIBLING of the per-RID zip
	 * (FUnrealMcpServerManager::ServerDownloadUrl) under the SAME `v<Version>` release tag:
	 * `https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v<Version>/SHA256SUMS`. The
	 * downloaded zip's SHA256 is verified against this manifest BEFORE extraction/execution (fail-closed).
	 * Pure string build — unit-testable with no editor.
	 */
	static UNREALMCPEDITOR_API FString Sha256SumsUrl(const FString& Version);

	/**
	 * The per-RID server zip asset name, e.g. `gamedev-mcp-server-win-x64.zip`. This is the EXACT key
	 * looked up in the parsed `SHA256SUMS` map — it must match the trailing segment of the download URL
	 * so the verified asset name can never drift from the downloaded asset. Pure.
	 */
	static UNREALMCPEDITOR_API FString ServerZipAssetName(const FString& Rid);

	/**
	 * Parse a coreutils `sha256sum` manifest into a `{filename → lowercase-hex-digest}` map. The exact LIVE
	 * format is one line per file: a 64-character lowercase hex digest, then TWO spaces (the coreutils
	 * text-mode separator), then the file name — `844d4ad8…53319␠␠gamedev-mcp-server-linux-x64.zip`.
	 * Tolerances applied (so a hand-edited or CRLF manifest still parses, while a malformed one yields no
	 * usable entry):
	 *   - CRLF and bare-LF line endings; blank lines skipped.
	 *   - Leading/trailing whitespace on each line trimmed.
	 *   - The coreutils binary-mode `'*'` marker before the filename (`<hex> *<name>`) is stripped.
	 *   - A line whose first token is NOT a 64-char hex string, or which has no filename, is SKIPPED (it
	 *     never produces a spurious entry — fail-closed at the lookup layer).
	 * Digests are normalized to lowercase; filenames kept verbatim (case-sensitive, matching the asset
	 * names). On a duplicate filename the LAST entry wins. Never asserts — a null/empty/garbage input
	 * yields an empty map. Pure; unit-testable with no editor.
	 */
	static UNREALMCPEDITOR_API TMap<FString, FString> ParseSha256Sums(const FString& Sha256SumsText);

	/**
	 * Look up the expected SHA256 digest for @p AssetZipName (e.g. `gamedev-mcp-server-win-x64.zip`) in a
	 * parsed ParseSha256Sums map. The lookup is EXACT-key — `linux-x64` never cross-matches `linux-arm64`,
	 * `win-x64` never cross-matches `win-x86` (no substring/prefix match). Writes the lowercase hex digest
	 * to @p OutDigest and returns true when present, or returns false (the MISSING-entry fail-closed case)
	 * when the manifest has no entry for that asset. Pure.
	 */
	static UNREALMCPEDITOR_API bool LookupDigest(const TMap<FString, FString>& ParsedSha256Sums, const FString& AssetZipName, FString& OutDigest);

	/**
	 * Case-insensitive hex-digest equality (both sides trimmed). A null/empty/whitespace digest on either
	 * side is NEVER a match (fail-closed: an unknown digest must not pass). Pure.
	 */
	static UNREALMCPEDITOR_API bool DigestMatches(const FString& ExpectedHexDigest, const FString& ActualHexDigest);

	/** The verdict of verifying a downloaded zip against a release `SHA256SUMS` manifest. */
	enum class EChecksumVerdict : uint8
	{
		/** The manifest parsed, contained this asset's entry, and the digest matched. SAFE to extract/execute. */
		Verified,
		/** The manifest text was missing/empty/unparsable (no usable entries). Fail-closed. */
		ManifestUnparsable,
		/** The manifest parsed but had no line for this asset's zip name. Fail-closed. */
		MissingEntry,
		/** The manifest's entry for this asset did NOT match the downloaded zip's digest. Fail-closed. */
		DigestMismatch
	};

	/**
	 * The single fail-closed integrity decision the manager calls BEFORE FZipArchiveReader /
	 * FPlatformProcess::CreateProc: parse the release's `SHA256SUMS`, find the entry for @p AssetZipName,
	 * and compare it (case-insensitive hex) against the locally-computed SHA256 of the downloaded zip
	 * (@p ActualZipHexDigest). Returns EChecksumVerdict::Verified ONLY when the manifest parsed, contained
	 * the asset, and the digest matched; every other outcome is a distinct fail-closed verdict the caller
	 * MUST treat as "do NOT extract, do NOT launch". Keeping this here (not inline in the manager) makes
	 * the entire decision unit-testable with no editor and no real download. Pure.
	 *
	 * @param Sha256SumsText    The raw downloaded `SHA256SUMS` manifest text.
	 * @param AssetZipName      This RID's zip name, e.g. `gamedev-mcp-server-win-x64.zip`.
	 * @param ActualZipHexDigest The SHA256 of the downloaded zip, as lowercase/any-case hex.
	 */
	static UNREALMCPEDITOR_API EChecksumVerdict VerifyZipChecksum(const FString& Sha256SumsText, const FString& AssetZipName, const FString& ActualZipHexDigest);

	/**
	 * A short, actionable human-readable reason for a non-Verified verdict, for the manager's fail-closed
	 * log line. Pure string transform.
	 */
	static UNREALMCPEDITOR_API FString ChecksumFailureReason(EChecksumVerdict Verdict, const FString& AssetZipName);

	/**
	 * Compute the SHA256 of @p Bytes as a 64-character lowercase hex string. Uses a self-contained FIPS 180-4
	 * SHA-256 (pure integer math on UE types — NO third-party dep) because UE's
	 * `FPlatformMisc::GetSHA256Signature` is a generic stub that `checkf(false)`s on desktop (no platform
	 * override) and UE Core ships no other dependency-free SHA-256. The manager calls this on the downloaded
	 * zip bytes, then feeds the result to VerifyZipChecksum. Pure (no IO); empty input hashes the empty
	 * message (still deterministic). Exposed so an Automation spec can assert the exact hex against the FIPS
	 * reference vectors.
	 */
	static UNREALMCPEDITOR_API FString ComputeSha256Hex(const TArray<uint8>& Bytes);
};
