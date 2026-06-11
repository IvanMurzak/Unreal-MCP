// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Tools/UnrealMcpSourceTools.h"

/**
 * C++ source / script tool family specs (docs/ARCHITECTURE.md §10, issue #18).
 *
 * Fast + deterministic groups (no real UBT invoke):
 *  - registration   — all six tools present with kebab-case ids and the expected hints.
 *  - jail           — ResolveJailedPath accepts in-jail paths and rejects `..` traversal /
 *    absolute-outside escapes (the security-critical surface, proven without a live editor).
 *  - diagnostics    — ParseDiagnostics turns canned MSVC + clang build output into exact
 *    {file,line,severity,message} rows and excludes link-stage (LNK) failures — this is the
 *    structured-report proof for the compile feedback loop.
 *  - file ops       — create-class -> read -> list -> update -> delete round-trip against a temp
 *    module folder under the testbed Source/, plus per-tool jail-escape rejection. The temp folder
 *    is removed in AfterEach so the testbed working tree stays clean.
 *
 * Heavy, env-gated group (real editor + real UBT, only when UNREAL_MCP_RUN_LIVE_COMPILE is set):
 *  - live compile   — create class -> compile (compiler-clean) -> break -> compile (structured
 *    errors with file/line) -> fix -> compile (compiler-clean). Asserts on compiler-error count, not
 *    the process return code: a running editor holds the module DLL, so the link stage cannot relink
 *    it, but the compiler diagnostics (the AI feedback loop) are emitted before linking.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpSourceToolsSpec, "UnrealMcp.Tools.Source",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	FString TempModule;     // temp module folder name under Source/ for the file-op round-trip
	FString TempModuleDir;  // absolute path of that folder
	void RemoveTempModule();
END_DEFINE_SPEC(FUnrealMcpSourceToolsSpec)

namespace
{
	// Uniquely named (not just `Run`/`Obj`) so the helpers never collide with the identically-shaped
	// helpers in the sibling family specs under a UE unity build.
	FUnrealMcpToolResult RunSourceTool(FUnrealMcpToolRegistry& Registry, const FString& Name, const TSharedPtr<FJsonObject>& Args)
	{
		return Registry.Execute(Name, FUnrealMcpToolCall(Args));
	}

	TSharedPtr<FJsonObject> SourceArgs() { return MakeShared<FJsonObject>(); }
}

void FUnrealMcpSourceToolsSpec::RemoveTempModule()
{
	if (!TempModuleDir.IsEmpty() && IFileManager::Get().DirectoryExists(*TempModuleDir))
	{
		IFileManager::Get().DeleteDirectory(*TempModuleDir, /*RequireExists*/ false, /*Tree*/ true);
	}
}

void FUnrealMcpSourceToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers all six source tools with kebab-case ids", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSourceTools::Register(Registry);

			const TArray<FString> Expected = {
				TEXT("source-read"), TEXT("source-create-class"), TEXT("source-update"),
				TEXT("source-delete"), TEXT("source-list"), TEXT("source-compile")
			};
			for (const FString& Name : Expected)
			{
				TestTrue(FString::Printf(TEXT("has %s"), *Name), Registry.HasTool(Name));
				TestTrue(FString::Printf(TEXT("%s is valid kebab id"), *Name), FUnrealMcpToolRegistry::IsValidToolName(Name));
			}
			TestEqual(TEXT("exactly six tools"), Registry.Num(), Expected.Num());
		});

		It("marks source-delete destructive and source-read/list read-only", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSourceTools::Register(Registry);
			TestTrue(TEXT("delete destructive"), Registry.Find(TEXT("source-delete"))->bDestructiveHint);
			TestTrue(TEXT("read read-only"), Registry.Find(TEXT("source-read"))->bReadOnlyHint);
			TestTrue(TEXT("list read-only"), Registry.Find(TEXT("source-list"))->bReadOnlyHint);
		});
	});

	Describe("jail", [this]()
	{
		It("accepts a path inside the jail and reports a normalized relative path", [this]()
		{
			const FString Root = UnrealMcpSourceTools::GetProjectSourceRoot();
			const UnrealMcpSourceTools::FJailedPath R = UnrealMcpSourceTools::ResolveJailedPath(Root, TEXT("MyGame/MyActor.cpp"));
			TestTrue(TEXT("accepted"), R.bOk);
			TestEqual(TEXT("relpath"), R.RelPath, FString(TEXT("MyGame/MyActor.cpp")));
		});

		It("rejects parent-directory traversal", [this]()
		{
			const FString Root = UnrealMcpSourceTools::GetProjectSourceRoot();
			TestFalse(TEXT("../ escapes"), UnrealMcpSourceTools::ResolveJailedPath(Root, TEXT("../Config/secret.ini")).bOk);
			TestFalse(TEXT("deep ../ escapes"), UnrealMcpSourceTools::ResolveJailedPath(Root, TEXT("MyGame/../../../Windows/System32/x")).bOk);
		});

		It("rejects an absolute path outside the jail", [this]()
		{
			const FString Root = UnrealMcpSourceTools::GetProjectSourceRoot();
			TestFalse(TEXT("absolute outside"), UnrealMcpSourceTools::ResolveJailedPath(Root, TEXT("C:/Windows/System32/drivers/etc/hosts")).bOk);
		});

		It("rejects an empty path", [this]()
		{
			const FString Root = UnrealMcpSourceTools::GetProjectSourceRoot();
			TestFalse(TEXT("empty rejected"), UnrealMcpSourceTools::ResolveJailedPath(Root, TEXT("")).bOk);
		});
	});

	Describe("diagnostics", [this]()
	{
		It("parses MSVC errors and warnings and excludes link failures", [this]()
		{
			const FString Output =
				TEXT("Building UnrealTestProjectEditor...\n")
				TEXT("C:\\proj\\Source\\MyGame\\MyActor.cpp(12): error C2065: 'Foo': undeclared identifier\n")
				TEXT("C:\\proj\\Source\\MyGame\\MyActor.cpp(8): warning C4101: 'x': unreferenced local variable\n")
				TEXT("LINK : fatal error LNK1104: cannot open file 'UnrealEditor-UnrealTestProject.dll'\n");

			TArray<UnrealMcpSourceTools::FSourceDiagnostic> Diags;
			UnrealMcpSourceTools::ParseDiagnostics(Output, Diags);

			TestEqual(TEXT("two compiler diagnostics (LNK excluded)"), Diags.Num(), 2);
			if (Diags.Num() == 2)
			{
				TestEqual(TEXT("error file"), Diags[0].File, FString(TEXT("C:\\proj\\Source\\MyGame\\MyActor.cpp")));
				TestEqual(TEXT("error line"), Diags[0].Line, 12);
				TestEqual(TEXT("error severity"), Diags[0].Severity, FString(TEXT("error")));
				TestTrue(TEXT("error message carries code"), Diags[0].Message.Contains(TEXT("C2065")));
				TestEqual(TEXT("warning severity"), Diags[1].Severity, FString(TEXT("warning")));
				TestEqual(TEXT("warning line"), Diags[1].Line, 8);
			}
		});

		It("parses clang-style diagnostics", [this]()
		{
			const FString Output = TEXT("/proj/Source/MyGame/MyActor.cpp:5:9: error: use of undeclared identifier 'Foo'\n");
			TArray<UnrealMcpSourceTools::FSourceDiagnostic> Diags;
			UnrealMcpSourceTools::ParseDiagnostics(Output, Diags);
			TestEqual(TEXT("one diagnostic"), Diags.Num(), 1);
			if (Diags.Num() == 1)
			{
				TestEqual(TEXT("file"), Diags[0].File, FString(TEXT("/proj/Source/MyGame/MyActor.cpp")));
				TestEqual(TEXT("line"), Diags[0].Line, 5);
				TestEqual(TEXT("severity"), Diags[0].Severity, FString(TEXT("error")));
			}
		});

		It("returns no diagnostics for clean output", [this]()
		{
			const FString Output = TEXT("Building UnrealTestProjectEditor...\nTarget is up to date\n");
			TArray<UnrealMcpSourceTools::FSourceDiagnostic> Diags;
			UnrealMcpSourceTools::ParseDiagnostics(Output, Diags);
			TestEqual(TEXT("no diagnostics"), Diags.Num(), 0);
		});
	});

	Describe("file ops", [this]()
	{
		BeforeEach([this]()
		{
			TempModule = TEXT("__UnrealMcpSourceSpec__");
			TempModuleDir = UnrealMcpSourceTools::GetProjectSourceRoot() / TempModule;
			RemoveTempModule();
			IFileManager::Get().MakeDirectory(*TempModuleDir, /*Tree*/ true);
		});

		AfterEach([this]()
		{
			RemoveTempModule();
		});

		It("creates a class, reads it back, lists it, updates and deletes it", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSourceTools::Register(Registry);

			// create-class (Actor parent -> A-prefix, UCLASS).
			TSharedPtr<FJsonObject> Create = SourceArgs();
			Create->SetStringField(TEXT("className"), TEXT("SpecPawnThing"));
			Create->SetStringField(TEXT("module"), TempModule);
			Create->SetStringField(TEXT("parentClass"), TEXT("Actor"));
			const FUnrealMcpToolResult CreateResult = RunSourceTool(Registry, TEXT("source-create-class"), Create);
			TestTrue(TEXT("create succeeded"), CreateResult.bSuccess);
			const FString HeaderRel = TempModule / TEXT("SpecPawnThing.h");
			TestTrue(TEXT("header on disk"), FPaths::FileExists(UnrealMcpSourceTools::GetProjectSourceRoot() / HeaderRel));

			// create-class refuses to overwrite without force.
			TestFalse(TEXT("no overwrite without force"), RunSourceTool(Registry, TEXT("source-create-class"), Create).bSuccess);

			// read the header back and confirm the generated symbol + UCLASS.
			TSharedPtr<FJsonObject> Read = SourceArgs();
			Read->SetStringField(TEXT("path"), HeaderRel);
			const FUnrealMcpToolResult ReadResult = RunSourceTool(Registry, TEXT("source-read"), Read);
			TestTrue(TEXT("read succeeded"), ReadResult.bSuccess);
			FString Content;
			if (ReadResult.Structured.IsValid()) { ReadResult.Structured->TryGetStringField(TEXT("content"), Content); }
			// The MODULE_API macro is derived from the (temp) module name, so assert the
			// module-independent parts: the prefixed symbol, the parent, and the UCLASS marker.
			TestTrue(TEXT("declares ASpecPawnThing : public AActor"), Content.Contains(TEXT("ASpecPawnThing : public AActor")));
			TestTrue(TEXT("is a UCLASS"), Content.Contains(TEXT("UCLASS()")));

			// list the temp module and confirm both files are present with sizes.
			TSharedPtr<FJsonObject> List = SourceArgs();
			List->SetStringField(TEXT("module"), TempModule);
			const FUnrealMcpToolResult ListResult = RunSourceTool(Registry, TEXT("source-list"), List);
			TestTrue(TEXT("list succeeded"), ListResult.bSuccess);
			int32 ListCount = 0;
			if (ListResult.Structured.IsValid()) { ListResult.Structured->TryGetNumberField(TEXT("count"), ListCount); }
			TestEqual(TEXT("two files listed"), ListCount, 2);

			// update the cpp with full-content replacement.
			TSharedPtr<FJsonObject> Update = SourceArgs();
			Update->SetStringField(TEXT("path"), TempModule / TEXT("SpecPawnThing.cpp"));
			Update->SetStringField(TEXT("content"), TEXT("// replaced\n"));
			TestTrue(TEXT("update succeeded"), RunSourceTool(Registry, TEXT("source-update"), Update).bSuccess);

			// update refuses a missing file (use create instead).
			TSharedPtr<FJsonObject> UpdateMissing = SourceArgs();
			UpdateMissing->SetStringField(TEXT("path"), TempModule / TEXT("DoesNotExist.cpp"));
			UpdateMissing->SetStringField(TEXT("content"), TEXT("x"));
			TestFalse(TEXT("update missing file fails"), RunSourceTool(Registry, TEXT("source-update"), UpdateMissing).bSuccess);

			// delete the header.
			TSharedPtr<FJsonObject> Delete = SourceArgs();
			Delete->SetStringField(TEXT("path"), HeaderRel);
			TestTrue(TEXT("delete succeeded"), RunSourceTool(Registry, TEXT("source-delete"), Delete).bSuccess);
			TestFalse(TEXT("header gone"), FPaths::FileExists(UnrealMcpSourceTools::GetProjectSourceRoot() / HeaderRel));
		});

		It("rejects jail escapes on every file op", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSourceTools::Register(Registry);
			const FString Escape = TEXT("../Config/DefaultEngine.ini");

			TSharedPtr<FJsonObject> Read = SourceArgs();
			Read->SetStringField(TEXT("path"), Escape);
			TestFalse(TEXT("read escape rejected"), RunSourceTool(Registry, TEXT("source-read"), Read).bSuccess);

			TSharedPtr<FJsonObject> Update = SourceArgs();
			Update->SetStringField(TEXT("path"), Escape);
			Update->SetStringField(TEXT("content"), TEXT("pwned"));
			TestFalse(TEXT("update escape rejected"), RunSourceTool(Registry, TEXT("source-update"), Update).bSuccess);

			TSharedPtr<FJsonObject> Delete = SourceArgs();
			Delete->SetStringField(TEXT("path"), Escape);
			TestFalse(TEXT("delete escape rejected"), RunSourceTool(Registry, TEXT("source-delete"), Delete).bSuccess);

			TSharedPtr<FJsonObject> Create = SourceArgs();
			Create->SetStringField(TEXT("className"), TEXT("Evil"));
			Create->SetStringField(TEXT("module"), TEXT("../../../Windows"));
			TestFalse(TEXT("create escape rejected"), RunSourceTool(Registry, TEXT("source-create-class"), Create).bSuccess);
		});
	});

	// Heavy, real-UBT round-trip — only when explicitly requested (kept out of the default fast suite).
	if (!FPlatformMisc::GetEnvironmentVariable(TEXT("UNREAL_MCP_RUN_LIVE_COMPILE")).IsEmpty())
	{
		Describe("live compile", [this]()
		{
			It("compiles a new class, reports structured errors when broken, and recovers", [this]()
			{
				FUnrealMcpToolRegistry Registry;
				UnrealMcpSourceTools::Register(Registry);
				const FString Module = FApp::GetProjectName();
				const FString ClassName = TEXT("UnrealMcpLiveCompileProbe");
				const FString HeaderRel = Module / (ClassName + TEXT(".h"));
				const FString CppRel = Module / (ClassName + TEXT(".cpp"));
				const FString HeaderAbs = UnrealMcpSourceTools::GetProjectSourceRoot() / HeaderRel;
				const FString CppAbs = UnrealMcpSourceTools::GetProjectSourceRoot() / CppRel;

				auto Cleanup = [&]()
				{
					IFileManager::Get().Delete(*HeaderAbs, false, true, true);
					IFileManager::Get().Delete(*CppAbs, false, true, true);
				};
				Cleanup();

				auto Compile = [&](int32& OutErrors) -> FUnrealMcpToolResult
				{
					TSharedPtr<FJsonObject> Args = SourceArgs();
					Args->SetBoolField(TEXT("useLiveCoding"), false);
					const FUnrealMcpToolResult Result = RunSourceTool(Registry, TEXT("source-compile"), Args);
					OutErrors = -1;
					if (Result.Structured.IsValid()) { Result.Structured->TryGetNumberField(TEXT("errorCount"), OutErrors); }
					return Result;
				};

				// create -> compile: the new (empty UObject) class must be compiler-clean.
				TSharedPtr<FJsonObject> Create = SourceArgs();
				Create->SetStringField(TEXT("className"), ClassName);
				Create->SetStringField(TEXT("module"), Module);
				Create->SetStringField(TEXT("parentClass"), TEXT("UObject"));
				TestTrue(TEXT("create succeeded"), RunSourceTool(Registry, TEXT("source-create-class"), Create).bSuccess);

				int32 Errors = -1;
				Compile(Errors);
				TestEqual(TEXT("clean after create"), Errors, 0);

				// break the cpp -> compile: structured errors with file + line.
				TSharedPtr<FJsonObject> Break = SourceArgs();
				Break->SetStringField(TEXT("path"), CppRel);
				Break->SetStringField(TEXT("content"),
					FString::Printf(TEXT("#include \"%s.h\"\nthis is not valid c++ ;\n"), *ClassName));
				TestTrue(TEXT("break update succeeded"), RunSourceTool(Registry, TEXT("source-update"), Break).bSuccess);

				FUnrealMcpToolResult BrokenResult = Compile(Errors);
				TestTrue(TEXT("errors reported when broken"), Errors > 0);

				// fix -> compile: compiler-clean again.
				TSharedPtr<FJsonObject> Fix = SourceArgs();
				Fix->SetStringField(TEXT("path"), CppRel);
				Fix->SetStringField(TEXT("content"), FString::Printf(TEXT("#include \"%s.h\"\n"), *ClassName));
				TestTrue(TEXT("fix update succeeded"), RunSourceTool(Registry, TEXT("source-update"), Fix).bSuccess);

				Compile(Errors);
				TestEqual(TEXT("clean after fix"), Errors, 0);

				Cleanup();
			});
		});
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
