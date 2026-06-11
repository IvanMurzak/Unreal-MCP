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

		It("parses MSVC and clang 'fatal error' compiler diagnostics (e.g. C1083 missing include)", [this]()
		{
			// A missing/typo'd #include is one of the most common AI-edit failures; both toolchains
			// emit it as a file(line)-attributed "fatal error" that must surface as a normal error row.
			const FString Output =
				TEXT("C:\\proj\\Source\\MyGame\\MyActor.cpp(1): fatal error C1083: Cannot open include file: 'X.h': No such file or directory\n")
				TEXT("/proj/Source/MyGame/Other.cpp:10:10: fatal error: 'Y.h' file not found\n")
				TEXT("LINK : fatal error LNK1104: cannot open file 'UnrealEditor-UnrealTestProject.dll'\n");

			TArray<UnrealMcpSourceTools::FSourceDiagnostic> Diags;
			UnrealMcpSourceTools::ParseDiagnostics(Output, Diags);

			// The two compiler-stage fatals are reported; the LNK link-stage fatal is still excluded.
			TestEqual(TEXT("two fatal compiler diagnostics (LNK excluded)"), Diags.Num(), 2);
			if (Diags.Num() == 2)
			{
				TestEqual(TEXT("msvc fatal file"), Diags[0].File, FString(TEXT("C:\\proj\\Source\\MyGame\\MyActor.cpp")));
				TestEqual(TEXT("msvc fatal line"), Diags[0].Line, 1);
				TestEqual(TEXT("msvc fatal severity normalized to error"), Diags[0].Severity, FString(TEXT("error")));
				TestTrue(TEXT("msvc fatal message carries code"), Diags[0].Message.Contains(TEXT("C1083")));
				TestEqual(TEXT("clang fatal file"), Diags[1].File, FString(TEXT("/proj/Source/MyGame/Other.cpp")));
				TestEqual(TEXT("clang fatal line"), Diags[1].Line, 10);
				TestEqual(TEXT("clang fatal severity normalized to error"), Diags[1].Severity, FString(TEXT("error")));
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

		It("splices a line range with exact bytes and never accumulates blank lines across edits", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSourceTools::Register(Registry);

			// A newline-terminated, LF, 4-line file. The phantom trailing empty element that
			// ParseIntoArrayLines yields for such a file used to double the final newline on EVERY range
			// splice (accumulating one blank line per edit) and inflate the addressable range by 1.
			const FString FileRel = TempModule / TEXT("Range.cpp");
			const FString FileAbs = UnrealMcpSourceTools::GetProjectSourceRoot() / FileRel;
			TestTrue(TEXT("seed file written"),
				FFileHelper::SaveStringToFile(FString(TEXT("L1\nL2\nL3\nL4\n")), *FileAbs, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

			auto SpliceLine = [&](int32 Line, const TCHAR* Text) -> bool
			{
				TSharedPtr<FJsonObject> Args = SourceArgs();
				Args->SetStringField(TEXT("path"), FileRel);
				Args->SetStringField(TEXT("content"), Text);
				Args->SetNumberField(TEXT("startLine"), Line);
				Args->SetNumberField(TEXT("endLine"), Line);
				return RunSourceTool(Registry, TEXT("source-update"), Args).bSuccess;
			};

			TestTrue(TEXT("first range splice ok"), SpliceLine(2, TEXT("X2")));
			TestTrue(TEXT("second range splice ok"), SpliceLine(3, TEXT("X3")));

			FString OnDisk;
			TestTrue(TEXT("read seed back"), FFileHelper::LoadFileToString(OnDisk, *FileAbs));
			// Exactly the two edits applied, single trailing newline, NO extra blank lines accumulated.
			TestEqual(TEXT("exact bytes after two consecutive range edits"), OnDisk, FString(TEXT("L1\nX2\nX3\nL4\n")));

			// The phantom no longer inflates the addressable range: line 5 (== real lines + 1) is rejected.
			TSharedPtr<FJsonObject> OutOfRange = SourceArgs();
			OutOfRange->SetStringField(TEXT("path"), FileRel);
			OutOfRange->SetStringField(TEXT("content"), TEXT("X"));
			OutOfRange->SetNumberField(TEXT("startLine"), 5);
			OutOfRange->SetNumberField(TEXT("endLine"), 5);
			TestFalse(TEXT("line 5 (real lines + 1) rejected"), RunSourceTool(Registry, TEXT("source-update"), OutOfRange).bSuccess);

			// A huge startLine must be rejected in int64 BEFORE narrowing — a raw (int32) cast wraps
			// 4294967297 -> 1, which would otherwise splice line 1. Validate-then-narrow rejects it.
			TSharedPtr<FJsonObject> Wrap = SourceArgs();
			Wrap->SetStringField(TEXT("path"), FileRel);
			Wrap->SetStringField(TEXT("content"), TEXT("X"));
			Wrap->SetNumberField(TEXT("startLine"), 4294967297.0);
			Wrap->SetNumberField(TEXT("endLine"), 4294967297.0);
			TestFalse(TEXT("wraparound startLine rejected"), RunSourceTool(Registry, TEXT("source-update"), Wrap).bSuccess);

			// The rejected edits left the file byte-identical.
			FString StillOnDisk;
			TestTrue(TEXT("re-read after rejected edits"), FFileHelper::LoadFileToString(StillOnDisk, *FileAbs));
			TestEqual(TEXT("file unchanged after rejected edits"), StillOnDisk, FString(TEXT("L1\nX2\nX3\nL4\n")));
		});

		It("reports totalLines as real lines for newline-terminated and unterminated files", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSourceTools::Register(Registry);

			auto ReadTotalLines = [&](const TCHAR* Body, const TCHAR* Name) -> int32
			{
				const FString Rel = TempModule / Name;
				const FString Abs = UnrealMcpSourceTools::GetProjectSourceRoot() / Rel;
				FFileHelper::SaveStringToFile(FString(Body), *Abs, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				TSharedPtr<FJsonObject> Args = SourceArgs();
				Args->SetStringField(TEXT("path"), Rel);
				const FUnrealMcpToolResult R = RunSourceTool(Registry, TEXT("source-read"), Args);
				int32 Total = -1;
				if (R.Structured.IsValid()) { R.Structured->TryGetNumberField(TEXT("totalLines"), Total); }
				return Total;
			};

			// Both hold three visible lines; the phantom trailing empty element previously made the
			// newline-terminated file over-report 4.
			TestEqual(TEXT("newline-terminated -> 3 lines"), ReadTotalLines(TEXT("A\nB\nC\n"), TEXT("Term.cpp")), 3);
			TestEqual(TEXT("unterminated -> 3 lines"), ReadTotalLines(TEXT("A\nB\nC"), TEXT("Unterm.cpp")), 3);
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

	Describe("compile arg validation", [this]()
	{
		It("rejects injection in target/platform/configuration before launching UBT", [this]()
		{
			// target/platform/configuration are pasted into the UBT command line; a value carrying
			// whitespace, a quote, or a dash-prefixed flag would inject arbitrary UBT args (e.g. -Clean
			// wipes Binaries). Each must be rejected as a single identifier token BEFORE any process
			// launch — so these calls fail fast and never invoke a real build.
			FUnrealMcpToolRegistry Registry;
			UnrealMcpSourceTools::Register(Registry);

			TSharedPtr<FJsonObject> BadTarget = SourceArgs();
			BadTarget->SetStringField(TEXT("target"), TEXT("UnrealTestProjectEditor Win64 Development -Clean"));
			TestFalse(TEXT("whitespace/flag target rejected"), RunSourceTool(Registry, TEXT("source-compile"), BadTarget).bSuccess);

			TSharedPtr<FJsonObject> DashTarget = SourceArgs();
			DashTarget->SetStringField(TEXT("target"), TEXT("-Mode=QueryTargets"));
			TestFalse(TEXT("dash-prefixed target rejected"), RunSourceTool(Registry, TEXT("source-compile"), DashTarget).bSuccess);

			TSharedPtr<FJsonObject> BadPlatform = SourceArgs();
			BadPlatform->SetStringField(TEXT("platform"), TEXT("Win64 -Clean"));
			TestFalse(TEXT("whitespace platform rejected"), RunSourceTool(Registry, TEXT("source-compile"), BadPlatform).bSuccess);

			TSharedPtr<FJsonObject> BadConfig = SourceArgs();
			BadConfig->SetStringField(TEXT("configuration"), TEXT("Development\" -project=\"x"));
			TestFalse(TEXT("quote-injection configuration rejected"), RunSourceTool(Registry, TEXT("source-compile"), BadConfig).bSuccess);
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
				// WARNING: this env-gated spec writes a probe class into the REAL primary module's Source/.
				// AfterEach-style cleanup runs only if the It-body completes; an abort/crash between the
				// "break" and "fix" steps below strands invalid C++ that would break subsequent builds.
				// This leading Cleanup() pre-clean removes any such stranded probe from a prior aborted run.
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
				// Strengthen the structured-report proof: at least one diagnostic row must carry the
				// probe cpp's file path and a real (1-based) line number, not just a non-zero count.
				bool bHasLocatedDiag = false;
				if (BrokenResult.Structured.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* DiagArray = nullptr;
					if (BrokenResult.Structured->TryGetArrayField(TEXT("diagnostics"), DiagArray))
					{
						for (const TSharedPtr<FJsonValue>& Entry : *DiagArray)
						{
							const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
							if (Obj.IsValid())
							{
								FString File;
								int32 LineNo = 0;
								Obj->TryGetStringField(TEXT("file"), File);
								Obj->TryGetNumberField(TEXT("line"), LineNo);
								if (!File.IsEmpty() && LineNo > 0) { bHasLocatedDiag = true; break; }
							}
						}
					}
				}
				TestTrue(TEXT("a diagnostic row carries file + line"), bHasLocatedDiag);

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
