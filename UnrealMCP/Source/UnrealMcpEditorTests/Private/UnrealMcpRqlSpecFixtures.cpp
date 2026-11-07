// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSpecFixtures.h"

#include "IUnrealMcpRqlModule.h"

void UUnrealMcpRqlSpecApi::Boom(const FString& Message)
{
	UnrealMcpRql::Fail(Message);
}

FUnrealMcpRqlSpecRow& UUnrealMcpRqlSpecApi::RowRef(const FString& Label)
{
	Stored.Label = Label;
	return Stored;
}

void UUnrealMcpRqlSpecApi::Double(int32& Value)
{
	Value *= 2;
}

void UUnrealMcpRqlSpecApi::MakeRow(int32 Damage, FUnrealMcpRqlSpecRow& Row)
{
	Row = FUnrealMcpRqlSpecRow();
	Row.Damage = Damage;
}
