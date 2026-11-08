// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "UnrealMcpRqlSpecHelpers.h"
#include "UnrealMcpRqlPlay.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"

/**
 * Play testing (docs/rql.md "Play testing"): editor | call StartPlay / AdvancePlay / StopPlay and editor.PlayState. The
 * session starts inside the call, each advance runs fixed 1/60 s frames inside the call (so the result does not depend on
 * the editor ticking between calls), and held keys reach the player through its input like real ones.
 *
 * The open editor level is played with the engine's GameModeBase from an added PlayerStart (both undone after), so the
 * player is a DefaultPawn that flies with W; the Enhanced Input extension's spec checks that a mapped key fires its action.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpRqlPlaySpec, "UnrealMcp.Tools.Rql.Play",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	TSharedPtr<FJsonObject> PlayCall(const FString& Method, const FString& Args = FString())
	{
		const FString Plan = FString::Printf(TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"call","method":"%s","args":[%s]}]})RQL"),
			*Method, *Args);
		return UnrealMcpRqlSpecHelpers::Exec(*this, Plan, /*bMutate*/ true);
	}

	/** What PreparePlayWorld changed in the editor world (later specs depend on that world): put back by RestoreWorld. */
	TWeakObjectPtr<AWorldSettings> PreparedSettings;
	TSubclassOf<AGameModeBase> PreviousGameMode;
	TWeakObjectPtr<APlayerStart> AddedStart;
	bool bLevelWasDirty = false;

	/**
	 * Play the open editor world with the engine's GameModeBase (a DefaultPawn that flies with W / S / A / D) from an added
	 * PlayerStart; RestoreWorld undoes both. The level itself is kept: specs after this one use it.
	 */
	bool PreparePlayWorld()
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
		if (!TestNotNull(TEXT("editor world settings"), Settings))
			return false;
		PreparedSettings = Settings;
		bLevelWasDirty = World->GetOutermost()->IsDirty();
		PreviousGameMode = Settings->DefaultGameMode;
		Settings->DefaultGameMode = AGameModeBase::StaticClass();
		AddedStart = World->SpawnActor<APlayerStart>(FVector(0.0, 0.0, 5000.0), FRotator::ZeroRotator);
		return true;
	}

	void EndPlay()
	{
		if (GEditor && GEditor->PlayWorld)
			GEditor->EndPlayMap();
	}

	void RestoreWorld()
	{
		EndPlay();
		if (APlayerStart* Start = AddedStart.Get())
			Start->GetWorld()->EditorDestroyActor(Start, /*bShouldModifyLevel*/ true);
		if (AWorldSettings* Settings = PreparedSettings.Get())
		{
			Settings->DefaultGameMode = PreviousGameMode;
			Settings->GetOutermost()->SetDirtyFlag(bLevelWasDirty); // unchanged, as later specs found it
		}
		PreparedSettings.Reset();
		AddedStart.Reset();
	}

END_DEFINE_SPEC(FUnrealMcpRqlPlaySpec)

