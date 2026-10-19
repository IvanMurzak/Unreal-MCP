// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "UnrealMcpToolRegistry.h"
#include "Session/UnrealMcpEditorSession.h"

/**
 * The editor half of a CLI session (Code-Exec plan §4.6): the AIGD_SESSION_* environment, the idle timer, the lease
 * compare-and-delete and the `editor-quit` guard. The quit itself (save + RequestExit) is covered live by the CLI e2e.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpEditorSessionSpec, "UnrealMcp.EditorSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	FString SessionSpecTempDir;

	FString SessionSpecWriteLease(const FString& Nonce)
	{
		const FString Path = FUnrealMcpEditorSession::LeasePath(SessionSpecTempDir);
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"pid\":1,\"nonce\":\"%s\",\"ownedBy\":\"cli\"}"), *Nonce), *Path);
		return Path;
	}

	FUnrealMcpToolResult SessionSpecQuit(const FUnrealMcpSessionEnv& Env, const FString& Nonce)
	{
		const TSharedRef<FUnrealMcpEditorSession> Session = MakeShared<FUnrealMcpEditorSession>(Env, SessionSpecTempDir);
		FUnrealMcpToolRegistry Registry;
		Session->RegisterTools(Registry);
		Session->Disarm(); // a granted request must not really quit the test editor
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("nonce"), Nonce);
		return Registry.Execute(FUnrealMcpEditorSession::QuitToolName, FUnrealMcpToolCall(Args));
	}

END_DEFINE_SPEC(FUnrealMcpEditorSessionSpec)

void FUnrealMcpEditorSessionSpec::Define()
{
	BeforeEach([this]()
	{
		SessionSpecTempDir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("EditorSession"), FGuid::NewGuid().ToString());
		IFileManager::Get().MakeDirectory(*FPaths::Combine(SessionSpecTempDir, TEXT(".ai-game-dev")), /*Tree*/ true);
	});

	AfterEach([this]()
	{
		IFileManager::Get().DeleteDirectory(*SessionSpecTempDir, /*RequireExists*/ false, /*Tree*/ true);
	});

	Describe("environment", [this]()
	{
		It("reads the idle seconds, owner and nonce", [this]()
		{
			const FUnrealMcpSessionEnv Env = FUnrealMcpSessionEnv::Parse(TEXT(" 300 "), TEXT("cli"), TEXT("abc"));
			TestEqual(TEXT("idle"), Env.IdleSeconds, 300);
			TestTrue(TEXT("cli-owned"), Env.IsCliOwned());
		});

		It("turns the idle shutdown off for a malformed value", [this]()
		{
			for (const TCHAR* Raw : { TEXT(""), TEXT("0"), TEXT("-5"), TEXT("300s"), TEXT("1e3"), TEXT("9999999999") })
				TestEqual(FString::Printf(TEXT("idle for '%s'"), Raw), FUnrealMcpSessionEnv::Parse(Raw, TEXT(""), TEXT("")).IdleSeconds, 0);
		});

		It("is not CLI-owned without the cli owner or a nonce", [this]()
		{
			TestFalse(TEXT("user"), FUnrealMcpSessionEnv::Parse(TEXT("300"), TEXT("user"), TEXT("abc")).IsCliOwned());
			TestFalse(TEXT("no nonce"), FUnrealMcpSessionEnv::Parse(TEXT("300"), TEXT("cli"), TEXT("")).IsCliOwned());
		});
	});

	Describe("idle timer", [this]()
	{
		It("quits after the timeout without activity", [this]()
		{
			FUnrealMcpIdleTimer Timer(10.0, 0.0);
			TestFalse(TEXT("at 5s"), Timer.ShouldQuit(1.0, 0, false) || Timer.ShouldQuit(5.0, 0, false));
			for (double T = 6.0; T < 10.0; T += 1.0)
				Timer.ShouldQuit(T, 0, false);
			TestTrue(TEXT("at 10s"), Timer.ShouldQuit(10.0, 0, false));
		});

		It("restarts on tool activity, while busy and after a long stall", [this]()
		{
			FUnrealMcpIdleTimer Timer(10.0, 0.0);
			for (double T = 1.0; T <= 9.0; T += 1.0)
				Timer.ShouldQuit(T, 0, false);
			TestFalse(TEXT("call at 9s"), Timer.ShouldQuit(9.5, 1, false));
			for (double T = 10.0; T <= 19.0; T += 1.0)
				Timer.ShouldQuit(T, 1, T < 12.0); // busy until 11s
			TestFalse(TEXT("busy restarted the clock"), Timer.ShouldQuit(20.0, 1, false));
			TestFalse(TEXT("a 70s stall is activity"), Timer.ShouldQuit(90.0, 1, false));
			for (double T = 91.0; T < 100.0; T += 1.0)
				Timer.ShouldQuit(T, 1, false);
			TestTrue(TEXT("10s after the stall"), Timer.ShouldQuit(100.0, 1, false));
			TestTrue(TEXT("an answered call is activity too"), !Timer.ShouldQuit(101.0, 2, false));
		});

		It("counts user input as activity", [this]()
		{
			FUnrealMcpIdleTimer Timer(10.0, 0.0);
			for (double T = 1.0; T < 10.0; T += 1.0)
				Timer.ShouldQuit(T, 0, false, /*LastUserInput*/ 8.0);
			TestFalse(TEXT("input at 8s"), Timer.ShouldQuit(10.0, 0, false, 8.0));
			for (double T = 11.0; T < 18.0; T += 1.0)
				Timer.ShouldQuit(T, 0, false, 8.0);
			TestTrue(TEXT("10s after the input"), Timer.ShouldQuit(18.0, 0, false, 8.0));
		});
	});

	Describe("lease", [this]()
	{
		It("removes the lease that carries our nonce", [this]()
		{
			const FString Path = SessionSpecWriteLease(TEXT("ours"));
			TestTrue(TEXT("removed"), FUnrealMcpEditorSession::RemoveLeaseIfOwned(Path, TEXT("ours")));
			TestFalse(TEXT("gone"), FPaths::FileExists(Path));
		});

		It("keeps another session's lease", [this]()
		{
			const FString Path = SessionSpecWriteLease(TEXT("newer"));
			TestFalse(TEXT("not removed"), FUnrealMcpEditorSession::RemoveLeaseIfOwned(Path, TEXT("ours")));
			TestTrue(TEXT("still there"), FPaths::FileExists(Path));
		});

		It("keeps a lease it cannot parse and ignores a missing one", [this]()
		{
			const FString Path = FUnrealMcpEditorSession::LeasePath(SessionSpecTempDir);
			FFileHelper::SaveStringToFile(TEXT("not json"), *Path);
			TestFalse(TEXT("unparseable"), FUnrealMcpEditorSession::RemoveLeaseIfOwned(Path, TEXT("ours")));
			IFileManager::Get().Delete(*Path);
			TestFalse(TEXT("missing"), FUnrealMcpEditorSession::RemoveLeaseIfOwned(Path, TEXT("ours")));
		});

		It("removes the lease on Shutdown only for a CLI-owned editor", [this]()
		{
			const FString Path = SessionSpecWriteLease(TEXT("ours"));
			MakeShared<FUnrealMcpEditorSession>(FUnrealMcpSessionEnv::Parse(TEXT(""), TEXT("user"), TEXT("ours")), SessionSpecTempDir)->Shutdown();
			TestTrue(TEXT("kept for a user editor"), FPaths::FileExists(Path));
			MakeShared<FUnrealMcpEditorSession>(FUnrealMcpSessionEnv::Parse(TEXT(""), TEXT("cli"), TEXT("ours")), SessionSpecTempDir)->Shutdown();
			TestFalse(TEXT("removed for ours"), FPaths::FileExists(Path));
		});
	});

	Describe("savable packages", [this]()
	{
		It("saves packages with a path, never temp or memory ones", [this]()
		{
			TestTrue(TEXT("/Game"), FUnrealMcpEditorSession::HasSavablePath(TEXT("/Game/Maps/Main")));
			TestFalse(TEXT("untitled map"), FUnrealMcpEditorSession::HasSavablePath(TEXT("/Temp/Untitled_1")));
			TestFalse(TEXT("memory"), FUnrealMcpEditorSession::HasSavablePath(TEXT("/Memory/Foo")));
			TestFalse(TEXT("transient"), FUnrealMcpEditorSession::HasSavablePath(TEXT("/Engine/Transient")));
			TestFalse(TEXT("not a package name"), FUnrealMcpEditorSession::HasSavablePath(TEXT("C:/x.uasset")));
		});
	});

	Describe("editor-quit", [this]()
	{
		It("is a system tool", [this]()
		{
			const TSharedRef<FUnrealMcpEditorSession> Session = MakeShared<FUnrealMcpEditorSession>(FUnrealMcpSessionEnv(), SessionSpecTempDir);
			FUnrealMcpToolRegistry Registry;
			Session->RegisterTools(Registry);
			const FUnrealMcpRegisteredTool* Tool = Registry.Find(FUnrealMcpEditorSession::QuitToolName);
			if (TestNotNull(TEXT("registered"), Tool))
				TestTrue(TEXT("system"), Tool->ToolType == EUnrealMcpToolType::System);
		});

		It("refuses an editor no CLI started", [this]()
		{
			const FUnrealMcpToolResult Result = SessionSpecQuit(FUnrealMcpSessionEnv(), TEXT(""));
			TestFalse(TEXT("refused"), Result.bSuccess);
			TestTrue(TEXT("NotOwned"), Result.Message.StartsWith(TEXT("NotOwned")));
		});

		It("refuses a wrong nonce", [this]()
		{
			const FUnrealMcpToolResult Result = SessionSpecQuit(FUnrealMcpSessionEnv::Parse(TEXT(""), TEXT("cli"), TEXT("ours")), TEXT("theirs"));
			TestFalse(TEXT("refused"), Result.bSuccess);
			TestTrue(TEXT("NotOwned"), Result.Message.StartsWith(TEXT("NotOwned")));
		});

		It("refuses once the session is disarmed (the editor is shutting down)", [this]()
		{
			const FUnrealMcpToolResult Result = SessionSpecQuit(FUnrealMcpSessionEnv::Parse(TEXT(""), TEXT("cli"), TEXT("ours")), TEXT("ours"));
			TestFalse(TEXT("refused"), Result.bSuccess);
			TestTrue(TEXT("shutting down"), Result.Message.Contains(TEXT("shutting down")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
