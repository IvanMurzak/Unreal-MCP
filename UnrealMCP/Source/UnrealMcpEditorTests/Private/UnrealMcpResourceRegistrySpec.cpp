// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "UnrealMcpRuntimeCoreResources.h"
#include "UnrealMcpResourceRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/** Resource registry + core-resource specs (docs/ARCHITECTURE.md §A.1 / §A.2). */
BEGIN_DEFINE_SPEC(FUnrealMcpResourceRegistrySpec, "UnrealMcp.Resources.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpResourceRegistrySpec)

namespace
{
	// Spec-UNIQUE helper names (unity-build ODR). Find the resource descriptor with the given uri in a manifest.
	TSharedPtr<FJsonObject> ResourceSpecFindDescriptor(const TSharedPtr<FJsonObject>& Manifest, const FString& Uri)
	{
		const TArray<TSharedPtr<FJsonValue>>* Resources = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("resources"), Resources))
			return nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Resources)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (Obj.IsValid() && Obj->GetStringField(TEXT("uri")) == Uri)
				return Obj;
		}
		return nullptr;
	}

	/** A test-only resource provider that registers one text resource under a given id/uri. */
	class FResourceSpecProvider
	{
	public:
		static void RegisterInto(FUnrealMcpResourceRegistry& Registry, const FString& Uri)
		{
			Registry.Resource(Uri)
				.Name(TEXT("Spec resource"))
				.Description(TEXT("A spec fixture resource."))
				.MimeType(TEXT("text/plain"))
				.Read([](const FString& U) { return FUnrealMcpResourceResult::Text(U, TEXT("ok"), TEXT("text/plain")); });
		}
	};
}

