// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlPlay.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "Misc/EngineVersionComparison.h"
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
#include "InputKeyEventArgs.h"
#endif
#include "Misc/ITransaction.h"
#include "Misc/PackageName.h"
#include "PlayInEditorDataTypes.h"
#include "Tickable.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace UnrealMcpRqlPlayLocal
{
	const TCHAR* const NoSessionError = TEXT("no Play In Editor session is running: editor | call StartPlay() first");

	/** One call may simulate this long (wall clock) before it stops with what it has: a tool call has a timeout. */
	constexpr double MaxAdvanceWallSeconds = 25.0;

	/**
	 * Finish the open editor transaction (the RQL call's own) before PIE starts or ends: PIE start cancels it (with a
	 * warning toast) and PIE end may reset the undo buffer, which an open transaction turns into an error. It records
	 * nothing a play session could undo; the writes before the call in the same query keep their undo step.
	 */
	void CloseOpenTransactions()
	{
		for (int32 Guard = 0; Guard < 8 && GEditor->IsTransactionActive(); ++Guard)
			GEditor->EndTransaction();
	}

	/** Loaded Blueprints that failed to compile, as PIE's own check lists them (it would show a modal dialog). */
	TArray<FString> ErroredBlueprints()
	{
		TArray<FString> Names;
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			if (It->Status == BS_Error && It->bDisplayCompilePIEWarning && !It->HasAnyFlags(RF_ClassDefaultObject | RF_Transient))
				Names.Add(It->GetPathName());
		}
		return Names;
	}
}

UWorld* UnrealMcpRql::PlayWorld()
{
	return GEditor && GEditor->PlayWorld && !GEditor->ShouldEndPlayMap() ? GEditor->PlayWorld.Get() : nullptr;
}

APlayerController* UnrealMcpRql::PlayController()
{
	UWorld* World = PlayWorld();
	return World ? World->GetFirstPlayerController() : nullptr;
}

APlayerController* UnrealMcpRql::RequirePlayController(FString& OutError)
{
	if (!PlayWorld())
	{
		OutError = UnrealMcpRqlPlayLocal::NoSessionError;
		return nullptr;
	}
	APlayerController* Controller = PlayController();
	if (!Controller)
		OutError = TEXT("the play world has no player controller to send input to");
	return Controller;
}

bool UnrealMcpRql::StartPlay(bool bPaused, FString& OutError, TArray<FString>* OutNotes)
{
	if (!GEditor)
	{
		OutError = TEXT("no editor");
		return false;
	}
	if (!PlayWorld())
	{
		UnrealMcpRqlPlayLocal::CloseOpenTransactions();
		// A stop that was requested but not yet run (set IsPlaying = false): finish it, then start a new session.
		if (GEditor->PlayWorld)
			GEditor->EndPlayMap();
		// Unattended: PIE's "Blueprint Compilation Errors" dialog (a modal on the game thread) is skipped and play goes on;
		// the errored Blueprints are reported instead.
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->RequestPlaySession(FRequestPlaySessionParams());
		GEditor->StartQueuedPlaySessionRequest();
		if (!PlayWorld())
		{
			OutError = TEXT("Play In Editor did not start (the console source has the reason: console | where Verbosity == \"Error\" | sort Sequence desc | first 5)");
			return false;
		}
		if (OutNotes)
		{
			for (const FString& Blueprint : UnrealMcpRqlPlayLocal::ErroredBlueprints())
				OutNotes->Add(FString::Printf(TEXT("Blueprint %s has compile errors (it plays as last compiled): unreal.asset(\"%s\") | call Compile()"),
					*Blueprint, *FPackageName::ObjectPathToPackageName(Blueprint)));
		}
	}
	GEditor->SetPIEWorldsPaused(bPaused);
	return true;
}

bool UnrealMcpRql::StopPlay(FString& OutError)
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		OutError = TEXT("no Play In Editor session is running");
		return false;
	}
	UnrealMcpRqlPlayLocal::CloseOpenTransactions();
	GEditor->EndPlayMap();
	return true;
}