void FUnrealMcpRqlPlaySpec::Define()
{
	using namespace UnrealMcpRqlSpecHelpers;

	Describe("frame count", [this]()
	{
		It("turns seconds into 60 Hz frames and refuses what one call cannot run", [this]()
		{
			FString Error;
			TestEqual(TEXT("0.5 s = 30 frames"), UnrealMcpRql::PlayFrameCount(0.5, 0, Error), 30);
			TestEqual(TEXT("1/60 s rounds to one frame"), UnrealMcpRql::PlayFrameCount(1.0 / 60.0, 0, Error), 1);
			TestEqual(TEXT("frames given"), UnrealMcpRql::PlayFrameCount(0.0, 7, Error), 7);
			TestEqual(TEXT("both: the larger"), UnrealMcpRql::PlayFrameCount(0.1, 10, Error), 10);
			TestEqual(TEXT("neither: one frame"), UnrealMcpRql::PlayFrameCount(0.0, 0, Error), 1);
			TestEqual(TEXT("over 60 s refused"), UnrealMcpRql::PlayFrameCount(61.0, 0, Error), -1);
			TestTrue(*FString::Printf(TEXT("says to split (%s)"), *Error), Error.Contains(TEXT("several calls")));
			TestEqual(TEXT("negative refused"), UnrealMcpRql::PlayFrameCount(-1.0, 0, Error), -1);
		});
	});

	Describe("without a session", [this]()
	{
		It("AdvancePlay fails and names StartPlay; PlayState reads Playing = false", [this]()
		{
			EndPlay();
			const TSharedPtr<FJsonObject> Advance = PlayCall(TEXT("AdvancePlay"), TEXT(R"({"lit":1},{"lit":0},{"lit":""})"));
			TestFalse(TEXT("refused"), IsOk(Advance));
			TestTrue(*FString::Printf(TEXT("names StartPlay (%s)"), *Codes(Advance)), Codes(Advance).Contains(TEXT("StartPlay()")));
			TestFalse(TEXT("not playing"), UnrealMcpRql::ReadPlayState().Playing);
			const TSharedPtr<FJsonObject> Stop = PlayCall(TEXT("StopPlay"));
			TestFalse(TEXT("StopPlay without a session is refused"), IsOk(Stop));
		});

		It("refuses a key name that is not a key before simulating", [this]()
		{
			FString Error;
			int32 Advanced = 0;
			TestFalse(TEXT("refused"), UnrealMcpRql::AdvancePlayWithKeys(0.0, 1, TEXT("W, NotAKey"), Advanced, Error));
			TestTrue(*FString::Printf(TEXT("names the key (%s)"), *Error), Error.Contains(TEXT("'NotAKey' is not a key name")));
			TestFalse(TEXT("a 2D axis is refused"), UnrealMcpRql::AdvancePlayWithKeys(0.0, 1, TEXT("Gamepad_Left2D"), Advanced, Error));
			TestEqual(TEXT("nothing simulated"), Advanced, 0);
		});
	});

	Describe("a session", [this]()
	{
		AfterEach([this]()
		{
			RestoreWorld();
		});

		It("starts inside the call, advances exact frames while paused, moves the pawn with held keys, and stops", [this]()
		{
			EndPlay();
			if (!PreparePlayWorld())
				return;

			const TSharedPtr<FJsonObject> Start = PlayCall(TEXT("StartPlay"));
			if (!TestTrue(*FString::Printf(TEXT("StartPlay ok (%s)"), *Codes(Start)), IsOk(Start)))
				return;
			UWorld* PlayWorld = UnrealMcpRql::PlayWorld();
			if (!TestNotNull(TEXT("the session runs when the call returns"), PlayWorld))
				return;
			FUnrealMcpPlayState State = UnrealMcpRql::ReadPlayState();
			TestTrue(TEXT("paused between calls by default"), State.Playing && State.Paused);
			if (!TestNotNull(TEXT("player 0 possesses a pawn"), State.Pawn.Get()))
				return;
			TestFalse(TEXT("no RQL transaction left open"), GEditor->IsTransactionActive());

			// 30 frames of nothing: game time moves by exactly 0.5 s and the session stays paused.
			const double Before = State.Time;
			const TSharedPtr<FJsonObject> Wait = PlayCall(TEXT("AdvancePlay"), TEXT(R"({"lit":0.5},{"lit":0},{"lit":""})"));
			TestTrue(*FString::Printf(TEXT("AdvancePlay ok (%s)"), *Codes(Wait)), IsOk(Wait));
			State = UnrealMcpRql::ReadPlayState();
			TestEqual(TEXT("0.5 s of game time"), State.Time - Before, 0.5, 1e-3);
			TestTrue(TEXT("still paused"), State.Paused);

			const FVector Start0 = State.Location;

			// Held W reaches player 0's input as a real key press: pressed on the first frame, released on one extra frame.
			int32 Advanced = 0;
			FString Error;
			TestTrue(*FString::Printf(TEXT("held keys (%s)"), *Error), UnrealMcpRql::AdvancePlayWithKeys(0.5, 0, TEXT("W"), Advanced, Error));
			TestEqual(TEXT("30 held + 1 released frames"), Advanced, 31);
			APlayerController* Controller = UnrealMcpRql::PlayController();
			TestTrue(TEXT("W was pressed and released in the player input"), Controller && Controller->PlayerInput && Controller->PlayerInput->WasJustReleased(FKey(FName(TEXT("W")))));
			const FVector End0 = UnrealMcpRql::ReadPlayState().Location;
			TestTrue(*FString::Printf(TEXT("the DefaultPawn flew while W was held (%s -> %s)"), *Start0.ToString(), *End0.ToString()),
				FVector::Dist(Start0, End0) > 50.0);

			// The receipt carries the play state the call returned.
			const TSharedPtr<FJsonObject> Advance = PlayCall(TEXT("AdvancePlay"), TEXT(R"({"lit":0},{"lit":2},{"lit":""})"));
			FString Json;
			FJsonSerializer::Serialize(Advance.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
			TestTrue(*FString::Printf(TEXT("the state is in the receipt (%s)"), *Json.Left(400)), Json.Contains(TEXT("PawnClass")) && Json.Contains(TEXT("FramesAdvanced")));

			const TSharedPtr<FJsonObject> Stop = PlayCall(TEXT("StopPlay"));
			TestTrue(*FString::Printf(TEXT("StopPlay ok (%s)"), *Codes(Stop)), IsOk(Stop));
			TestNull(TEXT("stopped when the call returns"), UnrealMcpRql::PlayWorld());
		});

		It("moves the pawn each held key's own way, in sequence in one session and after re-possession, both ways for keys held together, and releases them", [this]()
		{
			EndPlay();
			if (!PreparePlayWorld())
				return;
			FString Error;
			if (!TestTrue(*FString::Printf(TEXT("started (%s)"), *Error), UnrealMcpRql::StartPlay(/*bPaused*/ true, Error)))
				return;
			APlayerController* Controller = UnrealMcpRql::PlayController();
			const APawn* Possessed = Controller ? Controller->GetPawn() : nullptr;
			if (!TestNotNull(TEXT("player 0 possesses a pawn"), Possessed))
				return;

			// The DefaultPawn flies with W / S forward and back and D / A right and left of the control rotation (the player
			// start's or the possessed pawn's): Local is in that frame.
			auto Fly = [this, Controller](const TCHAR* Keys, const FVector& Local, const TCHAR* When)
			{
				int32 Advanced = 0;
				FString HoldError;
				const FVector Direction = Controller->GetControlRotation().RotateVector(Local);
				const FVector Before = UnrealMcpRql::ReadPlayState().Location;
				TestTrue(*FString::Printf(TEXT("%s: held %s (%s)"), When, Keys, *HoldError), UnrealMcpRql::AdvancePlayWithKeys(0.25, 0, Keys, Advanced, HoldError));
				const FVector Moved = UnrealMcpRql::ReadPlayState().Location - Before;
				TestTrue(*FString::Printf(TEXT("%s: %s flew the pawn along %s (moved %s)"), When, Keys, *Direction.ToString(), *Moved.ToString()),
					Moved.Size() > 20.0 && Moved.GetSafeNormal().Dot(Direction.GetSafeNormal()) > 0.95);
				TArray<FString> Names;
				FString(Keys).ParseIntoArray(Names, TEXT(","), /*bCullEmpty*/ true);
				for (const FString& Name : Names)
					TestFalse(*FString::Printf(TEXT("%s: %s released after the call"), When, *Name), Controller->PlayerInput->IsPressed(FKey(*Name.TrimStartAndEnd())));
				// Coast to a stop: the next key starts from rest.
				UnrealMcpRql::AdvancePlayWithKeys(1.0, 0, TEXT(""), Advanced, HoldError);
			};
			auto Directions = [&Fly](const TCHAR* When)
			{
				Fly(TEXT("W"), FVector(1, 0, 0), When);
				Fly(TEXT("D"), FVector(0, 1, 0), When);
				Fly(TEXT("A"), FVector(0, -1, 0), When);
				Fly(TEXT("S"), FVector(-1, 0, 0), When);
				Fly(TEXT("W, D"), FVector(1, 1, 0), When);
			};
			Directions(TEXT("first pawn"));

			// Re-possession (entering and leaving a vehicle): another pawn, then the first again, the same controller.
			APawn* First = Controller->GetPawn();
			APawn* Second = UnrealMcpRql::PlayWorld()->SpawnActor<APawn>(First->GetClass(), First->GetActorLocation() + FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator);
			if (!TestNotNull(TEXT("a second pawn"), Second))
				return;
			Controller->Possess(Second);
			Directions(TEXT("second pawn"));
			Controller->Possess(First);
			Directions(TEXT("first pawn again"));
		});

		It("lists the play world's actors in playActors and reads PlayState in a query", [this]()
		{
			EndPlay();
			if (!PreparePlayWorld())
				return;
			FString Error;
			if (!TestTrue(*FString::Printf(TEXT("started (%s)"), *Error), UnrealMcpRql::StartPlay(/*bPaused*/ true, Error)))
				return;
			// playActors | where $ is DefaultPawn | count
			const TSharedPtr<FJsonObject> Starts = Exec(*this, TEXT(R"RQL({"rql":"0.1","source":{"path":["playActors"]},"stages":[{"op":"where","pred":{"op":"is","args":[{"path":[]},{"lit":"DefaultPawn"}]}},{"op":"count"}]})RQL"));
			TestEqual(*FString::Printf(TEXT("the player's DefaultPawn is in the play world (%s)"), *Codes(Starts)), Number(Starts), 1.0);
			// editor | select PlayState.Playing
			const TSharedPtr<FJsonObject> Playing = Exec(*this, TEXT(R"RQL({"rql":"0.1","source":{"path":["editor"]},"stages":[{"op":"select","fields":[{"name":"Playing","expr":{"path":["PlayState","Playing"]}}]}]})RQL"));
			TestEqual(*FString::Printf(TEXT("PlayState.Playing (%s)"), *Codes(Playing)), CellText(Playing), FString(TEXT("true")));
			EndPlay();
			const TSharedPtr<FJsonObject> Empty = Exec(*this, TEXT(R"RQL({"rql":"0.1","source":{"path":["playActors"]},"stages":[{"op":"count"}]})RQL"));
			TestEqual(TEXT("no play world: empty"), Number(Empty), 0.0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
