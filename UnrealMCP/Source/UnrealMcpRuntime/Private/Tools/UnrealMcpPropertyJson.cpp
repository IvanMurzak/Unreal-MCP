// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpPropertyJson.h"

#include "Dom/JsonObject.h"
#include "JsonObjectConverter.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

namespace FUnrealMcpPropertyJson
{
	TSharedPtr<FJsonObject> SerializeObject(const UObject* Object)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Object)
			return Json;
		const int64 SkipFlags = CPF_Transient | CPF_Deprecated;
		FJsonObjectConverter::UStructToJsonObject(Object->GetClass(), Object, Json.ToSharedRef(), /*CheckFlags*/ 0, SkipFlags);
		return Json;
	}
}
