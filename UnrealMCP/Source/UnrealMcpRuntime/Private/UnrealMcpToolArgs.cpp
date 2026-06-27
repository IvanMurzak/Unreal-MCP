// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpToolArgs.h"
#include "UnrealMcpToolRegistry.h"   // FUnrealMcpToolCall (Arguments)
#include "Dom/JsonValue.h"

namespace FUnrealMcpToolArgs
{
	TArray<FString> GetStringArray(const FUnrealMcpToolCall& Call, const FString& Key, FString& OutError)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Call.Arguments->TryGetArrayField(Key, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					Out.Add(S);
				}
				else
				{
					OutError = FString::Printf(TEXT("'%s' must be an array of strings; a non-string entry was provided."), *Key);
					Out.Reset();
					return Out;
				}
			}
		}
		else if (Call.Arguments->HasField(Key))
		{
			// Present but not an array (a bare string/object/number): error rather than returning an empty list,
			// which the caller would read as "select nothing" / "full object" and act on the OPPOSITE of intent.
			OutError = FString::Printf(TEXT("'%s' must be an array of strings."), *Key);
		}
		return Out;
	}
}