int32 UnrealMcpRql::PlayFrameCount(double Seconds, int32 Frames, FString& OutError)
{
	if (Seconds < 0.0 || Frames < 0)
	{
		OutError = TEXT("Seconds and Frames cannot be negative");
		return -1;
	}
	// Rounded up, less a hair so 0.5 s is exactly 30 frames despite floating point.
	const double FromSeconds = FMath::CeilToDouble(Seconds * PlayFrameRate - 1e-6);
	const double Count = FMath::Max3(FromSeconds, static_cast<double>(Frames), 0.0);
	if (Count > MaxPlayFrames)
	{
		OutError = FString::Printf(TEXT("at most %d frames (%d s) per call: advance in several calls"), MaxPlayFrames, MaxPlayFrames / PlayFrameRate);
		return -1;
	}
	return Count > 0.0 ? static_cast<int32>(Count) : 1;
}

bool UnrealMcpRql::AdvancePlay(int32 Frames, TFunctionRef<void(APlayerController*, int32)> BeforeFrame, int32& OutAdvanced, FString& OutError)
{
	OutAdvanced = 0;
	UWorld* World = PlayWorld();
	if (!World)
	{
		OutError = UnrealMcpRqlPlayLocal::NoSessionError;
		return false;
	}
	// Gameplay that calls Modify() would otherwise record PIE objects into the RQL call's transaction (the editor then
	// discards the whole undo buffer when PIE ends).
	TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
	TWeakObjectPtr<UWorld> WeakWorld(World);
	const bool bWasPaused = World->bDebugPauseExecution;
	const float Delta = 1.f / PlayFrameRate;
	const double Deadline = FPlatformTime::Seconds() + UnrealMcpRqlPlayLocal::MaxAdvanceWallSeconds;
	bool bOk = true;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		if (!WeakWorld.IsValid() || PlayWorld() != World)
		{
			OutError = FString::Printf(TEXT("the play session ended after %d of %d frame(s) (the game quit or travelled)"), OutAdvanced, Frames);
			bOk = false;
			break;
		}
		if (FPlatformTime::Seconds() > Deadline)
		{
			OutError = FString::Printf(TEXT("stopped after %d of %d frame(s): the simulation took longer than %.0f s; advance fewer frames per call"),
				OutAdvanced, Frames, UnrealMcpRqlPlayLocal::MaxAdvanceWallSeconds);
			bOk = false;
			break;
		}
		// As UEditorEngine::Tick ticks a PIE world: inside its world context (GWorld, the PIE instance id), unpaused for
		// the step as the toolbar's Advance Frame does.
		// Each simulated frame is a new engine frame: the tick manager runs an actor's tick function at most once per
		// GFrameCounter, so without this only the first frame would tick the player controller, pawns and components.
		++GFrameCounter;
		UWorld* EditorGWorld = SetPlayInEditorWorld(World);
		World->bDebugPauseExecution = false;
		BeforeFrame(World->GetFirstPlayerController(), Frame);
		World->Tick(LEVELTICK_All, Delta);
		// Paused during the frame (a Blueprint breakpoint, the pause command): it stays paused and the call stops there.
		const bool bPausedByGame = WeakWorld.IsValid() && World->bDebugPauseExecution;
		if (WeakWorld.IsValid() && !bPausedByGame)
			World->bDebugPauseExecution = bWasPaused;
		RestoreEditorWorld(EditorGWorld);
		// Then, as the editor does after the PIE world, the tickable objects without a world: Enhanced Input rebuilds a
		// player's key mappings there (a context added at BeginPlay maps its keys from the next frame on).
		FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, /*bIsPaused*/ false, Delta);
		++OutAdvanced;
		if (bPausedByGame && Frame + 1 < Frames)
		{
			OutError = FString::Printf(TEXT("the game paused itself (a breakpoint or the pause command) after %d of %d frame(s)"), OutAdvanced, Frames);
			bOk = false;
			break;
		}
	}
	return bOk;
}

