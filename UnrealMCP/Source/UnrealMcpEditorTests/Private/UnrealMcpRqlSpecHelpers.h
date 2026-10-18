// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealMcpRqlTools.h"

/**
 * Shared helpers of the RQL specs that run canonical QueryPlans (the C++ runtime has no text parser; each case carries
 * the plan Rql.Parser 0.1.1 produces for the query in the comment next to it).
 */
namespace UnrealMcpRqlSpecHelpers
{
	inline TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
	}

	/** Run a plan; the QueryResult JSON (null when it is not JSON, reported as a test error). */
	inline TSharedPtr<FJsonObject> Exec(FAutomationTestBase& Test, const FString& Plan, bool bMutate = false, bool bDryRun = false)
	{
		bool bOk = false;
		const FString Json = UnrealMcpRqlTools::ExecutePlan(Plan, bMutate, bDryRun, bOk);
		TSharedPtr<FJsonObject> Result = Parse(Json);
		if (Result.IsValid())
			Test.TestEqual(TEXT("ok flag matches the result"), Result->GetBoolField(TEXT("ok")), bOk);
		else
			Test.AddError(FString::Printf(TEXT("not JSON: %s"), *Json));
		return Result;
	}

	inline bool IsOk(const TSharedPtr<FJsonObject>& Result)
	{
		return Result.IsValid() && Result->GetBoolField(TEXT("ok"));
	}

	/** The diagnostics of a result, "code: message; ..." (for Contains checks and failure messages). */
	inline FString Codes(const TSharedPtr<FJsonObject>& Result)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		if (Result.IsValid() && Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics))
		{
			for (const TSharedPtr<FJsonValue>& Diagnostic : *Diagnostics)
				Out.Add(Diagnostic->AsObject()->GetStringField(TEXT("code")) + TEXT(": ") + Diagnostic->AsObject()->GetStringField(TEXT("message")));
		}
		return FString::Join(Out, TEXT("; "));
	}

	inline int32 RowCount(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		return Result.IsValid() && Result->TryGetArrayField(TEXT("rows"), Rows) ? Rows->Num() : 0;
	}

	/** rows[Row][Column] (null when absent). */
	inline TSharedPtr<FJsonValue> Cell(const TSharedPtr<FJsonObject>& Result, int32 Row, int32 Column)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("rows"), Rows) || !Rows->IsValidIndex(Row))
			return nullptr;
		const TArray<TSharedPtr<FJsonValue>>& Cells = (*Rows)[Row]->AsArray();
		return Cells.IsValidIndex(Column) ? Cells[Column] : nullptr;
	}

	/** A cell as text: strings as they are, numbers / bools printed, objects as their $ref, lists / maps as JSON. */
	inline FString CellText(const TSharedPtr<FJsonObject>& Result, int32 Row = 0, int32 Column = 0)
	{
		const TSharedPtr<FJsonValue> Value = Cell(Result, Row, Column);
		if (!Value.IsValid() || Value->IsNull())
			return FString();
		FString Text;
		if (Value->Type == EJson::Object)
		{
			FString Ref;
			if (Value->AsObject()->TryGetStringField(TEXT("$ref"), Ref))
				return Ref;
		}
		if (Value->Type == EJson::Object || Value->Type == EJson::Array)
		{
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Value, FString(), Writer);
			return Text;
		}
		if (Value->Type == EJson::Number)
			return FString::SanitizeFloat(Value->AsNumber(), 0);
		if (Value->Type == EJson::Boolean)
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		return Value->TryGetString(Text) ? Text : FString();
	}

	/** The scalar result of an aggregate (count, sum, ...). */
	inline double Number(const TSharedPtr<FJsonObject>& Result)
	{
		double Value = -1;
		if (Result.IsValid())
			Result->TryGetNumberField(TEXT("value"), Value);
		return Value;
	}

	/** A plan with "@KEY@" placeholders filled in (ids that exist only at run time). */
	inline FString Fill(FString Plan, std::initializer_list<TPair<const TCHAR*, FString>> Values)
	{
		for (const TPair<const TCHAR*, FString>& Value : Values)
			Plan.ReplaceInline(*FString::Printf(TEXT("@%s@"), Value.Key), *Value.Value);
		return Plan;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