void FUnrealMcpResourceRegistrySpec::Define()
{
	Describe("core resource registration", [this]()
	{
		It("registers the core resources and bumps the revision", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpCoreResources::Register(Registry);
			TestTrue(TEXT("has unreal://project/levels"), Registry.HasResource(TEXT("unreal://project/levels")));
			TestTrue(TEXT("has unreal://project/icon"), Registry.HasResource(TEXT("unreal://project/icon")));
			TestEqual(TEXT("two resources"), Registry.Num(), 2);
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);
		});

		It("reads unreal://project/levels as a JSON text block (application/json)", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			UnrealMcpCoreResources::Register(Registry);

			const FUnrealMcpResourceResult Result = Registry.Read(TEXT("unreal://project/levels"));
			TestTrue(TEXT("success"), Result.bSuccess);
			TestEqual(TEXT("one content block"), Result.Contents.Num(), 1);
			if (Result.Contents.Num() == 1)
			{
				const FUnrealMcpResourceContent& C = Result.Contents[0];
				TestFalse(TEXT("not a blob"), C.bIsBlob);
				TestEqual(TEXT("mime is application/json"), C.MimeType, FString(TEXT("application/json")));
				TestFalse(TEXT("text non-empty"), C.Text.IsEmpty());

				// The text must be valid JSON carrying the "hasWorld" key (null-world safe in headless).
				TSharedPtr<FJsonObject> Parsed;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(C.Text);
				TestTrue(TEXT("text parses as JSON"), FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid());
				if (Parsed.IsValid())
					TestTrue(TEXT("has hasWorld field"), Parsed->HasField(TEXT("hasWorld")));
			}
		});

		It("reads unreal://project/icon as a base64 PNG blob (image/png) that decodes to PNG-magic bytes", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			UnrealMcpCoreResources::Register(Registry);

			const FUnrealMcpResourceResult Result = Registry.Read(TEXT("unreal://project/icon"));
			TestTrue(TEXT("success"), Result.bSuccess);
			TestEqual(TEXT("one content block"), Result.Contents.Num(), 1);
			if (Result.Contents.Num() == 1)
			{
				const FUnrealMcpResourceContent& C = Result.Contents[0];
				TestTrue(TEXT("is a blob"), C.bIsBlob);
				TestEqual(TEXT("mime is image/png"), C.MimeType, FString(TEXT("image/png")));
				TestTrue(TEXT("text empty (blob XOR text)"), C.Text.IsEmpty());
				TestFalse(TEXT("blob non-empty"), C.Blob.IsEmpty());

				// The base64 blob must decode back to bytes whose 8-byte PNG signature is intact (round-trip).
				TArray<uint8> Decoded;
				if (TestTrue(TEXT("blob decodes from base64"), FBase64::Decode(C.Blob, Decoded)))
				{
					if (TestTrue(TEXT("decoded has >= 8 bytes"), Decoded.Num() >= 8))
					{
						static const uint8 PngMagic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
						bool bMagicOk = true;
						for (int32 i = 0; i < 8; ++i)
							bMagicOk = bMagicOk && (Decoded[i] == PngMagic[i]);
						TestTrue(TEXT("decoded bytes begin with the PNG signature"), bMagicOk);
					}
				}
			}
		});

		It("returns an error for an unknown resource", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			const FUnrealMcpResourceResult Result = Registry.Read(TEXT("unreal://does-not-exist"));
			TestFalse(TEXT("not success"), Result.bSuccess);
		});
	});

	Describe("manifest JSON", [this]()
	{
		It("has type resource-manifest, a revision, and the levels descriptor with uri + mimeType + a schema hash", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			UnrealMcpCoreResources::Register(Registry);

			TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
			TestEqual(TEXT("type"), Manifest->GetStringField(TEXT("type")), FString(TEXT("resource-manifest")));
			TestEqual(TEXT("revision"), (int32)Manifest->GetNumberField(TEXT("revision")), Registry.GetRevision());

			TSharedPtr<FJsonObject> Desc = ResourceSpecFindDescriptor(Manifest, TEXT("unreal://project/levels"));
			if (TestTrue(TEXT("descriptor present"), Desc.IsValid()))
			{
				TestEqual(TEXT("uri"), Desc->GetStringField(TEXT("uri")), FString(TEXT("unreal://project/levels")));
				TestEqual(TEXT("mimeType"), Desc->GetStringField(TEXT("mimeType")), FString(TEXT("application/json")));
				TestFalse(TEXT("schema hash non-empty"), Desc->GetStringField(TEXT("schemaHash")).IsEmpty());
			}
		});

		It("produces a stable schema hash for an unchanged resource", [this]()
		{
			FUnrealMcpResourceRegistry A, B;
			UnrealMcpCoreResources::Register(A);
			UnrealMcpCoreResources::Register(B);
			TestEqual(TEXT("same hash"),
				A.Find(TEXT("unreal://project/levels"))->SchemaHash, B.Find(TEXT("unreal://project/levels"))->SchemaHash);
		});
	});

	Describe("enable filter", [this]()
	{
		It("excludes a disabled resource from the manifest and errors on Read", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			UnrealMcpCoreResources::Register(Registry);

			const int32 RevBefore = Registry.GetRevision();
			TestTrue(TEXT("toggle changed"), Registry.SetResourceEnabled(TEXT("unreal://project/levels"), false));
			TestTrue(TEXT("revision bumped on toggle"), Registry.GetRevision() > RevBefore);

			TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
			TestFalse(TEXT("disabled resource excluded from manifest"),
				ResourceSpecFindDescriptor(Manifest, TEXT("unreal://project/levels")).IsValid());

			const FUnrealMcpResourceResult Result = Registry.Read(TEXT("unreal://project/levels"));
			TestFalse(TEXT("disabled resource errors on Read"), Result.bSuccess);
		});
	});

	Describe("extension registration (§5 isolation, pure registry)", [this]()
	{
		It("registers a valid extension resource under its id and bumps the revision", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			const int32 RevBefore = Registry.GetRevision();
			const FUnrealMcpExtensionRegistrationResult Result = Registry.RegisterExtension(
				TEXT("com.spec.ext"), [](FUnrealMcpResourceRegistry& Reg)
				{
					FResourceSpecProvider::RegisterInto(Reg, TEXT("unreal://ext/resource"));
				});
			TestEqual(TEXT("one entry registered"), Result.ToolsRegistered, 1);
			TestEqual(TEXT("no errors"), Result.Errors.Num(), 0);
			TestTrue(TEXT("ext resource present"), Registry.HasResource(TEXT("unreal://ext/resource")));
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > RevBefore);
			if (const FUnrealMcpRegisteredResource* R = Registry.Find(TEXT("unreal://ext/resource")))
				TestEqual(TEXT("stamped extension id"), R->ExtensionId, FString(TEXT("com.spec.ext")));
		});

		It("rejects a duplicate resource uri within an extension", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			const FUnrealMcpExtensionRegistrationResult Result = Registry.RegisterExtension(
				TEXT("com.spec.dup"), [](FUnrealMcpResourceRegistry& Reg)
				{
					FResourceSpecProvider::RegisterInto(Reg, TEXT("unreal://dup/resource"));
					FResourceSpecProvider::RegisterInto(Reg, TEXT("unreal://dup/resource")); // duplicate -> rejected
				});
			TestEqual(TEXT("only one entry registered"), Result.ToolsRegistered, 1);
			TestTrue(TEXT("a rejection was recorded"), Result.Errors.Num() >= 1);
			TestEqual(TEXT("one resource total"), Registry.Num(), 1);
		});

		It("drops an invalid resource uri", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			const FUnrealMcpExtensionRegistrationResult Result = Registry.RegisterExtension(
				TEXT("com.spec.bad"), [](FUnrealMcpResourceRegistry& Reg)
				{
					Reg.Resource(TEXT("has space")) // whitespace -> invalid uri
						.Name(TEXT("Bad"))
						.Read([](const FString& U) { return FUnrealMcpResourceResult::Text(U, TEXT("ok")); });
				});
			TestEqual(TEXT("nothing registered"), Result.ToolsRegistered, 0);
			TestTrue(TEXT("a drop was recorded"), Result.Errors.Num() >= 1);
			TestEqual(TEXT("no resources"), Registry.Num(), 0);
		});

		It("removes an extension's resources by id (hot-unload), leaving core untouched", [this]()
		{
			FUnrealMcpResourceRegistry Registry;
			UnrealMcpCoreResources::Register(Registry);
			Registry.RegisterExtension(TEXT("com.spec.ext"), [](FUnrealMcpResourceRegistry& Reg)
			{
				FResourceSpecProvider::RegisterInto(Reg, TEXT("unreal://ext/resource"));
			});
			TestEqual(TEXT("three resources total"), Registry.Num(), 3);

			const int32 Removed = Registry.RemoveResourcesForExtension(TEXT("com.spec.ext"));
			TestEqual(TEXT("removed exactly the ext resource"), Removed, 1);
			TestFalse(TEXT("ext resource gone"), Registry.HasResource(TEXT("unreal://ext/resource")));
			TestTrue(TEXT("core levels still present"), Registry.HasResource(TEXT("unreal://project/levels")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
