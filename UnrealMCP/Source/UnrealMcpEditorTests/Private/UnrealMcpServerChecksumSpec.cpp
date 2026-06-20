// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Server/UnrealMcpServerChecksum.h"

/**
 * Pure download-integrity specs (issue #155): the fail-closed SHA256SUMS verify-before-execute logic the
 * server manager runs BETWEEN the zip download and FZipArchiveReader/CreateProc. Every decision here is a
 * deterministic string/enum/map transform with no live download and no running server, so the whole
 * verdict is unit-asserted: the manifest URL build, the coreutils two-space parser (CRLF / '*' / garbage
 * tolerances), the EXACT-key RID lookup (no cross-match among the 7 RIDs), the case-insensitive digest
 * compare, the UE-native SHA256 compute, and the four-way VerifyZipChecksum verdict (Verified / mismatch /
 * missing-entry / unparsable). Asserts against the VERBATIM live v8.0.0 SHA256SUMS manifest. Mirrors
 * Unity-MCP's McpServerChecksum spec (PR #842) and Godot-MCP's GodotMcpServerView spec (PR #193).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpServerChecksumSpec, "UnrealMcp.ServerChecksum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// The VERBATIM live v8.0.0 SHA256SUMS manifest (downloaded via
	// `gh release download v8.0.0 --repo IvanMurzak/GameDev-MCP-Server --pattern SHA256SUMS`). Standard
	// coreutils format: <64-lowercase-hex>␠␠<filename>. All 7 RID zips. The test fixture and the production
	// contract are byte-identical so this spec proves ACCEPT-real / REJECT-tampered against ground truth.
	// Spec-unique helper name (unity-build ODR rule — every .cpp in the test module is one TU).
	static FString ServerChecksumLiveSha256Sums()
	{
		return FString(
			TEXT("5f17508e92812fbf9522eb552641d21dc2383fc2f6cf371f5413ad06c9820282  gamedev-mcp-server-linux-arm64.zip\n")
			TEXT("844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319  gamedev-mcp-server-linux-x64.zip\n")
			TEXT("ad0f50042dfa1edde26a9f26968538146ba792cc0188a47f6bfc1ae573bb513e  gamedev-mcp-server-osx-arm64.zip\n")
			TEXT("d25993216e610401c8925716d9ad0f8ecaf3dc93443b12cfd057a75495ef9952  gamedev-mcp-server-osx-x64.zip\n")
			TEXT("702f1d708c25dde6a58d3335c7adb92aa5fe36be618003821ceb040a9b59c51b  gamedev-mcp-server-win-arm64.zip\n")
			TEXT("7383638dbc1cad84cf3b85617405c29c7885a51e34b1ef7b8b8864d0656814cb  gamedev-mcp-server-win-x64.zip\n")
			TEXT("b171e1d8318d0ce4e88d30a5e86ad1cac1acea946ef1a71cd410a27f917c9799  gamedev-mcp-server-win-x86.zip\n"));
	}

	// The published win-x64 digest from the live manifest (the SHA256 of the real release zip).
	static FString ServerChecksumWinX64Digest()
	{
		return TEXT("7383638dbc1cad84cf3b85617405c29c7885a51e34b1ef7b8b8864d0656814cb");
	}

END_DEFINE_SPEC(FUnrealMcpServerChecksumSpec)

void FUnrealMcpServerChecksumSpec::Define()
{
	using EVerdict = FUnrealMcpServerChecksum::EChecksumVerdict;

	Describe("Sha256SumsUrl / ServerZipAssetName", [this]()
	{
		It("builds the v-prefixed SHA256SUMS sibling URL under the same release tag", [this]()
		{
			TestEqual(TEXT("manifest url"),
				FUnrealMcpServerChecksum::Sha256SumsUrl(TEXT("8.0.0")),
				FString(TEXT("https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v8.0.0/SHA256SUMS")));
		});

		It("builds the per-RID zip asset name (the exact lookup key)", [this]()
		{
			TestEqual(TEXT("win-x64 asset name"),
				FUnrealMcpServerChecksum::ServerZipAssetName(TEXT("win-x64")),
				FString(TEXT("gamedev-mcp-server-win-x64.zip")));
			TestEqual(TEXT("linux-arm64 asset name"),
				FUnrealMcpServerChecksum::ServerZipAssetName(TEXT("linux-arm64")),
				FString(TEXT("gamedev-mcp-server-linux-arm64.zip")));
		});
	});

	Describe("ParseSha256Sums", [this]()
	{
		It("parses the two-space coreutils format into all 7 RID entries (live manifest)", [this]()
		{
			const TMap<FString, FString> Parsed = FUnrealMcpServerChecksum::ParseSha256Sums(ServerChecksumLiveSha256Sums());
			TestEqual(TEXT("7 entries parsed"), Parsed.Num(), 7);

			const FString* WinX64 = Parsed.Find(TEXT("gamedev-mcp-server-win-x64.zip"));
			TestTrue(TEXT("win-x64 present"), WinX64 != nullptr);
			if (WinX64)
				TestEqual(TEXT("win-x64 digest verbatim"), *WinX64, ServerChecksumWinX64Digest());

			// All seven asset names resolve.
			TestTrue(TEXT("linux-arm64"), Parsed.Contains(TEXT("gamedev-mcp-server-linux-arm64.zip")));
			TestTrue(TEXT("linux-x64"), Parsed.Contains(TEXT("gamedev-mcp-server-linux-x64.zip")));
			TestTrue(TEXT("osx-arm64"), Parsed.Contains(TEXT("gamedev-mcp-server-osx-arm64.zip")));
			TestTrue(TEXT("osx-x64"), Parsed.Contains(TEXT("gamedev-mcp-server-osx-x64.zip")));
			TestTrue(TEXT("win-arm64"), Parsed.Contains(TEXT("gamedev-mcp-server-win-arm64.zip")));
			TestTrue(TEXT("win-x86"), Parsed.Contains(TEXT("gamedev-mcp-server-win-x86.zip")));
		});

		It("tolerates CRLF endings, the '*' binary marker, and lowercases uppercase digests", [this]()
		{
			const FString Manifest = FString(
				TEXT("7383638DBC1CAD84CF3B85617405C29C7885A51E34B1EF7B8B8864D0656814CB *gamedev-mcp-server-win-x64.zip\r\n")
				TEXT("844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319  gamedev-mcp-server-linux-x64.zip\r\n"));
			const TMap<FString, FString> Parsed = FUnrealMcpServerChecksum::ParseSha256Sums(Manifest);
			TestEqual(TEXT("2 entries"), Parsed.Num(), 2);
			const FString* WinX64 = Parsed.Find(TEXT("gamedev-mcp-server-win-x64.zip"));
			TestTrue(TEXT("win-x64 present after marker+CRLF strip"), WinX64 != nullptr);
			if (WinX64)
				TestEqual(TEXT("uppercase digest normalized to lowercase"), *WinX64, ServerChecksumWinX64Digest());
		});

		It("skips blank lines and garbage (non-64-hex / no-filename) without spurious entries", [this]()
		{
			const FString Manifest = FString(
				TEXT("\n")
				TEXT("not-a-hex-digest  some-file.zip\n")                                   // first token not 64-hex → skipped
				TEXT("deadbeef  too-short.zip\n")                                           // 8 hex → skipped
				TEXT("7383638dbc1cad84cf3b85617405c29c7885a51e34b1ef7b8b8864d0656814cb\n")  // no filename → skipped
				TEXT("   \n")
				TEXT("844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319  gamedev-mcp-server-linux-x64.zip\n"));
			const TMap<FString, FString> Parsed = FUnrealMcpServerChecksum::ParseSha256Sums(Manifest);
			TestEqual(TEXT("only the one valid line survives"), Parsed.Num(), 1);
			TestTrue(TEXT("the valid entry is linux-x64"), Parsed.Contains(TEXT("gamedev-mcp-server-linux-x64.zip")));
		});

		It("yields an empty map for empty input", [this]()
		{
			TestEqual(TEXT("empty -> 0"), FUnrealMcpServerChecksum::ParseSha256Sums(FString()).Num(), 0);
		});
	});

	Describe("LookupDigest (exact-key, no cross-match among the 7 RIDs)", [this]()
	{
		It("returns the correct digest per RID and never cross-matches a sibling RID", [this]()
		{
			const TMap<FString, FString> Parsed = FUnrealMcpServerChecksum::ParseSha256Sums(ServerChecksumLiveSha256Sums());

			FString Digest;
			TestTrue(TEXT("win-x64 found"), FUnrealMcpServerChecksum::LookupDigest(Parsed, TEXT("gamedev-mcp-server-win-x64.zip"), Digest));
			TestEqual(TEXT("win-x64 digest"), Digest, ServerChecksumWinX64Digest());

			// win-x64 vs win-x86 vs win-arm64 are DISTINCT digests — exact-key lookup must not conflate them.
			FString WinX86;
			FUnrealMcpServerChecksum::LookupDigest(Parsed, TEXT("gamedev-mcp-server-win-x86.zip"), WinX86);
			TestNotEqual(TEXT("win-x86 != win-x64 digest"), WinX86, ServerChecksumWinX64Digest());

			// linux-x64 vs linux-arm64 likewise distinct.
			FString LinuxX64, LinuxArm64;
			FUnrealMcpServerChecksum::LookupDigest(Parsed, TEXT("gamedev-mcp-server-linux-x64.zip"), LinuxX64);
			FUnrealMcpServerChecksum::LookupDigest(Parsed, TEXT("gamedev-mcp-server-linux-arm64.zip"), LinuxArm64);
			TestNotEqual(TEXT("linux-x64 != linux-arm64 digest"), LinuxX64, LinuxArm64);
		});

		It("returns false for an asset with no manifest entry (missing-RID fail-closed case)", [this]()
		{
			const TMap<FString, FString> Parsed = FUnrealMcpServerChecksum::ParseSha256Sums(ServerChecksumLiveSha256Sums());
			FString Digest;
			TestFalse(TEXT("unknown asset -> not found"),
				FUnrealMcpServerChecksum::LookupDigest(Parsed, TEXT("gamedev-mcp-server-solaris-sparc.zip"), Digest));
		});
	});

	Describe("DigestMatches (case-insensitive, fail-closed on empty)", [this]()
	{
		It("matches case-insensitively and trims, but never matches an empty digest", [this]()
		{
			TestTrue(TEXT("same hex matches"),
				FUnrealMcpServerChecksum::DigestMatches(ServerChecksumWinX64Digest(), ServerChecksumWinX64Digest()));
			TestTrue(TEXT("upper vs lower matches"),
				FUnrealMcpServerChecksum::DigestMatches(ServerChecksumWinX64Digest().ToUpper(), ServerChecksumWinX64Digest()));
			TestTrue(TEXT("trims surrounding whitespace"),
				FUnrealMcpServerChecksum::DigestMatches(FString::Printf(TEXT("  %s  "), *ServerChecksumWinX64Digest()), ServerChecksumWinX64Digest()));
			TestFalse(TEXT("different hex does not match"),
				FUnrealMcpServerChecksum::DigestMatches(ServerChecksumWinX64Digest(), TEXT("844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319")));
			TestFalse(TEXT("empty expected -> no match"), FUnrealMcpServerChecksum::DigestMatches(TEXT(""), ServerChecksumWinX64Digest()));
			TestFalse(TEXT("empty actual -> no match"), FUnrealMcpServerChecksum::DigestMatches(ServerChecksumWinX64Digest(), TEXT("")));
		});
	});

	Describe("ComputeSha256Hex (self-contained FIPS 180-4 SHA-256)", [this]()
	{
		It("computes the well-known SHA256 of \"abc\" as 64 lowercase hex chars", [this]()
		{
			// FIPS 180-2 reference vector: SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad.
			const TArray<uint8> Abc = { uint8('a'), uint8('b'), uint8('c') };
			const FString Hex = FUnrealMcpServerChecksum::ComputeSha256Hex(Abc);
			TestEqual(TEXT("64 hex chars"), Hex.Len(), 64);
			TestEqual(TEXT("SHA256(\"abc\")"), Hex,
				FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
		});

		It("computes the well-known SHA256 of the empty input", [this]()
		{
			// SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855.
			const FString Hex = FUnrealMcpServerChecksum::ComputeSha256Hex(TArray<uint8>());
			TestEqual(TEXT("SHA256(\"\")"), Hex,
				FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
		});
	});

	Describe("VerifyZipChecksum (the fail-closed gate, against the live manifest)", [this]()
	{
		It("VERIFIED when the manifest has the RID and the digest matches (valid passes)", [this]()
		{
			const EVerdict V = FUnrealMcpServerChecksum::VerifyZipChecksum(
				ServerChecksumLiveSha256Sums(), TEXT("gamedev-mcp-server-win-x64.zip"), ServerChecksumWinX64Digest());
			TestTrue(TEXT("verified"), V == EVerdict::Verified);
		});

		It("DIGEST MISMATCH when the computed digest differs (tampered rejected)", [this]()
		{
			// A tampered zip would hash to something other than the published digest — use the linux-x64
			// digest as a stand-in "wrong hash" for the win-x64 entry.
			const EVerdict V = FUnrealMcpServerChecksum::VerifyZipChecksum(
				ServerChecksumLiveSha256Sums(), TEXT("gamedev-mcp-server-win-x64.zip"),
				TEXT("844d4ad8cd152df44287341235ca2ae67cdb69b496252678eb6491f0bdc53319"));
			TestTrue(TEXT("mismatch"), V == EVerdict::DigestMismatch);
		});

		It("MISSING ENTRY when the manifest has no line for this RID (missing-entry rejected)", [this]()
		{
			const EVerdict V = FUnrealMcpServerChecksum::VerifyZipChecksum(
				ServerChecksumLiveSha256Sums(), TEXT("gamedev-mcp-server-solaris-sparc.zip"), ServerChecksumWinX64Digest());
			TestTrue(TEXT("missing entry"), V == EVerdict::MissingEntry);
		});

		It("MANIFEST UNPARSABLE for empty or all-garbage manifest text (malformed rejected)", [this]()
		{
			TestTrue(TEXT("empty -> unparsable"),
				FUnrealMcpServerChecksum::VerifyZipChecksum(FString(), TEXT("gamedev-mcp-server-win-x64.zip"), ServerChecksumWinX64Digest())
					== EVerdict::ManifestUnparsable);
			TestTrue(TEXT("garbage -> unparsable"),
				FUnrealMcpServerChecksum::VerifyZipChecksum(TEXT("not a manifest at all\njust noise"),
					TEXT("gamedev-mcp-server-win-x64.zip"), ServerChecksumWinX64Digest()) == EVerdict::ManifestUnparsable);
		});

		It("rejects a CORRECT digest looked up under the WRONG RID (no cross-RID acceptance)", [this]()
		{
			// The win-x64 digest is genuine, but asked about under the win-x86 asset name it must NOT verify
			// (win-x86 has its own distinct digest in the manifest).
			const EVerdict V = FUnrealMcpServerChecksum::VerifyZipChecksum(
				ServerChecksumLiveSha256Sums(), TEXT("gamedev-mcp-server-win-x86.zip"), ServerChecksumWinX64Digest());
			TestTrue(TEXT("win-x64 digest under win-x86 key -> mismatch, not verified"), V == EVerdict::DigestMismatch);
		});
	});

	Describe("ChecksumFailureReason", [this]()
	{
		It("renders a distinct actionable reason per fail-closed verdict", [this]()
		{
			TestTrue(TEXT("unparsable mentions manifest"),
				FUnrealMcpServerChecksum::ChecksumFailureReason(EVerdict::ManifestUnparsable, TEXT("x.zip")).Contains(TEXT("unparsable")));
			TestTrue(TEXT("missing mentions the asset"),
				FUnrealMcpServerChecksum::ChecksumFailureReason(EVerdict::MissingEntry, TEXT("gamedev-mcp-server-win-x64.zip")).Contains(TEXT("gamedev-mcp-server-win-x64.zip")));
			TestTrue(TEXT("mismatch mentions SHA256"),
				FUnrealMcpServerChecksum::ChecksumFailureReason(EVerdict::DigestMismatch, TEXT("x.zip")).Contains(TEXT("SHA256")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
