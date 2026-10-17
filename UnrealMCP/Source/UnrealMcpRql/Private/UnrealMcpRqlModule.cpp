// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTypes.h"

#include "Modules/ModuleManager.h"
#include "Tools/UnrealMcpObjectRef.h"

namespace UnrealMcpRqlModuleLocal
{
	bool GInCall = false;
	bool GCallFailed = false;
	FString GCallFailure;
}

FName IUnrealMcpRqlModule::GetModularFeatureName()
{
	static const FName Name(TEXT("UnrealMcpRqlModule"));
	return Name;
}

void UnrealMcpRql::Fail(const FString& Message)
{
	using namespace UnrealMcpRqlModuleLocal;
	if (!GInCall || GCallFailed)
		return; // outside an RQL call, or the first failure wins
	GCallFailed = true;
	GCallFailure = Message.IsEmpty() ? FString(TEXT("the call failed")) : Message;
}

UClass* UnrealMcpRql::ResolveClass(const FString& Name)
{
	return FUnrealMcpObjectRef::ResolveClass(Name);
}

UObject* UnrealMcpRql::LoadObject(UClass* Class, const FString& Path)
{
	return FUnrealMcpObjectRef::LoadObjectQuiet(Class, Path);
}

void UnrealMcpRqlBeginCall()
{
	using namespace UnrealMcpRqlModuleLocal;
	GInCall = true;
	GCallFailed = false;
	GCallFailure.Reset();
}

bool UnrealMcpRqlEndCall(FString& OutFailure)
{
	using namespace UnrealMcpRqlModuleLocal;
	GInCall = false;
	OutFailure = GCallFailure;
	const bool bFailed = GCallFailed;
	GCallFailed = false;
	GCallFailure.Reset();
	return bFailed;
}

/** The RQL data layer module (docs/rql.md): the editor coordinator registers the tools and runs the module pass. */
IMPLEMENT_MODULE(FDefaultModuleImpl, UnrealMcpRql)
