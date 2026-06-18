// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpRuntimeCorePrompts.h"
#include "UnrealMcpPromptRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** Prompt registry + core-prompt specs (docs/ARCHITECTURE.md §A.1 / §A.2). */
BEGIN_DEFINE_SPEC(FUnrealMcpPromptRegistrySpec, "UnrealMcp.Prompts.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealMcpPromptRegistrySpec)

namespace
{
	// Spec-UNIQUE helper names (unity-build ODR). Find the prompt descriptor with the given name in a manifest.
	TSharedPtr<FJsonObject> PromptSpecFindDescriptor(const TSharedPtr<FJsonObject>& Manifest, const FString& Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Prompts = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("prompts"), Prompts))
			return nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Prompts)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (Obj.IsValid() && Obj->GetStringField(TEXT("name")) == Name)
				return Obj;
		}
		return nullptr;
	}

	/** A test-only prompt provider that registers one prompt under a given id/name. */
	class FPromptSpecProvider
	{
	public:
		static void RegisterInto(FUnrealMcpPromptRegistry& Registry, const FString& Name)
		{
			Registry.Prompt(Name)
				.Title(TEXT("Spec prompt"))
				.Description(TEXT("A spec fixture prompt."))
				.Role(EUnrealMcpPromptRole::User)
				.ParamString(TEXT("theme"), TEXT("a theme"), EUnrealMcpParamRequirement::Required)
				.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpPromptResult::Success(TEXT("ok"), EUnrealMcpPromptRole::User); });
		}
	};
}