FUnrealMcpPlayState UnrealMcpRql::ReadPlayState()
{
	FUnrealMcpPlayState State;
	UWorld* World = PlayWorld();
	if (!World)
		return State;
	State.Playing = true;
	State.Paused = World->bDebugPauseExecution;
	State.Time = World->GetTimeSeconds();
	APlayerController* Controller = World->GetFirstPlayerController();
	if (Controller)
	{
		State.ControlRotation = Controller->GetControlRotation();
		if (APawn* Pawn = Controller->GetPawn())
		{
			State.Pawn = Pawn;
			State.PawnClass = Pawn->GetClass()->GetName();
			State.Location = Pawn->GetActorLocation();
			State.Rotation = Pawn->GetActorRotation();
			State.Velocity = Pawn->GetVelocity();
			State.Speed = State.Velocity.Size();
			if (const ACharacter* Character = Cast<ACharacter>(Pawn))
			{
				if (const UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
				{
					State.MovementMode = StaticEnum<EMovementMode>()->GetNameStringByValue(Movement->MovementMode.GetValue()).Replace(TEXT("MOVE_"), TEXT(""));
					State.MaxWalkSpeed = Movement->MaxWalkSpeed;
				}
				State.Crouching = Character->bIsCrouched;
			}
		}
	}
	State.MappingContexts = AppliedMappingContexts(Controller);
	OnReadPlayState().Broadcast(Controller, State);
	return State;
}

TArray<FString> UnrealMcpRql::AppliedMappingContexts(const APlayerController* Controller)
{
	// UEnhancedPlayerInput's applied contexts: UE 5.6+ AppliedInputContextData (context -> {Priority, ...}), UE 5.5
	// AppliedInputContexts (context -> priority).
	UPlayerInput* Input = Controller ? Controller->PlayerInput.Get() : nullptr;
	const FMapProperty* Map = Input ? FindFProperty<FMapProperty>(Input->GetClass(), TEXT("AppliedInputContextData")) : nullptr;
	if (Input && !Map)
		Map = FindFProperty<FMapProperty>(Input->GetClass(), TEXT("AppliedInputContexts"));
	const FObjectPropertyBase* ContextProperty = Map ? CastField<FObjectPropertyBase>(Map->KeyProp) : nullptr;
	if (!ContextProperty)
		return {};
	const FStructProperty* DataProperty = CastField<FStructProperty>(Map->ValueProp);
	const FIntProperty* PriorityProperty = DataProperty ? FindFProperty<FIntProperty>(DataProperty->Struct, TEXT("Priority"))
		: CastField<FIntProperty>(Map->ValueProp);
	TArray<TPair<int32, FString>> Applied;
	FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Input));
	for (FScriptMapHelper::FIterator It(Helper); It; ++It)
	{
		const UObject* Context = ContextProperty->GetObjectPropertyValue(Helper.GetKeyPtr(It));
		if (!Context)
			continue;
		const void* Value = Helper.GetValuePtr(It);
		const int32 Priority = !PriorityProperty ? 0
			: DataProperty ? PriorityProperty->GetPropertyValue_InContainer(Value) : PriorityProperty->GetPropertyValue(Value);
		Applied.Add({ Priority, Context->GetName() });
	}
	Applied.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key != B.Key ? A.Key > B.Key : A.Value < B.Value; });
	TArray<FString> Names;
	for (const TPair<int32, FString>& Entry : Applied)
		Names.Add(FString::Printf(TEXT("%s (priority %d)"), *Entry.Value, Entry.Key));
	return Names;
}

FUnrealMcpOnReadPlayState& UnrealMcpRql::OnReadPlayState()
{
	static FUnrealMcpOnReadPlayState Delegate;
	return Delegate;
}

namespace UnrealMcpRqlPlayLocal
{
	/** One key AdvancePlay holds: a button (pressed, then released) or a 1D axis fed its value every frame. */
	struct FHeldKey
	{
		FKey Key;
		float Value = 1.f;
		bool bAxis = false;
	};

