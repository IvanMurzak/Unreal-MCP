// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "Server/UnrealMcpServerChecksum.h"

const TCHAR* FUnrealMcpServerChecksum::Sha256SumsAssetName = TEXT("SHA256SUMS");

namespace
{
	// --- Self-contained SHA-256 (FIPS 180-4) -------------------------------------------------------------
	// UE's FPlatformMisc::GetSHA256Signature is a GENERIC STUB that checkf(false)s on Windows/desktop (it has
	// no platform override — verified: Engine/.../GenericPlatformMisc.cpp asserts "No SHA256 Platform
	// implementation"), and UE Core ships no other dependency-free SHA-256. So we implement the algorithm
	// directly here: pure integer math on UE types, no third-party dep, no engine hashing API, deterministic
	// across platforms, and unit-testable (asserted against the FIPS reference vectors for "abc" and ""). All
	// symbols are uniquely named (unity-build ODR rule — every .cpp in this module is one TU).

	FORCEINLINE uint32 ServerChecksumRotr(uint32 X, uint32 N)
	{
		return (X >> N) | (X << (32 - N));
	}

	void ServerChecksumSha256(const uint8* Data, int64 Length, uint8 OutDigest[32])
	{
		static const uint32 K[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
		};

		uint32 H[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
		};

		// Process the message in 64-byte blocks, padding the final block(s) per the spec. The total length in
		// BITS is appended as a big-endian 64-bit integer.
		const uint64 BitLength = static_cast<uint64>(Length) * 8;
		int64 Offset = 0;
		uint8 Block[64];

		auto ProcessBlock = [&](const uint8* B)
		{
			uint32 W[64];
			for (int32 i = 0; i < 16; ++i)
			{
				W[i] = (static_cast<uint32>(B[i * 4]) << 24)
					| (static_cast<uint32>(B[i * 4 + 1]) << 16)
					| (static_cast<uint32>(B[i * 4 + 2]) << 8)
					| (static_cast<uint32>(B[i * 4 + 3]));
			}
			for (int32 i = 16; i < 64; ++i)
			{
				const uint32 S0 = ServerChecksumRotr(W[i - 15], 7) ^ ServerChecksumRotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
				const uint32 S1 = ServerChecksumRotr(W[i - 2], 17) ^ ServerChecksumRotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
				W[i] = W[i - 16] + S0 + W[i - 7] + S1;
			}

			uint32 A = H[0], BB = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], Hh = H[7];
			for (int32 i = 0; i < 64; ++i)
			{
				const uint32 Sig1 = ServerChecksumRotr(E, 6) ^ ServerChecksumRotr(E, 11) ^ ServerChecksumRotr(E, 25);
				const uint32 Ch = (E & F) ^ (~E & G);
				const uint32 T1 = Hh + Sig1 + Ch + K[i] + W[i];
				const uint32 Sig0 = ServerChecksumRotr(A, 2) ^ ServerChecksumRotr(A, 13) ^ ServerChecksumRotr(A, 22);
				const uint32 Maj = (A & BB) ^ (A & C) ^ (BB & C);
				const uint32 T2 = Sig0 + Maj;
				Hh = G; G = F; F = E; E = D + T1; D = C; C = BB; BB = A; A = T1 + T2;
			}

			H[0] += A; H[1] += BB; H[2] += C; H[3] += D; H[4] += E; H[5] += F; H[6] += G; H[7] += Hh;
		};

		// Full 64-byte blocks straight from the input.
		while (Length - Offset >= 64)
		{
			ProcessBlock(Data + Offset);
			Offset += 64;
		}

		// Final block(s): copy the remainder, append 0x80, zero-pad, append the 64-bit big-endian bit length.
		const int32 Remaining = static_cast<int32>(Length - Offset);
		FMemory::Memzero(Block, sizeof(Block));
		if (Remaining > 0)
			FMemory::Memcpy(Block, Data + Offset, Remaining);
		Block[Remaining] = 0x80;

		if (Remaining >= 56)
		{
			// No room for the length in this block — process it, then a fresh zero block with the length.
			ProcessBlock(Block);
			FMemory::Memzero(Block, sizeof(Block));
		}
		for (int32 i = 0; i < 8; ++i)
			Block[56 + i] = static_cast<uint8>((BitLength >> (56 - i * 8)) & 0xFF);
		ProcessBlock(Block);

		for (int32 i = 0; i < 8; ++i)
		{
			OutDigest[i * 4] = static_cast<uint8>((H[i] >> 24) & 0xFF);
			OutDigest[i * 4 + 1] = static_cast<uint8>((H[i] >> 16) & 0xFF);
			OutDigest[i * 4 + 2] = static_cast<uint8>((H[i] >> 8) & 0xFF);
			OutDigest[i * 4 + 3] = static_cast<uint8>(H[i] & 0xFF);
		}
	}

	// True when @p Value is exactly 64 ASCII hex characters (a SHA256 hex digest). Named uniquely
	// (unity-build ODR rule — every .cpp in this module is concatenated into one TU).
	bool ServerChecksumIsHex64(const FString& Value)
	{
		if (Value.Len() != 64)
			return false;
		for (const TCHAR C : Value)
		{
			const bool bIsHex = (C >= TEXT('0') && C <= TEXT('9'))
				|| (C >= TEXT('a') && C <= TEXT('f'))
				|| (C >= TEXT('A') && C <= TEXT('F'));
			if (!bIsHex)
				return false;
		}
		return true;
	}
}

