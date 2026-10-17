// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "IUnrealMcpRqlModule.h"
#include "UnrealMcpRqlTools.h"
#include "UnrealMcpRqlTypes.h"

#include "Containers/Ticker.h"
#include "Features/IModularFeatures.h"
#include "Modules/ModuleManager.h"

namespace UnrealMcpRqlTools
{
	void Republish(); // UnrealMcpRqlTools.cpp
}

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
	return UnrealMcpRqlResolveClass(Name);
}

UObject* UnrealMcpRql::LoadObject(UClass* Class, const FString& Path)
{
	return UnrealMcpRqlLoadObject(Class, Path);
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

/**
 * The RQL data layer module (docs/rql.md). The tools are registered by the editor coordinator
 * (UnrealMcpRqlTools::Register); this module only watches for extension RQL modules and republishes the tools — their
 * descriptions and the unreal-rql skill list what the modules add — on the next tick after a module comes or goes.
 */
class FUnrealMcpRqlModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		IModularFeatures& Features = IModularFeatures::Get();
		RegisteredHandle = Features.OnModularFeatureRegistered().AddRaw(this, &FUnrealMcpRqlModule::OnFeature);
		UnregisteredHandle = Features.OnModularFeatureUnregistered().AddRaw(this, &FUnrealMcpRqlModule::OnFeature);
	}

	virtual void ShutdownModule() override
	{
		IModularFeatures& Features = IModularFeatures::Get();
		Features.OnModularFeatureRegistered().Remove(RegisteredHandle);
		Features.OnModularFeatureUnregistered().Remove(UnregisteredHandle);
		if (PendingTick.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(PendingTick);
			PendingTick.Reset();
		}
		UnrealMcpRqlTools::Detach();
	}

private:
	FDelegateHandle RegisteredHandle;
	FDelegateHandle UnregisteredHandle;
	FTSTicker::FDelegateHandle PendingTick;

	void OnFeature(const FName& Type, IModularFeature*)
	{
		if (Type != IUnrealMcpRqlModule::GetModularFeatureName() || PendingTick.IsValid())
			return;
		// Deferred: a module registers from its StartupModule, possibly while the tool registry is mid-rebuild.
		PendingTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
		{
			PendingTick.Reset();
			UnrealMcpRqlTools::Republish();
			return false;
		}));
	}
};

IMPLEMENT_MODULE(FUnrealMcpRqlModule, UnrealMcpRql)
