// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpRqlTypes.h"
#include "UObject/Class.h"

#include <functional>
#include <stdexcept>
#include <vector>

class ULevel;
class UWorld;

/** Helpers shared by the provider (UnrealMcpRqlTypes.cpp), the core module and the struct adapters. Game thread. */
namespace UnrealMcpRqlUtil
{
	/** Fail the current RQL getter / setter / method: the engine reports the message to the agent. */
	[[noreturn]] inline void Throw(const FString& Message)
	{
		throw std::runtime_error(UnrealMcpRqlToStd(Message));
	}

	/** "expected <What>, got <the value>" (a value of the wrong type). */
	[[noreturn]] inline void ThrowMismatch(const TCHAR* What, const rql::Value& Got)
	{
		Throw(FString::Printf(TEXT("expected %s, got %s"), What, *UnrealMcpRqlFromStd(rql::describe_value(Got))));
	}

	inline rql::Value Str(const FString& Text)
	{
		return rql::Value::string(UnrealMcpRqlToStd(Text));
	}

	/** Any scalar as text (null / missing read as ""). */
	inline FString TextArg(const rql::Value& Value)
	{
		if (Value.is_string())
			return UnrealMcpRqlFromStd(Value.as_string());
		if (Value.is_null() || Value.is_missing())
			return FString();
		return UnrealMcpRqlFromStd(rql::to_text(Value));
	}

	/** true / false, a number (non-zero = true) or the text "true" / "false"; anything else throws. */
	inline bool BoolArg(const rql::Value& Value)
	{
		if (Value.is_bool())
			return Value.as_bool();
		if (Value.is_number())
			return Value.as_number() != 0.0;
		if (Value.is_string())
		{
			const FString Text = UnrealMcpRqlFromStd(Value.as_string());
			if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase))
				return true;
			if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase))
				return false;
		}
		ThrowMismatch(TEXT("true or false"), Value);
	}

	/** A computed member (read-only without a setter). */
	inline rql::MemberDescriptor Member(const char* Name, const rql::TypeDescriptor* Type, rql::Getter Get, rql::Setter Set = {},
		const char* Description = "", bool bOwning = false, bool bNullable = false)
	{
		rql::MemberDescriptor Result;
		Result.name = Name;
		Result.type = Type;
		Result.getter = MoveTemp(Get);
		Result.setter = MoveTemp(Set);
		Result.readonly = !Result.setter;
		Result.description = Description;
		Result.owning = bOwning;
		Result.nullable = bNullable;
		return Result;
	}

	/** A `call` method of a host type (@p Result nullptr = void). */
	inline rql::MethodDescriptor Method(const char* Name, std::vector<rql::ParamDescriptor> Params, const rql::TypeDescriptor* Result,
		std::function<rql::Value(void*, const std::vector<rql::Value>&)> Invoke, const char* Description)
	{
		rql::MethodDescriptor Out;
		Out.name = Name;
		Out.params = std::move(Params);
		Out.result = Result;
		Out.invoke = std::move(Invoke);
		Out.description = Description;
		return Out;
	}

	/** The list items of an RQL list value (throws for anything else). */
	inline std::vector<rql::Value> Items(const rql::Value& Value)
	{
		if (!Value.is_list())
			ThrowMismatch(TEXT("a list"), Value);
		std::vector<rql::Value> Out;
		for (size_t Index = 0, Count = Value.list().size(); Index < Count; ++Index)
			Out.push_back(Value.list().at(Index));
		return Out;
	}

	/** RQL list of strings. */
	inline rql::Value StrList(const TArray<FString>& Texts)
	{
		std::vector<rql::Value> Out;
		Out.reserve(static_cast<size_t>(Texts.Num()));
		for (const FString& Text : Texts)
			Out.push_back(Str(Text));
		return rql::Value::list_of(std::move(Out));
	}

	/** The editor world (null outside the editor). */
	UWorld* EditorWorld();

	/** Make @p Level the level new actors spawn into; throws when it is not a level or cannot become current (locked). */
	void MakeLevelCurrent(ULevel* Level);

	/** The value of a math / reflected struct from any RQL form (FUnrealMcpRqlTypes::CoerceStruct); throws on mismatch. */
	template <typename T>
	T StructArg(FUnrealMcpRqlTypes& Types, const rql::Value& Value)
	{
		T Out;
		FMemory::Memzero(&Out, sizeof(T)); // math structs are plain data; a {X: 1} literal writes over zeros
		Types.CoerceStruct(TBaseStructure<T>::Get(), Value, &Out);
		return Out;
	}
}
