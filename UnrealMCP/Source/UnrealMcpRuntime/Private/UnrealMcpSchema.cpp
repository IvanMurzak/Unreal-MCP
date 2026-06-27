// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpSchema.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace FUnrealMcpSchema
{
	TSharedPtr<FJsonObject> TypedSchema(const FString& JsonType, const FString& Description)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), JsonType);
		if (!Description.IsEmpty())
			Schema->SetStringField(TEXT("description"), Description);
		return Schema;
	}

	/** Build a `{type:"object", properties:{<axes>:number}}` schema with an optional description. */
	static TSharedPtr<FJsonObject> AxisObjectSchema(const TArray<FString>& Axes, const FString& Description)
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		for (const FString& Axis : Axes)
			Props->SetObjectField(Axis, TypedSchema(TEXT("number"), FString()));

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		if (!Description.IsEmpty())
			Schema->SetStringField(TEXT("description"), Description);
		Schema->SetObjectField(TEXT("properties"), Props);
		return Schema;
	}

	TSharedPtr<FJsonObject> VectorSchema(const FString& Description)
	{
		return AxisObjectSchema({ TEXT("x"), TEXT("y"), TEXT("z") }, Description);
	}

	TSharedPtr<FJsonObject> Rotator(const FString& Description)
	{
		return AxisObjectSchema({ TEXT("pitch"), TEXT("yaw"), TEXT("roll") }, Description);
	}

	TSharedPtr<FJsonObject> StringArray(const FString& Description)
	{
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("array"));
		if (!Description.IsEmpty())
			Schema->SetStringField(TEXT("description"), Description);
		Schema->SetObjectField(TEXT("items"), Items);
		return Schema;
	}

	TSharedPtr<FJsonObject> ObjectBag(const FString& Description)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		if (!Description.IsEmpty())
			Schema->SetStringField(TEXT("description"), Description);
		Schema->SetBoolField(TEXT("additionalProperties"), true);
		return Schema;
	}
}

FString UnrealMcpSerializeCondensed(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
		return FString();

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Out;
}
