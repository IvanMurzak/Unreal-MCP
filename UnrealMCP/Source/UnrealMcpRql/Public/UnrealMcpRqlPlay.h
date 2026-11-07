// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Templates/Function.h"

#include "UnrealMcpRqlPlay.generated.h"

class APawn;
class APlayerController;
class UWorld;

/**
 * What a play test reads back from the running Play In Editor session: the player's pawn and its movement. The `editor`
 * operations StartPlay / AdvancePlay return it, `editor.PlayState` reads it, and extensions return it from their own play
 * operations (the Enhanced Input extension's InjectInput) after adding to it (UnrealMcpRql::OnReadPlayState).
 */
USTRUCT(BlueprintType)
struct UNREALMCPRQL_API FUnrealMcpPlayState
{
	GENERATED_BODY()

	/** a Play In Editor session is running */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	bool Playing = false;

	/** the session is paused: its world only moves inside AdvancePlay (and other play operations) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	bool Paused = false;

	/** seconds of game time since play started */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	double Time = 0.0;

	/** frames the call advanced (each a fixed 1/60 s step) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	int32 FramesAdvanced = 0;

	/** the pawn player 0 possesses (null when it possesses none) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	TObjectPtr<APawn> Pawn = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	FString PawnClass;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	FRotator Rotation = FRotator::ZeroRotator;

	/** the player controller's aim (camera) rotation */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	FRotator ControlRotation = FRotator::ZeroRotator;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	FVector Velocity = FVector::ZeroVector;

	/** the velocity's length, cm/s */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	double Speed = 0.0;

	/** a Character's movement mode (Walking, Falling, Swimming, Flying, ...); empty for other pawns */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	FString MovementMode;

	/** a Character is crouched */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	bool Crouching = false;

	/** a Character's maximum walk speed now (a sprint raises it), cm/s */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	double MaxWalkSpeed = 0.0;

	/** the Enhanced Input mapping contexts applied to player 0 (filled by the Enhanced Input extension when installed) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	TArray<FString> MappingContexts;

	/** what the call observed besides the state (an input action's trigger events, Blueprints that failed to compile, ...) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Play")
	TArray<FString> Notes;
};

/** Lets an extension add to every play state read (player 0's controller; null when there is none). */
DECLARE_MULTICAST_DELEGATE_TwoParams(FUnrealMcpOnReadPlayState, APlayerController* /*Controller*/, FUnrealMcpPlayState& /*State*/);

/**
 * Deterministic play testing for agents (docs/rql.md "Play testing"). Between tool calls an MCP client cannot rely on the
 * editor ticking PIE (a background editor is throttled, a one-frame input injection is consumed or dropped), so play
 * operations advance the PIE world themselves, inside the call, in fixed 1/60 s steps, and read the result back before
 * returning. Game thread only.
 */
namespace UnrealMcpRql
{
	/** Fixed simulation step of the play operations: 60 frames per second. */
	inline constexpr int32 PlayFrameRate = 60;

	/** The most frames one call advances (60 s of game time). */
	inline constexpr int32 MaxPlayFrames = 3600;

	/** The running Play In Editor world; nullptr when no session runs (or its end was requested). */
	UNREALMCPRQL_API UWorld* PlayWorld();

	/** Player 0's controller in the play world; nullptr when there is none. */
	UNREALMCPRQL_API APlayerController* PlayController();

	/**
	 * Start Play In Editor now (inside this call, not on the next editor tick) with the editor's play settings; keeps a
	 * running session. bPaused pauses it so the world moves only inside play operations. False + the reason when it did
	 * not start. Blueprints that failed to compile are listed in @p OutNotes (no dialog is shown).
	 */
	UNREALMCPRQL_API bool StartPlay(bool bPaused, FString& OutError, TArray<FString>* OutNotes = nullptr);

	/** End the running session now; false + the reason when none runs. */
	UNREALMCPRQL_API bool StopPlay(FString& OutError);

	/**
	 * How many frames @p Seconds and / or @p Frames ask for (seconds at PlayFrameRate, rounded up; both given: the larger;
	 * neither: 1). -1 + the reason when negative or above MaxPlayFrames.
	 */
	UNREALMCPRQL_API int32 PlayFrameCount(double Seconds, int32 Frames, FString& OutError);

	/**
	 * Advance the running session by @p Frames fixed steps of 1 / PlayFrameRate s inside this call (a paused session is
	 * stepped as the toolbar's Advance Frame does and stays paused). @p BeforeFrame runs before each frame's tick with
	 * player 0's controller (it may be null) and the frame index: inject input there. False + the reason when no session
	 * runs or it ended part-way (@p OutAdvanced: the frames that ran).
	 */
	UNREALMCPRQL_API bool AdvancePlay(int32 Frames, TFunctionRef<void(APlayerController*, int32)> BeforeFrame, int32& OutAdvanced,
		FString& OutError);

	/** The session's state now (Playing = false when none runs), with what OnReadPlayState adds. */
	UNREALMCPRQL_API FUnrealMcpPlayState ReadPlayState();

	/** Bound by extensions to add to ReadPlayState (remove the binding when the module shuts down). */
	UNREALMCPRQL_API FUnrealMcpOnReadPlayState& OnReadPlayState();

	/**
	 * AdvancePlay for PlayFrameCount(@p Seconds, @p Frames) frames while holding @p Keys ("W, LeftShift", axes with a
	 * value: "Gamepad_LeftY=1") as player 0's input, released on one extra frame at the end (counted in @p OutAdvanced).
	 */
	UNREALMCPRQL_API bool AdvancePlayWithKeys(double Seconds, int32 Frames, const FString& Keys, int32& OutAdvanced, FString& OutError);
}