	/** "W, LeftShift, Gamepad_LeftY=0.5" as held keys; false + the reason for a name that is not a key. */
	bool ParseHeldKeys(const FString& Text, TArray<FHeldKey>& Out, FString& OutError)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), /*bCullEmpty*/ true);
		for (const FString& Part : Parts)
		{
			FString Name = Part.TrimStartAndEnd(), ValueText;
			if (Name.IsEmpty())
				continue;
			FHeldKey Held;
			if (Name.Split(TEXT("="), &Name, &ValueText))
			{
				Name.TrimStartAndEndInline();
				if (!LexTryParseString(Held.Value, *ValueText.TrimStartAndEnd()))
				{
					OutError = FString::Printf(TEXT("'%s': the value after = must be a number (Gamepad_LeftY=1)"), *Part.TrimStartAndEnd());
					return false;
				}
			}
			Held.Key = FKey(*Name);
			if (!Held.Key.IsValid())
			{
				OutError = FString::Printf(TEXT("'%s' is not a key name (W, SpaceBar, LeftShift, E, LeftMouseButton, Gamepad_FaceButton_Bottom, Gamepad_LeftY=1)"), *Name);
				return false;
			}
			if (Held.Key.IsAxis2D() || Held.Key.IsAxis3D())
			{
				OutError = FString::Printf(TEXT("'%s' is a 2D / 3D axis: hold its 1D axes instead (Gamepad_LeftX=0, Gamepad_LeftY=1)"), *Name);
				return false;
			}
			Held.bAxis = Held.Key.IsAxis1D();
			Out.Add(Held);
		}
		return true;
	}

	/**
	 * Send one key event to the controller's player input, as the game viewport does for real input: from the player's
	 * primary input device (as Enhanced Input's own key injection), so input filtered by platform user still reaches it.
	 */
	void SendKey(APlayerController* Controller, const FHeldKey& Held, EInputEvent Event, float Amount)
	{
		const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(Controller->GetPlatformUserId());
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		if (Event == IE_Axis)
			Controller->InputKey(FInputKeyParams(Held.Key, static_cast<double>(Amount), 1.f / UnrealMcpRql::PlayFrameRate, 1, Held.Key.IsGamepadKey(), Device));
		else
			Controller->InputKey(FInputKeyParams(Held.Key, Event, static_cast<double>(Amount), Held.Key.IsGamepadKey(), Device));
#else
		Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Held.Key, Event, Amount, /*InNumSamplesOverride*/ -1, Device));
