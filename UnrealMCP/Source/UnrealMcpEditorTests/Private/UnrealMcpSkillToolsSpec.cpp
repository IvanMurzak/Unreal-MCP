// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpGeneratedSkills.h"
#include "Tools/UnrealMcpSkillTools.h"

/**
 * `unreal-skill-create` specs (docs/ARCHITECTURE.md §2.4).
 *
 * The generator is PURE (spec text in, C++ text out), so everything that matters is asserted without touching
 * disk or the plugin install: the emitted translation unit's shape, the unity-build-safe unique symbol naming,
 * literal escaping, parameter mapping, and the validation gate. The two disk-touching behaviours (writing into
 * the plugin's Skills folder, refusing a source-less precompiled install) are covered by the registration group
 * plus the live Suite 7 e2e — a spec must never write into the plugin's own source tree.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpSkillToolsSpec, "UnrealMcp.Tools.Skills",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	/** A minimal valid spec the individual tests tweak. */
	UnrealMcpSkillTools::FSkillSpec SkillsMakeBaseSpec() const;
END_DEFINE_SPEC(FUnrealMcpSkillToolsSpec)

UnrealMcpSkillTools::FSkillSpec FUnrealMcpSkillToolsSpec::SkillsMakeBaseSpec() const
{
	UnrealMcpSkillTools::FSkillSpec Spec;
	Spec.ToolId = TEXT("my-skill");
	Spec.Description = TEXT("Does a thing.");
	Spec.Body = TEXT("return FUnrealMcpToolResult::Success(TEXT(\"ok\"));");
	return Spec;
}

