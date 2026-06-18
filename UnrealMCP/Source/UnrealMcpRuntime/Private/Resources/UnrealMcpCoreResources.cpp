// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRuntimeCoreResources.h"
#include "UnrealMcpResourceRegistry.h"
#include "Tools/UnrealMcpWorldProvider.h"

#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Misc/Base64.h"
#include "Dom/JsonObject.h"          // FJsonObject::SetStringField overloads — NOT transitively present in the Game build
#include "Dom/JsonValue.h"           // FJsonValueObject for the levels[] array
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"         // UObject::GetOutermost()->GetName() (UPackage) — Game-build standalone include

/**
 * The core resources (docs/ARCHITECTURE.md §A.1). Self-contained, Engine-only handlers — no UnrealEd symbols
 * (Model-A runtime safety, §A.5 risk 5) — so the family compiles + links in both the editor and the
 * non-editor Game target. Both run ON the game thread (the dispatcher guarantees it, §4).
 *
 * Unity-build ODR note: this file shares a TU with the tool/prompt core families. The helpers here are given
 * family-unique names (Resource*-prefixed) to dodge a same-name/same-signature collision (conventions.md).
 */
namespace
{
	/** Serialize a JSON object to a compact string (family-unique name to avoid a unity-build ODR clash). */
	FString ResourceCoreSerializeJson(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}

	/** Build the unreal://project/levels JSON body from the active world (Engine-only; null-world safe). */
	FString ResourceBuildLevelsJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

		UWorld* World = FUnrealMcpWorldProvider::GetActiveWorld();
		if (World == nullptr)
		{
			// No world resolved (un-wired runtime module, or no active world yet) — return an honest empty
			// snapshot rather than failing, so the resource read still round-trips deterministically.
			Root->SetBoolField(TEXT("hasWorld"), false);
			Root->SetArrayField(TEXT("levels"), TArray<TSharedPtr<FJsonValue>>());
			return ResourceCoreSerializeJson(Root);
		}

		Root->SetBoolField(TEXT("hasWorld"), true);
		Root->SetStringField(TEXT("worldName"), World->GetName());

		TArray<TSharedPtr<FJsonValue>> Levels;

		// The persistent level first.
		if (ULevel* Persistent = World->PersistentLevel)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Persistent->GetOutermost()->GetName());
			Entry->SetBoolField(TEXT("persistent"), true);
			Entry->SetBoolField(TEXT("loaded"), true);
			Levels.Add(MakeShared<FJsonValueObject>(Entry));
		}

		// Then every streaming level (Engine-module API — UWorld::GetStreamingLevels / ULevelStreaming).
		for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
		{
			if (Streaming == nullptr)
				continue;
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Streaming->GetWorldAssetPackageName());
			Entry->SetBoolField(TEXT("persistent"), false);
			Entry->SetBoolField(TEXT("loaded"), Streaming->IsLevelLoaded());
			Levels.Add(MakeShared<FJsonValueObject>(Entry));
		}

		Root->SetNumberField(TEXT("count"), Levels.Num());
		Root->SetArrayField(TEXT("levels"), Levels);
		return ResourceCoreSerializeJson(Root);
	}
}

namespace UnrealMcpCoreResources
{
	UNREALMCPRUNTIME_API void Register(FUnrealMcpResourceRegistry& Registry)
	{
		// 1. unreal://project/levels — a JSON snapshot of the active world + its levels (application/json).
		Registry.Resource(TEXT("unreal://project/levels"))
			.Name(TEXT("Project Levels"))
			.Description(TEXT("A JSON snapshot of the active world and its loaded/streaming levels."))
			.MimeType(TEXT("application/json"))
			.Read([](const FString& Uri) -> FUnrealMcpResourceResult
			{
				// Runs ON the game thread (the dispatcher guarantees it, §4) — engine-only, no UnrealEd symbols.
				return FUnrealMcpResourceResult::Text(Uri, ResourceBuildLevelsJson(), TEXT("application/json"));
			});

		// 2. unreal://project/icon — a tiny base64 PNG, covering the Blob (binary) round-trip end to end.
		//    A 1x1 transparent PNG: the smallest valid PNG, base64-encoded. We re-encode from raw bytes at
		//    read time so the path exercises FBase64::Encode exactly like a real screenshot/asset blob would.
		Registry.Resource(TEXT("unreal://project/icon"))
			.Name(TEXT("Project Icon"))
			.Description(TEXT("A 1x1 transparent PNG, returned as a base64 blob (binary round-trip sample)."))
			.MimeType(TEXT("image/png"))
			.Read([](const FString& Uri) -> FUnrealMcpResourceResult
			{
				// A minimal valid 1x1 transparent PNG (67 bytes). Held as raw bytes so the handler exercises the
				// real FBase64::Encode path (not a hard-coded base64 literal) — the same path a screenshot blob uses.
				static const uint8 PngBytes[] = {
					0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
					0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
					0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
					0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
					0x42, 0x60, 0x82
				};

				const FString Base64 = FBase64::Encode(PngBytes, sizeof(PngBytes));
				return FUnrealMcpResourceResult::Blob(Uri, Base64, TEXT("image/png"));
			});
	}
}
