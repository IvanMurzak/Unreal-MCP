// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpNdjson.h"

FString FUnrealMcpNdjsonAccumulator::DrainLine()
{
	int32 Len = Buffer.Num();
	// Tolerate CRLF: strip a trailing '\r'.
	if (Len > 0 && Buffer[Len - 1] == '\r')
		--Len;

	FString Line;
	if (Len > 0)
	{
		FUTF8ToTCHAR Convert(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), Len);
		Line = FString(Convert.Length(), Convert.Get());
	}
	Buffer.Reset();
	return Line;
}

bool FUnrealMcpNdjsonAccumulator::Push(const uint8* Data, int32 Count, TArray<FString>& OutLines)
{
	for (int32 i = 0; i < Count; ++i)
	{
		const uint8 B = Data[i];
		if (B == '\n')
		{
			OutLines.Add(DrainLine());
		}
		else
		{
			Buffer.Add(B);
			if (Buffer.Num() > MaxLineBytes)
				return false; // line exceeded the cap before a newline (§1.2) → abort
		}
	}
	return true;
}

TArray<uint8> FUnrealMcpNdjsonAccumulator::Encode(const FString& JsonLine)
{
	FTCHARToUTF8 Convert(*JsonLine);
	TArray<uint8> Out;
	Out.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
	Out.Add('\n');
	return Out;
}