void FUnrealMcpSkillToolsSpec::Define()
{
	Describe("naming", [this]()
	{
		It("derives a unity-build-safe PascalCase symbol from the tool id", [this]()
		{
			// The modules are unity-built, so two generated files must never share a symbol — the id does that.
			TestEqual(TEXT("simple"), UnrealMcpSkillTools::SkillSymbolFromToolId(TEXT("my-skill")), FString(TEXT("MySkill")));
			TestEqual(TEXT("multi-part"), UnrealMcpSkillTools::SkillSymbolFromToolId(TEXT("a-b-c")), FString(TEXT("ABC")));
			TestEqual(TEXT("single word"), UnrealMcpSkillTools::SkillSymbolFromToolId(TEXT("solo")), FString(TEXT("Solo")));
		});

		It("derives a default title and file name", [this]()
		{
			TestEqual(TEXT("title"), UnrealMcpSkillTools::DefaultTitleFromToolId(TEXT("my-cool-skill")), FString(TEXT("My Cool Skill")));
			TestEqual(TEXT("file"), UnrealMcpSkillTools::SkillFileNameFromToolId(TEXT("my-skill")), FString(TEXT("UnrealMcpSkill_MySkill.cpp")));
		});
	});

	Describe("validation", [this]()
	{
		It("accepts a minimal valid spec", [this]()
		{
			FString Error;
			TestTrue(TEXT("valid"), UnrealMcpSkillTools::ValidateSkillSpec(SkillsMakeBaseSpec(), Error));
			TestTrue(TEXT("no error"), Error.IsEmpty());
		});

		It("rejects a non-kebab tool id", [this]()
		{
			// Delegates to the registry's OWN predicate, so an id this accepts always survives registration.
			UnrealMcpSkillTools::FSkillSpec Spec = SkillsMakeBaseSpec();
			FString Error;

			Spec.ToolId = TEXT("MySkill");
			TestFalse(TEXT("PascalCase rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));
			Spec.ToolId = TEXT("my_skill");
			TestFalse(TEXT("snake_case rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));
			Spec.ToolId = TEXT("my--skill");
			TestFalse(TEXT("double hyphen rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));
			Spec.ToolId = FString();
			TestFalse(TEXT("empty rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));
		});

		It("requires a description and a body", [this]()
		{
			UnrealMcpSkillTools::FSkillSpec Spec = SkillsMakeBaseSpec();
			FString Error;

			Spec.Description = TEXT("   ");
			TestFalse(TEXT("blank description rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));

			Spec = SkillsMakeBaseSpec();
			Spec.Body = FString();
			TestFalse(TEXT("empty body rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));
		});

		It("rejects bad parameters", [this]()
		{
			UnrealMcpSkillTools::FSkillSpec Spec = SkillsMakeBaseSpec();
			FString Error;

			Spec.Params.Add({ TEXT("ok"), TEXT("string"), TEXT("desc"), false });
			TestTrue(TEXT("plain param accepted"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));

			Spec.Params.Add({ TEXT("ok"), TEXT("string"), TEXT("dup"), false });
			TestFalse(TEXT("duplicate name rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));

			Spec = SkillsMakeBaseSpec();
			Spec.Params.Add({ TEXT("bad name"), TEXT("string"), TEXT("desc"), false });
			TestFalse(TEXT("space in name rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));

			Spec = SkillsMakeBaseSpec();
			Spec.Params.Add({ TEXT("1st"), TEXT("string"), TEXT("desc"), false });
			TestFalse(TEXT("leading digit rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));

			Spec = SkillsMakeBaseSpec();
			Spec.Params.Add({ TEXT("thing"), TEXT("vector"), TEXT("desc"), false });
			TestFalse(TEXT("unsupported type rejected"), UnrealMcpSkillTools::ValidateSkillSpec(Spec, Error));
		});
	});

	Describe("literal escaping", [this]()
	{
		It("escapes quotes, backslashes and newlines", [this]()
		{
			TestEqual(TEXT("quote"), UnrealMcpSkillTools::EscapeCppStringLiteral(TEXT("say \"hi\"")), FString(TEXT("say \\\"hi\\\"")));
			TestEqual(TEXT("backslash"), UnrealMcpSkillTools::EscapeCppStringLiteral(TEXT("C:\\dir")), FString(TEXT("C:\\\\dir")));
			TestEqual(TEXT("newline"), UnrealMcpSkillTools::EscapeCppStringLiteral(TEXT("a\nb")), FString(TEXT("a\\nb")));
		});
	});

	Describe("generated source", [this]()
	{
		It("emits a self-registering translation unit", [this]()
		{
			const FString Code = UnrealMcpSkillTools::BuildSkillSourceCode(SkillsMakeBaseSpec());

			TestTrue(TEXT("copyright header"), Code.StartsWith(TEXT("// Copyright (c) 2026 Ivan Murzak.")));
			TestTrue(TEXT("generated banner"), Code.Contains(TEXT("GENERATED by the `unreal-skill-create`")));
			TestTrue(TEXT("includes the bus header"), Code.Contains(TEXT("#include \"Tools/UnrealMcpGeneratedSkills.h\"")));
			TestTrue(TEXT("includes the registry header"), Code.Contains(TEXT("#include \"UnrealMcpToolRegistry.h\"")));
			// The unique namespace is what keeps two generated files from colliding in the unity build.
			TestTrue(TEXT("unique namespace"), Code.Contains(TEXT("namespace UnrealMcpGeneratedSkill_MySkill")));
			TestTrue(TEXT("declares the tool"), Code.Contains(TEXT("Registry.Tool(TEXT(\"my-skill\"))")));
			TestTrue(TEXT("default title"), Code.Contains(TEXT(".Title(TEXT(\"My Skill\"))")));
			TestTrue(TEXT("description"), Code.Contains(TEXT(".Description(TEXT(\"Does a thing.\"))")));
			TestTrue(TEXT("handler"), Code.Contains(TEXT(".Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult")));
			TestTrue(TEXT("body carried through"), Code.Contains(TEXT("return FUnrealMcpToolResult::Success(TEXT(\"ok\"));")));
			// Self-registration: this line is why no other file has to be edited per generated skill.
			TestTrue(TEXT("self-registrar"), Code.Contains(TEXT("static const FUnrealMcpGeneratedSkillRegistrar Registrar(&Register);")));
		});

		It("maps each declared parameter to its typed builder call", [this]()
		{
			UnrealMcpSkillTools::FSkillSpec Spec = SkillsMakeBaseSpec();
			Spec.Params.Add({ TEXT("name"), TEXT("string"), TEXT("A name."), true });
			Spec.Params.Add({ TEXT("count"), TEXT("integer"), TEXT("How many."), false });
			Spec.Params.Add({ TEXT("scale"), TEXT("number"), TEXT("A scale."), false });
			Spec.Params.Add({ TEXT("flag"), TEXT("boolean"), TEXT("A flag."), false });

			const FString Code = UnrealMcpSkillTools::BuildSkillSourceCode(Spec);
			TestTrue(TEXT("required string"),
				Code.Contains(TEXT(".ParamString(TEXT(\"name\"), TEXT(\"A name.\"), EUnrealMcpParamRequirement::Required)")));
			TestTrue(TEXT("optional int"), Code.Contains(TEXT(".ParamInt(TEXT(\"count\"), TEXT(\"How many.\"))")));
			TestTrue(TEXT("optional number"), Code.Contains(TEXT(".ParamNumber(TEXT(\"scale\"), TEXT(\"A scale.\"))")));
			TestTrue(TEXT("optional bool"), Code.Contains(TEXT(".ParamBool(TEXT(\"flag\"), TEXT(\"A flag.\"))")));
		});

		It("emits only the hints that were requested", [this]()
		{
			UnrealMcpSkillTools::FSkillSpec Spec = SkillsMakeBaseSpec();
			TestFalse(TEXT("no hints by default"),
				UnrealMcpSkillTools::BuildSkillSourceCode(Spec).Contains(TEXT(".ReadOnlyHint(")));

			Spec.bReadOnlyHint = true;
			Spec.bIdempotentHint = true;
			const FString Code = UnrealMcpSkillTools::BuildSkillSourceCode(Spec);
			TestTrue(TEXT("read-only"), Code.Contains(TEXT(".ReadOnlyHint(true)")));
			TestTrue(TEXT("idempotent"), Code.Contains(TEXT(".IdempotentHint(true)")));
			TestFalse(TEXT("not destructive"), Code.Contains(TEXT(".DestructiveHint(")));
		});

		It("escapes an embedded quote in the description", [this]()
		{
			// A raw quote here would produce a file that does not compile — the whole point of "emits
			// compilable C++".
			UnrealMcpSkillTools::FSkillSpec Spec = SkillsMakeBaseSpec();
			Spec.Description = TEXT("Says \"hello\".");
			const FString Code = UnrealMcpSkillTools::BuildSkillSourceCode(Spec);
			TestTrue(TEXT("escaped"), Code.Contains(TEXT(".Description(TEXT(\"Says \\\"hello\\\".\"))")));
		});

		It("is deterministic", [this]()
		{
			TestEqual(TEXT("same input -> same output"),
				UnrealMcpSkillTools::BuildSkillSourceCode(SkillsMakeBaseSpec()),
				UnrealMcpSkillTools::BuildSkillSourceCode(SkillsMakeBaseSpec()));
		});
	});

	Describe("registration", [this]()
	{
		It("registers unreal-skill-create with the expected argument surface", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSkillTools::Register(Registry);

			const FUnrealMcpRegisteredTool* Tool = Registry.Find(FString(UnrealMcpSkillTools::SkillCreateToolId));
			TestTrue(TEXT("registered"), Tool != nullptr);
			if (Tool == nullptr)
				return;

			TArray<FString> ParamNames;
			for (const FUnrealMcpParamSpec& Param : Tool->Params)
				ParamNames.Add(Param.Name);
			TestTrue(TEXT("toolId param"), ParamNames.Contains(TEXT("toolId")));
			TestTrue(TEXT("description param"), ParamNames.Contains(TEXT("description")));
			TestTrue(TEXT("body param"), ParamNames.Contains(TEXT("body")));
			TestTrue(TEXT("params param"), ParamNames.Contains(TEXT("params")));
		});

		It("rejects an invalid tool id at call time without writing anything", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSkillTools::Register(Registry);

			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("toolId"), TEXT("Not Kebab"));
			Args->SetStringField(TEXT("description"), TEXT("d"));
			Args->SetStringField(TEXT("body"), TEXT("return FUnrealMcpToolResult::Success(TEXT(\"ok\"));"));

			const FUnrealMcpToolResult Result =
				Registry.Execute(FString(UnrealMcpSkillTools::SkillCreateToolId), FUnrealMcpToolCall(Args));
			TestFalse(TEXT("failed"), Result.bSuccess);
			TestTrue(TEXT("names the problem"), Result.Message.Contains(TEXT("kebab-case")));
		});

		It("refuses to shadow an already-registered tool", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSkillTools::Register(Registry);

			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			// Target the skill tool itself — guaranteed present, no dependency on another family.
			Args->SetStringField(TEXT("toolId"), FString(UnrealMcpSkillTools::SkillCreateToolId));
			Args->SetStringField(TEXT("description"), TEXT("d"));
			Args->SetStringField(TEXT("body"), TEXT("return FUnrealMcpToolResult::Success(TEXT(\"ok\"));"));

			const FUnrealMcpToolResult Result =
				Registry.Execute(FString(UnrealMcpSkillTools::SkillCreateToolId), FUnrealMcpToolCall(Args));
			TestFalse(TEXT("failed"), Result.bSuccess);
			TestTrue(TEXT("names the collision"), Result.Message.Contains(TEXT("already registered")));
		});
	});

	Describe("generated-skill bus", [this]()
	{
		It("runs every self-registered skill against the registry", [this]()
		{
			// A clean checkout has no generated skills, so Register() must be a safe no-op; the count is
			// whatever this build compiled in.
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.Num();
			UnrealMcpGeneratedSkills::Register(Registry);
			TestEqual(TEXT("registered exactly the compiled-in generated skills"),
				Registry.Num(), Before + UnrealMcpGeneratedSkills::Num());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