void FUnrealMcpPromptRegistrySpec::Define()
{
	Describe("core prompt registration", [this]()
	{
		It("registers level-design-brief and bumps the revision", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpCorePrompts::Register(Registry);
			TestTrue(TEXT("has level-design-brief"), Registry.HasPrompt(TEXT("level-design-brief")));
			TestEqual(TEXT("one prompt"), Registry.Num(), 1);
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);
		});

		It("executes with a theme arg -> templated User message (success)", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			UnrealMcpCorePrompts::Register(Registry);

			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("theme"), TEXT("haunted forest"));
			FUnrealMcpToolCall Call(Args);

			const FUnrealMcpPromptResult Result = Registry.Execute(TEXT("level-design-brief"), Call);
			TestTrue(TEXT("success"), Result.bSuccess);
			TestEqual(TEXT("one message"), Result.Messages.Num(), 1);
			if (Result.Messages.Num() == 1)
			{
				TestTrue(TEXT("role is User"), Result.Messages[0].Role == EUnrealMcpPromptRole::User);
				TestTrue(TEXT("templated theme present"), Result.Messages[0].Text.Contains(TEXT("haunted forest")));
			}
		});

		It("returns an error when theme is missing", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			UnrealMcpCorePrompts::Register(Registry);
			const FUnrealMcpPromptResult Result = Registry.Execute(TEXT("level-design-brief"), FUnrealMcpToolCall());
			TestFalse(TEXT("not success"), Result.bSuccess);
		});

		It("returns an error for an unknown prompt", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			const FUnrealMcpPromptResult Result = Registry.Execute(TEXT("does-not-exist"), FUnrealMcpToolCall());
			TestFalse(TEXT("not success"), Result.bSuccess);
		});
	});

	Describe("manifest JSON", [this]()
	{
		It("has type prompt-manifest, a revision, and the level-design-brief descriptor with role=user + required theme + a schema hash", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			UnrealMcpCorePrompts::Register(Registry);

			TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
			TestEqual(TEXT("type"), Manifest->GetStringField(TEXT("type")), FString(TEXT("prompt-manifest")));
			TestEqual(TEXT("revision"), (int32)Manifest->GetNumberField(TEXT("revision")), Registry.GetRevision());

			TSharedPtr<FJsonObject> Desc = PromptSpecFindDescriptor(Manifest, TEXT("level-design-brief"));
			if (TestTrue(TEXT("descriptor present"), Desc.IsValid()))
			{
				TestEqual(TEXT("role"), Desc->GetStringField(TEXT("role")), FString(TEXT("user")));
				TestFalse(TEXT("schema hash non-empty"), Desc->GetStringField(TEXT("schemaHash")).IsEmpty());

				const TSharedPtr<FJsonObject>* InputSchema = nullptr;
				if (TestTrue(TEXT("has inputSchema"), Desc->TryGetObjectField(TEXT("inputSchema"), InputSchema)))
				{
					const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
					if (TestTrue(TEXT("inputSchema has required[]"), (*InputSchema)->TryGetArrayField(TEXT("required"), Required)))
					{
						bool bThemeRequired = false;
						for (const TSharedPtr<FJsonValue>& V : *Required)
						{
							FString Field;
							if (V->TryGetString(Field) && Field == TEXT("theme"))
								bThemeRequired = true;
						}
						TestTrue(TEXT("theme is required"), bThemeRequired);
					}
				}
			}
		});

		It("produces a stable schema hash for an unchanged prompt", [this]()
		{
			FUnrealMcpPromptRegistry A, B;
			UnrealMcpCorePrompts::Register(A);
			UnrealMcpCorePrompts::Register(B);
			TestEqual(TEXT("same hash"),
				A.Find(TEXT("level-design-brief"))->SchemaHash, B.Find(TEXT("level-design-brief"))->SchemaHash);
		});
	});

	Describe("enable filter", [this]()
	{
		It("excludes a disabled prompt from the manifest and errors on Execute", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			UnrealMcpCorePrompts::Register(Registry);

			const int32 RevBefore = Registry.GetRevision();
			TestTrue(TEXT("toggle changed"), Registry.SetPromptEnabled(TEXT("level-design-brief"), false));
			TestTrue(TEXT("revision bumped on toggle"), Registry.GetRevision() > RevBefore);

			TSharedPtr<FJsonObject> Manifest = Registry.BuildManifestJson();
			TestFalse(TEXT("disabled prompt excluded from manifest"),
				PromptSpecFindDescriptor(Manifest, TEXT("level-design-brief")).IsValid());

			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("theme"), TEXT("x"));
			const FUnrealMcpPromptResult Result = Registry.Execute(TEXT("level-design-brief"), FUnrealMcpToolCall(Args));
			TestFalse(TEXT("disabled prompt errors on Execute"), Result.bSuccess);
		});
	});

	Describe("extension registration (§5 isolation, pure registry)", [this]()
	{
		It("registers a valid extension prompt under its id and bumps the revision", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			const int32 RevBefore = Registry.GetRevision();
			const FUnrealMcpExtensionRegistrationResult Result = Registry.RegisterExtension(
				TEXT("com.spec.ext"), [](FUnrealMcpPromptRegistry& Reg)
				{
					FPromptSpecProvider::RegisterInto(Reg, TEXT("ext-prompt"));
				});
			TestEqual(TEXT("one entry registered"), Result.ToolsRegistered, 1);
			TestEqual(TEXT("no errors"), Result.Errors.Num(), 0);
			TestTrue(TEXT("ext prompt present"), Registry.HasPrompt(TEXT("ext-prompt")));
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > RevBefore);
			if (const FUnrealMcpRegisteredPrompt* P = Registry.Find(TEXT("ext-prompt")))
				TestEqual(TEXT("stamped extension id"), P->ExtensionId, FString(TEXT("com.spec.ext")));
		});

		It("rejects a duplicate prompt name within an extension", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			const FUnrealMcpExtensionRegistrationResult Result = Registry.RegisterExtension(
				TEXT("com.spec.dup"), [](FUnrealMcpPromptRegistry& Reg)
				{
					FPromptSpecProvider::RegisterInto(Reg, TEXT("dup-prompt"));
					FPromptSpecProvider::RegisterInto(Reg, TEXT("dup-prompt")); // duplicate -> rejected
				});
			TestEqual(TEXT("only one entry registered"), Result.ToolsRegistered, 1);
			TestTrue(TEXT("a rejection was recorded"), Result.Errors.Num() >= 1);
			TestEqual(TEXT("one prompt total"), Registry.Num(), 1);
		});

		It("drops an invalid prompt name", [this]()
		{
			FUnrealMcpPromptRegistry Registry;
			const FUnrealMcpExtensionRegistrationResult Result = Registry.RegisterExtension(
				TEXT("com.spec.bad"), [](FUnrealMcpPromptRegistry& Reg)
				{
					Reg.Prompt(TEXT("Bad Name")) // spaces + uppercase -> invalid kebab
						.Title(TEXT("Bad"))
						.Handle([](const FUnrealMcpToolCall&) { return FUnrealMcpPromptResult::Success(TEXT("ok"), EUnrealMcpPromptRole::User); });
				});
			TestEqual(TEXT("nothing registered"), Result.ToolsRegistered, 0);
			TestTrue(TEXT("a drop was recorded"), Result.Errors.Num() >= 1);
			TestEqual(TEXT("no prompts"), Registry.Num(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
