// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpValidation.h"
#include "UnrealMcpToolRegistry.h"   // FUnrealMcpParamSpec + EUnrealMcpParamRequirement

namespace FUnrealMcpValidation
{
	bool IsValidKebabName(const FString& Name)
	{
		if (Name.IsEmpty())
			return false;

		bool bPrevHyphen = false;
		const int32 Len = Name.Len();
		for (int32 i = 0; i < Len; ++i)
		{
			const TCHAR C = Name[i];
			const bool bLower = (C >= TEXT('a') && C <= TEXT('z'));
			const bool bDigit = (C >= TEXT('0') && C <= TEXT('9'));
			const bool bHyphen = (C == TEXT('-'));
			if (!bLower && !bDigit && !bHyphen)
				return false;
			if (bHyphen && (i == 0 || i == Len - 1 || bPrevHyphen))
				return false; // no leading, trailing, or doubled hyphen
			bPrevHyphen = bHyphen;
		}
		return true;
	}

	bool ValidateParamSpecs(
		const TArray<FUnrealMcpParamSpec>& Params, const TCHAR* Noun, const FString& EntryName, FString& OutError)
	{
		static const TCHAR* const KnownTypes[] = { TEXT("string"), TEXT("integer"), TEXT("number"), TEXT("boolean"), TEXT("object"), TEXT("array") };
		TSet<FString> SeenParams;
		for (const FUnrealMcpParamSpec& Param : Params)
		{
			if (Param.Name.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s '%s' has a parameter with an empty name (malformed schema)"), Noun, *EntryName);
				return false;
			}

			bool bKnown = false;
			for (const TCHAR* const Known : KnownTypes)
			{
				if (Param.JsonType == Known) { bKnown = true; break; }
			}
			if (!bKnown)
			{
				OutError = FString::Printf(TEXT("%s '%s' parameter '%s' has unknown JSON type '%s' (malformed schema)"),
					Noun, *EntryName, *Param.Name, *Param.JsonType);
				return false;
			}

			if ((Param.JsonType == TEXT("object") || Param.JsonType == TEXT("array")) && !Param.ObjectSchema.IsValid())
			{
				OutError = FString::Printf(TEXT("%s '%s' %s parameter '%s' has no schema (malformed schema)"),
					Noun, *EntryName, *Param.JsonType, *Param.Name);
				return false;
			}

			bool bAlreadyPresent = false;
			SeenParams.Add(Param.Name, &bAlreadyPresent);
			if (bAlreadyPresent)
			{
				OutError = FString::Printf(TEXT("%s '%s' declares duplicate parameter '%s' (malformed schema)"),
					Noun, *EntryName, *Param.Name);
				return false;
			}
		}
		return true;
	}
}
