// Copyright (c) 2026 IvanMurzak/Unreal-AI-Niagara. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FNiagaraSystemViewModel;
class UNiagaraDataInterface;
class UNiagaraEmitterEditorData;
class UNiagaraStackEntry;
class UNiagaraStackFunctionInput;
class UNiagaraStackModuleItem;
class UNiagaraStackScriptItemGroup;
class UNiagaraStackViewModel;
class UNiagaraSystem;
struct FNiagaraEmitterHandle;
struct FNiagaraTypeDefinition;

/**
 * The Niagara editor's stack (emitters, stages, modules, module inputs) for the `niagara` operations and members: the
 * same view models the System editor builds (FNiagaraSystemViewModel, data-processing mode: no preview component, no
 * auto compile, as the engine's own audit commandlet opens a system), so every edit runs the editor's code paths
 * (rapid-iteration parameters, override pins, dynamic inputs, transactions). Names are matched ignoring case, spaces and
 * underscores; misses fail (UnrealMcpRql::Fail) naming what exists.
 */
namespace UnrealAINiagara
{
	/** "Spawn Rate", "spawn_rate" and "SpawnRate" are the same name. */
	FString Normalize(const FString& Name);

	/** The emitter handle named @p Name; nullptr after Fail (naming the system's emitters). */
	FNiagaraEmitterHandle* FindEmitter(UNiagaraSystem& System, const FString& Name);

	/** The emitter names of @p System, comma separated. */
	FString EmitterNames(const UNiagaraSystem& System);

	/** The user-facing name of a Niagara type: float, int, bool, Vector, Position, LinearColor, an enum or struct name. */
	FString TypeName(const FNiagaraTypeDefinition& Type);

	/** Read @p Text into a value of @p Type at @p Memory (numbers, "(x, y, z)" or a single number for every component, "#rrggbb", enum entries, true / false, or UE's struct text); false with @p OutError. */
	bool ParseValue(const FNiagaraTypeDefinition& Type, const FString& Text, uint8* Memory, FString& OutError);

	/** The text form ParseValue reads back. */
	FString FormatValue(const FNiagaraTypeDefinition& Type, const uint8* Memory);

	/** A data interface's editable properties as "Class(Name=Value, ...)". */
	FString FormatDataInterface(UNiagaraDataInterface& Data);

	/**
	 * Set data interface properties from "Name=Value, ..." (UE's text form per value), or from an asset path alone when one
	 * property takes that asset ("/Engine/BasicShapes/Cone" for a static mesh interface's DefaultMesh); false with @p OutError.
	 */
	bool WriteDataInterface(UNiagaraDataInterface& Data, const FString& Text, FString& OutError);

	/** One module of a stack: the stack item plus where it sits. */
	struct FModuleRef
	{
		UNiagaraStackModuleItem* Item = nullptr;
		FString Emitter;
		FString Stage;
		FString Name;
	};

	/** One module input, by its path below the module ("Lifetime", or "Lifetime.Minimum" inside a dynamic input). */
	struct FInputRef
	{
		UNiagaraStackFunctionInput* Input = nullptr;
		FString Path;
		/** Hidden by the module's current options (an edit condition that hides it, e.g. Lifetime while Lifetime Mode is Random). */
		bool bHidden = false;
	};

	/** A system opened as the System editor's stack for one call. */
	class FSystemStack
	{
	public:
		explicit FSystemStack(UNiagaraSystem& InSystem);
		~FSystemStack();

		UNiagaraSystem& GetSystem() const { return System; }

		/** The view model; nullptr after Fail when the system cannot be opened safely (see Check). */
		FNiagaraSystemViewModel* ViewModel() const;

		/**
		 * Whether the system could be opened: a system whose modules use Niagara types of a plugin that is not loaded
		 * (Niagara Fluids without ChaosNiagara) would hit an assertion in the Niagara editor, so it is refused; false after Fail.
		 */
		bool Check() const;

		/** The stack of @p Emitter ("" or "System": the system's own stack); nullptr after Fail. */
		UNiagaraStackViewModel* Stack(const FString& Emitter);

		/** Every module of @p Emitter's stack (refreshed), in stack order. */
		TArray<FModuleRef> Modules(const FString& Emitter);

		/** The module @p Module ("Spawn Rate", or "Particle Update/Collision" when two stages have one); false after Fail. */
		bool FindModule(const FString& Emitter, const FString& Module, FModuleRef& Out);

		/** The stage group @p Stage ("Particle Update", "Emitter Spawn", a simulation stage name); nullptr after Fail. */
		UNiagaraStackScriptItemGroup* FindStage(const FString& Emitter, const FString& Stage);

		/** Rebuild the stack after a structural edit (a dynamic input set, a module added). */
		void Refresh(const FString& Emitter);

		/** Set one module input from text (see SetInput's guide entry); returns the new value text, or empty after Fail. */
		FString SetInput(const FString& Emitter, const FString& Module, const FString& Input, const FString& Value);

		/** The modules of every stack (system and each emitter) with their inputs, as JSON for the Modules member. */
		TSharedPtr<FJsonValue> ToJson(bool bWithInputs);

	private:
		UNiagaraSystem& System;
		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
		FString Problem;
		/** Emitters shown as their Summary view (Niagara Fluids templates): opened in the full stack, restored on close. */
		TArray<TWeakObjectPtr<UNiagaraEmitterEditorData>> SummaryViews;
	};

	/** The visible inputs of @p Module (dynamic inputs' own inputs follow theirs, with dotted paths); hidden ones too with @p bHidden. */
	TArray<FInputRef> Inputs(const FModuleRef& Module, bool bHidden = false);

	/** The value of an input as text: a local value, "@Particles.NormalizedAge" (linked), "random(1, 2)", "curve(0: 1, 1: 0)", an asset path, ... */
	FString FormatInput(UNiagaraStackFunctionInput& Input);
}