#endif
	}

	/** "SwizzleAxis, Negate": one Enhanced Input key mapping's modifiers ("none" without). */
	FString ModifierNames(const FArrayProperty* Modifiers, const void* Mapping)
	{
		const FObjectPropertyBase* Element = Modifiers ? CastField<FObjectPropertyBase>(Modifiers->Inner) : nullptr;
		if (!Element)
			return TEXT("none");
		FScriptArrayHelper Array(Modifiers, Modifiers->ContainerPtrToValuePtr<void>(Mapping));
		TArray<FString> Names;
		for (int32 Index = 0; Index < Array.Num(); ++Index)
		{
			if (const UObject* Modifier = Element->GetObjectPropertyValue(Array.GetRawPtr(Index)))
			{
				// As the Enhanced Input extension's short names: InputModifierNegate -> Negate, BP_Mine_C -> BP_Mine.
				FString Name = Modifier->GetClass()->GetName();
				Name.RemoveFromEnd(TEXT("_C"));
				Name.RemoveFromStart(TEXT("InputModifier"));
				Names.Add(Name);
			}
		}
		return Names.Num() ? FString::Join(Names, TEXT(", ")) : TEXT("none");
	}

	/**
	 * A held key that reaches one Enhanced Input action through several of the player's key mappings (read by reflection
	 * from UEnhancedPlayerInput::EnhancedActionMappings) gives that action the combined value of all of them, whichever of
	 * their keys is pressed: one note per key and action.
	 */
	void NoteSharedKeyMappings(const APlayerController* Controller, const TArray<FHeldKey>& Held, TArray<FString>& OutNotes)
	{
		UPlayerInput* Input = Controller ? Controller->PlayerInput.Get() : nullptr;
		const FArrayProperty* Mappings = Input ? FindFProperty<FArrayProperty>(Input->GetClass(), TEXT("EnhancedActionMappings")) : nullptr;
		const FStructProperty* Mapping = Mappings ? CastField<FStructProperty>(Mappings->Inner) : nullptr;
		const FObjectPropertyBase* ActionProperty = Mapping ? FindFProperty<FObjectPropertyBase>(Mapping->Struct, TEXT("Action")) : nullptr;
		const FStructProperty* KeyProperty = Mapping ? FindFProperty<FStructProperty>(Mapping->Struct, TEXT("Key")) : nullptr;
		if (!ActionProperty || !KeyProperty || KeyProperty->Struct != FKey::StaticStruct())
			return;
		const FArrayProperty* ModifiersProperty = FindFProperty<FArrayProperty>(Mapping->Struct, TEXT("Modifiers"));
		// One pass over the mappings: the modifiers of each (held key, action) pair.
		TMap<TPair<FKey, const UObject*>, TArray<FString>> ModifiersByKeyAction;
		FScriptArrayHelper Array(Mappings, Mappings->ContainerPtrToValuePtr<void>(Input));
		for (int32 Index = 0; Index < Array.Num(); ++Index)
		{
			const void* Row = Array.GetRawPtr(Index);
			const FKey& Key = *KeyProperty->ContainerPtrToValuePtr<FKey>(Row);
			const UObject* Action = ActionProperty->GetObjectPropertyValue_InContainer(Row);
			if (Action && Held.ContainsByPredicate([&Key](const FHeldKey& HeldKey) { return HeldKey.Key == Key; }))
				ModifiersByKeyAction.FindOrAdd({ Key, Action }).AddUnique(ModifierNames(ModifiersProperty, Row));
		}
		// Mappings of one key and action that differ only in triggers (Tap and Hold) give the same value: only differing
		// modifiers make the keys of those mappings indistinguishable.
		for (const TPair<TPair<FKey, const UObject*>, TArray<FString>>& Entry : ModifiersByKeyAction)
		{
			if (Entry.Value.Num() > 1)
				OutNotes.Add(FString::Printf(TEXT("%s reaches %s through %d differently modified key mappings (modifiers: %s), so those mappings' keys give it one combined "
					"value, as real presses would; e.g. Enhanced Input user settings put every keyboard key of a player-mappable action on each of its "
					"mappings that share a name: give each mapping its own Player Mappable Key Settings name, or make them not player-mappable."),
					*Entry.Key.Key.ToString(), *Entry.Key.Value->GetName(), Entry.Value.Num(), *FString::Join(Entry.Value, TEXT(" | "))));
		}
	}

	/**
	 * AdvancePlayWithKeys with its keys parsed and its frames counted. @p OutFarthest gets how far the pawn got from where it
	 * started while the keys were held: a jump that lands inside the call moved, though it ends where it began.
	 */
	bool AdvanceHoldingKeys(const TArray<FHeldKey>& Held, int32 Count, int32& OutAdvanced, FString& OutError, TArray<FString>* OutNotes,
		double* OutFarthest = nullptr)
	{
		using namespace UnrealMcpRql;
		OutAdvanced = 0;
		if (Held.Num() == 0)
			return AdvancePlay(Count, [](APlayerController*, int32) {}, OutAdvanced, OutError);
		if (!RequirePlayController(OutError))
			return false;
		// Pressed on the first frame and held (buttons stay down until released; axes are fed every frame), released on one
		// extra frame so key-up handlers and Completed events run. Pressed again on a controller that replaced the first one
		// mid-call (the game swapped it, e.g. after travel), and released on the one that holds the keys.
		TWeakObjectPtr<APlayerController> Pressed;
		TOptional<FVector> PawnStart;
		const bool bHeld = AdvancePlay(Count, [&Held, &Pressed, &PawnStart, OutFarthest](APlayerController* Controller, int32 Frame)
		{
			if (!Controller)
				return;
			if (const APawn* Pawn = OutFarthest ? Controller->GetPawn() : nullptr)
			{
				const FVector Location = Pawn->GetActorLocation();
				*OutFarthest = FMath::Max(*OutFarthest, FVector::Dist(PawnStart.Get(Location), Location));
				if (!PawnStart.IsSet())
					PawnStart = Location;
			}
			const bool bFirst = !Pressed.IsValid() || Pressed.Get() != Controller;
			Pressed = Controller;
			for (const FHeldKey& Key : Held)
			{
				if (Key.bAxis)
					SendKey(Controller, Key, IE_Axis, Key.Value);
				else if (bFirst)
					SendKey(Controller, Key, IE_Pressed, Key.Value);
			}
		}, OutAdvanced, OutError);
		auto Release = [&Held, &Pressed](APlayerController*, int32)
		{
			if (APlayerController* Target = Pressed.Get())
			{
				for (const FHeldKey& Key : Held)
					SendKey(Target, Key, Key.bAxis ? IE_Axis : IE_Released, 0.f);
			}
		};
		if (!bHeld)
		{
			// Stopped part-way (the deadline, a pause): never leave the keys held for the frames after this call.
			Release(nullptr, 0);
			return false;
		}
		if (OutNotes)
			NoteSharedKeyMappings(Pressed.Get(), Held, *OutNotes);
		int32 ReleaseFrames = 0;
		const bool bReleased = AdvancePlay(1, Release, ReleaseFrames, OutError);
		OutAdvanced += ReleaseFrames;
		return bReleased;
	}
}