FString FUnrealMcpServerChecksum::Sha256SumsUrl(const FString& Version)
{
	return FString::Printf(
		TEXT("https://github.com/IvanMurzak/GameDev-MCP-Server/releases/download/v%s/%s"),
		*Version, Sha256SumsAssetName);
}

FString FUnrealMcpServerChecksum::ServerZipAssetName(const FString& Rid)
{
	return FString::Printf(TEXT("gamedev-mcp-server-%s.zip"), *Rid);
}

TMap<FString, FString> FUnrealMcpServerChecksum::ParseSha256Sums(const FString& Sha256SumsText)
{
	TMap<FString, FString> Map;
	if (Sha256SumsText.IsEmpty())
		return Map;

	// Normalize CRLF → LF, split into lines.
	FString Normalized = Sha256SumsText;
	Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	TArray<FString> Lines;
	Normalized.ParseIntoArray(Lines, TEXT("\n"), /*CullEmpty*/ false);

	for (const FString& RawLine : Lines)
	{
		FString Line = RawLine.TrimStartAndEnd();
		if (Line.Len() == 0)
			continue;

		// Split into the digest token and the remainder (the filename). The coreutils separator is two
		// spaces, but we split on the FIRST run of whitespace so a single-space or tab variant still parses
		// — the digest token is fixed-width 64 hex, the filename is everything after.
		int32 SepIndex = INDEX_NONE;
		for (int32 i = 0; i < Line.Len(); ++i)
		{
			if (Line[i] == TEXT(' ') || Line[i] == TEXT('\t'))
			{
				SepIndex = i;
				break;
			}
		}
		if (SepIndex <= 0 || SepIndex >= Line.Len() - 1)
			continue;

		const FString DigestToken = Line.Left(SepIndex);
		if (!ServerChecksumIsHex64(DigestToken))
			continue;

		FString FileName = Line.RightChop(SepIndex + 1);
		// Trim the leading whitespace run before the filename (coreutils text-mode uses two spaces).
		FileName.TrimStartInline();
		// coreutils binary-mode marker: `<hex> *<name>`. Strip a single leading '*'.
		if (FileName.StartsWith(TEXT("*"), ESearchCase::CaseSensitive))
			FileName.RightChopInline(1);
		FileName.TrimStartAndEndInline();
		if (FileName.Len() == 0)
			continue;

		// Last entry wins on a duplicate filename.
		Map.Add(FileName, DigestToken.ToLower());
	}

	return Map;
}

bool FUnrealMcpServerChecksum::LookupDigest(const TMap<FString, FString>& ParsedSha256Sums, const FString& AssetZipName, FString& OutDigest)
{
	if (AssetZipName.IsEmpty())
		return false;
	// EXACT-key lookup — never substring/prefix. linux-x64 must NOT cross-match linux-arm64, etc.
	if (const FString* Found = ParsedSha256Sums.Find(AssetZipName))
	{
		OutDigest = *Found;
		return true;
	}
	return false;
}

bool FUnrealMcpServerChecksum::DigestMatches(const FString& ExpectedHexDigest, const FString& ActualHexDigest)
{
	const FString Expected = ExpectedHexDigest.TrimStartAndEnd();
	const FString Actual = ActualHexDigest.TrimStartAndEnd();
	// A null/empty/whitespace digest on either side is NEVER a match (fail-closed).
	if (Expected.IsEmpty() || Actual.IsEmpty())
		return false;
	return Expected.Equals(Actual, ESearchCase::IgnoreCase);
}

FUnrealMcpServerChecksum::EChecksumVerdict FUnrealMcpServerChecksum::VerifyZipChecksum(
	const FString& Sha256SumsText, const FString& AssetZipName, const FString& ActualZipHexDigest)
{
	const TMap<FString, FString> Parsed = ParseSha256Sums(Sha256SumsText);
	if (Parsed.Num() == 0)
		return EChecksumVerdict::ManifestUnparsable;

	FString Expected;
	if (!LookupDigest(Parsed, AssetZipName, Expected))
		return EChecksumVerdict::MissingEntry;

	return DigestMatches(Expected, ActualZipHexDigest)
		? EChecksumVerdict::Verified
		: EChecksumVerdict::DigestMismatch;
}

FString FUnrealMcpServerChecksum::ChecksumFailureReason(EChecksumVerdict Verdict, const FString& AssetZipName)
{
	switch (Verdict)
	{
	case EChecksumVerdict::ManifestUnparsable:
		return FString::Printf(TEXT("the downloaded %s manifest was empty or unparsable"), Sha256SumsAssetName);
	case EChecksumVerdict::MissingEntry:
		return FString::Printf(TEXT("the %s manifest has no entry for '%s'"), Sha256SumsAssetName, *AssetZipName);
	case EChecksumVerdict::DigestMismatch:
		return FString::Printf(TEXT("the downloaded '%s' SHA256 did not match the %s manifest entry"), *AssetZipName, Sha256SumsAssetName);
	default:
		return TEXT("the checksum was verified");
	}
}

FString FUnrealMcpServerChecksum::ComputeSha256Hex(const TArray<uint8>& Bytes)
{
	uint8 Digest[32];
	ServerChecksumSha256(Bytes.GetData(), Bytes.Num(), Digest);

	// Emit 64 lowercase hex chars (the SHA256SUMS / coreutils format; compare is case-insensitive anyway).
	FString Hex;
	Hex.Reserve(64);
	for (int32 i = 0; i < 32; ++i)
		Hex += FString::Printf(TEXT("%02x"), Digest[i]);
	return Hex;
}