bool UnrealMcpRql::AdvancePlayWithKeys(double Seconds, int32 Frames, const FString& Keys, int32& OutAdvanced, FString& OutError,
	TArray<FString>* OutNotes)
{
	OutAdvanced = 0;
	TArray<UnrealMcpRqlPlayLocal::FHeldKey> Held;
	if (!UnrealMcpRqlPlayLocal::ParseHeldKeys(Keys, Held, OutError))
		return false;
	const int32 Count = PlayFrameCount(Seconds, Frames, OutError);
	return Count >= 0 && UnrealMcpRqlPlayLocal::AdvanceHoldingKeys(Held, Count, OutAdvanced, OutError, OutNotes);
}

namespace UnrealMcpRqlPlayLocal
{
	/** Less than this is floor adjustment or float noise, not the pawn moving or turning. */
	constexpr double StillCentimeters = 1.0;
	constexpr double StillDegrees = 1.0;

	/** Input held at least this long (0.25 s) is expected to show: a shorter press may be a toggle that moves nothing. */
	constexpr int32 MinHeldFramesToNote = UnrealMcpRql::PlayFrameRate / 4;

	/** What the call did to the pawn it started with; false, nothing measured, when the call changed the possessed pawn. */
	bool MeasurePawnMotion(const FUnrealMcpPlayState& Before, FUnrealMcpPlayState& State)
	{
		if (!State.Pawn || State.Pawn != Before.Pawn)
			return false;
		const FVector Displacement = State.Location - Before.Location;
		const double Elapsed = State.Time - Before.Time;
		State.Moved = Displacement.Size();
		State.MeasuredVelocity = Elapsed > 0.0 ? Displacement / Elapsed : FVector::ZeroVector;
		State.Turned = FMath::RadiansToDegrees(Before.Rotation.Quaternion().AngularDistance(State.Rotation.Quaternion()));
		return true;
	}

	/**
	 * Held input the pawn did not respond to. A fact, not a verdict: the core cannot tell a movement key from a menu key, so
	 * the note is conditional. Not noted: a short press, a game time that stood still, or a turned camera (the input did
	 * something).
	 */
	void NoteInputThatMovedNothing(const FUnrealMcpPlayState& Before, const TArray<FHeldKey>& Keys, int32 HeldFrames, double Farthest, FUnrealMcpPlayState& State)
	{
		if (HeldFrames < MinHeldFramesToNote || State.Time <= Before.Time || FMath::Max(State.Moved, Farthest) >= StillCentimeters
			|| !State.ControlRotation.Equals(Before.ControlRotation, StillDegrees))
			return;
		const TArray<FHeldKey> Held = Keys.FilterByPredicate([](const FHeldKey& Key) { return Key.Value != 0.f; });
		if (Held.Num() == 0)
			return;
		const FString Names = FString::JoinBy(Held, TEXT(", "), [](const FHeldKey& Key) { return Key.Key.ToString(); });
		const FString Response = State.Turned < StillDegrees ? FString(TEXT("the pawn neither moved nor turned"))
			: FString::Printf(TEXT("the pawn turned %.0f degrees without moving"), State.Turned);
		State.Notes.Add(FString::Printf(TEXT("Held %s for %.2f s and %s: if this input should move it, check whether the pawn is blocked and whether the action is bound."),
			*Names, static_cast<double>(HeldFrames) / UnrealMcpRql::PlayFrameRate, *Response));
	}
}

bool UnrealMcpRql::AdvancePlayAndRead(double Seconds, int32 Frames, const FString& Keys, FUnrealMcpPlayState& OutState, FString& OutError)
{
	using namespace UnrealMcpRqlPlayLocal;
	TArray<FHeldKey> Held;
	if (!ParseHeldKeys(Keys, Held, OutError))
		return false;
	const int32 Count = PlayFrameCount(Seconds, Frames, OutError);
	if (Count < 0)
		return false;
	const FUnrealMcpPlayState Before = ReadPlayState();
	int32 Advanced = 0;
	TArray<FString> Notes;
	double Farthest = 0.0;
	if (!AdvanceHoldingKeys(Held, Count, Advanced, OutError, &Notes, &Farthest))
		return false;
	OutState = ReadPlayState();
	OutState.FramesAdvanced = Advanced;
	OutState.Notes.Append(Notes);
	if (MeasurePawnMotion(Before, OutState))
		NoteInputThatMovedNothing(Before, Held, Count, Farthest, OutState);
	return true;
}
