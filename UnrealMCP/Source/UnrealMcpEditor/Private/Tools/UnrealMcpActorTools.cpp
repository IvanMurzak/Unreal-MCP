// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpObjectRef.h"
#include "UnrealMcpPropertyJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "ScopedTransaction.h"      // FScopedTransaction — gives the per-handler Modify() snapshots an open transaction to record into
#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"   // StaticFindObjectFast

/**
 * The actor & component tool family (docs/ARCHITECTURE.md §10 "actor family", declared via §3.3).
 *
 * ~13 native C++ tools over UClass/FProperty reflection. Object references are resolved through
 * FUnrealMcpObjectRef (§3.2: label → FindObject → soft-path load); scoped reads + FProperty/transform
 * writes go through FUnrealMcpPropertyJson. Every Handle() body runs ON the game thread — the bridge
 * dispatches Registry.Execute through FUnrealMcpGameThreadDispatcher (§4), so the bodies below touch
 * the editor world / UObject graph directly without any extra marshalling.
 */
namespace
{
	// --- Local schema builders (the typed builder helpers don't cover rotators / property bags / lists) ---

	/** `{pitch,yaw,roll: number}` — the §3.2 FRotator mapping (degrees). Read back via FUnrealMcpToolCall::GetRotator. */
	TSharedPtr<FJsonObject> MakeRotatorSchema(const FString& Desc)
	{
		auto Num = []() { TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("type"), TEXT("number")); return N; };
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetObjectField(TEXT("pitch"), Num());
		Props->SetObjectField(TEXT("yaw"), Num());
		Props->SetObjectField(TEXT("roll"), Num());

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		if (!Desc.IsEmpty())
			Schema->SetStringField(TEXT("description"), Desc);
		Schema->SetObjectField(TEXT("properties"), Props);
		return Schema;
	}

	/** A free-form `{ key: value }` property bag — arbitrary FProperty writes (§3.2), additionalProperties allowed. */
	TSharedPtr<FJsonObject> MakePropertyBagSchema(const FString& Desc)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		if (!Desc.IsEmpty())
			Schema->SetStringField(TEXT("description"), Desc);
		Schema->SetBoolField(TEXT("additionalProperties"), true);
		return Schema;
	}

	/** An `array` of strings — used for the §3.2 scoped-read `paths` filter. */
	TSharedPtr<FJsonObject> MakeStringArraySchema(const FString& Desc)
	{
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("array"));
		if (!Desc.IsEmpty())
			Schema->SetStringField(TEXT("description"), Desc);
		Schema->SetObjectField(TEXT("items"), Items);
		return Schema;
	}

	// --- Small argument helpers ---

	/** Read a string array argument (the scoped-read `paths` filter); empty when absent or not an array. */
	TArray<FString> GetStringArray(const FUnrealMcpToolCall& Call, const FString& Key)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Call.Arguments->TryGetArrayField(Key, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
					Out.Add(S);
			}
		}
		return Out;
	}

	/** The 'properties' object argument (the write property bag); null when absent. */
	TSharedPtr<FJsonObject> GetPropertiesArg(const FUnrealMcpToolCall& Call)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Call.Arguments->TryGetObjectField(TEXT("properties"), Obj) && Obj && Obj->IsValid())
			return *Obj;
		return nullptr;
	}

	/** Resolve an actor by ref or produce a uniform error result. Returns null + fills OutError on miss. */
	AActor* ResolveActorOrError(const FString& Ref, FUnrealMcpToolResult& OutError, bool& bFailed)
	{
		bFailed = false;
		if (Ref.IsEmpty())
		{
			OutError = FUnrealMcpToolResult::Error(TEXT("Missing required 'actor' reference."));
			bFailed = true;
			return nullptr;
		}
		AActor* Actor = FUnrealMcpObjectRef::ResolveActor(Ref);
		if (!Actor)
		{
			OutError = FUnrealMcpToolResult::Error(FString::Printf(TEXT("No actor matched '%s' (by label/name/path)."), *Ref));
			bFailed = true;
		}
		return Actor;
	}

	/** Build a `{ applied, errors:[...] }` structured result for the *-modify tools. */
	FUnrealMcpToolResult MakeModifyResult(const FString& Subject, int32 Applied, const TArray<FString>& Errors)
	{
		TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
		Structured->SetNumberField(TEXT("applied"), Applied);
		TArray<TSharedPtr<FJsonValue>> ErrArray;
		for (const FString& E : Errors)
			ErrArray.Add(MakeShared<FJsonValueString>(E));
		Structured->SetArrayField(TEXT("errors"), ErrArray);

		// Partial success is still success: report the count and surface per-field reasons in the payload.
		const FString Message = Errors.Num() == 0
			? FString::Printf(TEXT("Applied %d propert%s to %s."), Applied, Applied == 1 ? TEXT("y") : TEXT("ies"), *Subject)
			: FString::Printf(TEXT("Applied %d propert%s to %s; %d field(s) failed."),
				Applied, Applied == 1 ? TEXT("y") : TEXT("ies"), *Subject, Errors.Num());
		return FUnrealMcpToolResult::Success(Message, Structured);
	}
}

namespace UnrealMcpActorTools
{
	UNREALMCPEDITOR_API void Register(FUnrealMcpToolRegistry& Registry)
	{
		// ----------------------------------------------------------------------------------------
		// actor-create — spawn from a class path / Blueprint asset path.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-create"))
			.Title(TEXT("Create Actor"))
			.Description(TEXT("Spawn a new actor in the current editor level from a native class path "
			                  "(e.g. '/Script/Engine.StaticMeshActor', 'PointLight') or a Blueprint asset "
			                  "path. Optionally set label, location, rotation, and a parent actor to attach to."))
			.ParamString(TEXT("classPath"), TEXT("Native class or Blueprint asset path/name."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Actor label. Auto-generated when omitted."))
			.ParamVector(TEXT("location"), TEXT("World location {x,y,z}. Defaults to origin."))
			.Param(TEXT("rotation"), TEXT("object"), TEXT("World rotation {pitch,yaw,roll} in degrees."), EUnrealMcpParamRequirement::Optional, MakeRotatorSchema(TEXT("World rotation {pitch,yaw,roll} in degrees.")))
			.ParamString(TEXT("parentActor"), TEXT("Label/path of an actor to attach the new actor to."))
			.DestructiveHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No editor world available (not running in the editor)."));

				const FString ClassPath = Call.GetString(TEXT("classPath"));
				UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassPath);
				if (!Class || !Class->IsChildOf(AActor::StaticClass()))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a spawnable Actor class."), *ClassPath));
				if (Class->HasAnyClassFlags(CLASS_Abstract))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is abstract and cannot be spawned."), *ClassPath));

				const FVector Location = Call.GetVector(TEXT("location"), FVector::ZeroVector);
				const FRotator Rotation = Call.GetRotator(TEXT("rotation"), FRotator::ZeroRotator);

				// Resolve the parent (if requested) BEFORE spawning: resolving after the spawn would leak an
				// orphan actor on a bad ref and the error would carry no identity for the leaked actor.
				AActor* Parent = nullptr;
				if (Call.Has(TEXT("parentActor")))
				{
					const FString ParentRef = Call.GetString(TEXT("parentActor"));
					if (!ParentRef.IsEmpty())
					{
						Parent = FUnrealMcpObjectRef::ResolveActor(ParentRef);
						if (!Parent)
							return FUnrealMcpToolResult::Error(FString::Printf(
								TEXT("Parent actor '%s' was not found; nothing was spawned."), *ParentRef));
					}
				}

				// Spawn through the engine UWorld path (not UEditorActorSubsystem): the actor still lands in
				// the editor world (outliner-visible), and this avoids the editor viewport-snapping code that
				// is unsafe under headless -nullrhi automation. SpawnParameters get a transactional, dirtyable actor.
				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ActorCreate", "MCP: Create Actor"));
				FActorSpawnParameters SpawnParams;
				SpawnParams.ObjectFlags |= RF_Transactional;
				AActor* Actor = World->SpawnActor<AActor>(Class, Location, Rotation, SpawnParams);
				if (!Actor)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to spawn '%s'."), *ClassPath));

				if (Call.Has(TEXT("name")))
				{
					const FString Label = Call.GetString(TEXT("name"));
					// SetActorLabelUnique (not SetActorLabel): a user-supplied label that collides with an
					// existing actor's label would otherwise make both ambiguous to ResolveActor.
					if (!Label.IsEmpty())
						FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
				}

				if (Parent)
					Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Spawned %s '%s'."), *Class->GetName(), *Actor->GetActorLabel()),
					FUnrealMcpObjectRef::ActorIdentity(Actor));
			});

		// ----------------------------------------------------------------------------------------
		// actor-destroy
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-destroy"))
			.Title(TEXT("Destroy Actor"))
			.Description(TEXT("Remove an actor from the current editor level. Identify it by label, object "
			                  "name, or full path."))
			.ParamString(TEXT("actor"), TEXT("Actor label / name / path to destroy."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FUnrealMcpToolResult Err; bool bFailed = false;
				AActor* Actor = ResolveActorOrError(Call.GetString(TEXT("actor")), Err, bFailed);
				if (bFailed) return Err;

				UWorld* World = Actor->GetWorld();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("Actor has no world."));

				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ActorDestroy", "MCP: Destroy Actor"));
				const FString Label = Actor->GetActorLabel();
				if (!World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to destroy actor '%s'."), *Label));
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Destroyed actor '%s'."), *Label));
			});

		// ----------------------------------------------------------------------------------------
		// actor-duplicate
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-duplicate"))
			.Title(TEXT("Duplicate Actor"))
			.Description(TEXT("Duplicate an existing actor in the current level, optionally offsetting its "
			                  "location and assigning a new label."))
			.ParamString(TEXT("actor"), TEXT("Actor label / name / path to duplicate."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Label for the duplicate. Auto-generated when omitted."))
			.ParamVector(TEXT("offset"), TEXT("World-space location offset {x,y,z} applied to the duplicate. Defaults to none."))
			.DestructiveHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FUnrealMcpToolResult Err; bool bFailed = false;
				AActor* Source = ResolveActorOrError(Call.GetString(TEXT("actor")), Err, bFailed);
				if (bFailed) return Err;

				UWorld* World = Source->GetWorld();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("Source actor has no world."));

				// Duplicate by spawning the same class with the source as a property template (engine path,
				// headless-safe), then apply the requested location offset.
				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ActorDuplicate", "MCP: Duplicate Actor"));
				const FVector Offset = Call.GetVector(TEXT("offset"), FVector::ZeroVector);
				FActorSpawnParameters SpawnParams;
				SpawnParams.Template = Source;
				SpawnParams.ObjectFlags |= RF_Transactional;
				const FTransform DupXform(Source->GetActorRotation(), Source->GetActorLocation() + Offset, Source->GetActorScale3D());
				AActor* Dup = World->SpawnActor<AActor>(Source->GetClass(), DupXform, SpawnParams);
				if (!Dup)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to duplicate actor '%s'."), *Source->GetActorLabel()));

				const FString NameArg = Call.GetString(TEXT("name"));
				// The spawn template copies the source's label verbatim, so two actors would share a label and
				// ResolveActor could not disambiguate them. SetActorLabelUnique guarantees a unique label whether
				// the caller supplied one explicitly or we fall back to the source's label.
				FActorLabelUtilities::SetActorLabelUnique(Dup, NameArg.IsEmpty() ? Source->GetActorLabel() : NameArg);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Duplicated '%s' as '%s'."), *Source->GetActorLabel(), *Dup->GetActorLabel()),
					FUnrealMcpObjectRef::ActorIdentity(Dup));
			});

		// ----------------------------------------------------------------------------------------
		// actor-find — filter by label/path/class; scoped data reads via `paths` (§3.2).
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-find"))
			.Title(TEXT("Find Actors"))
			.Description(TEXT("List actors in the current level filtered by label substring, path substring, "
			                  "and/or class. Optionally include each actor's reflected data, scoped to the "
			                  "dotted 'paths' filter to save tokens."))
			.ParamString(TEXT("labelFilter"), TEXT("Case-insensitive substring matched against actor labels."))
			.ParamString(TEXT("pathFilter"), TEXT("Case-insensitive substring matched against full actor paths."))
			.ParamString(TEXT("classFilter"), TEXT("Class path/name; matches actors of this class or a subclass."))
			.Param(TEXT("paths"), TEXT("array"), TEXT("Dotted property paths to include per actor (scoped read). Identity only when omitted."), EUnrealMcpParamRequirement::Optional, MakeStringArraySchema(TEXT("Dotted property paths to include per actor (scoped read).")))
			.ParamInt(TEXT("limit"), TEXT("Maximum number of actors to return. Defaults to 100."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				UWorld* World = FUnrealMcpObjectRef::GetEditorWorld();
				if (!World)
					return FUnrealMcpToolResult::Error(TEXT("No editor world available."));

				const FString LabelFilter = Call.GetString(TEXT("labelFilter"));
				const FString PathFilter = Call.GetString(TEXT("pathFilter"));
				const FString ClassFilter = Call.GetString(TEXT("classFilter"));
				const TArray<FString> Paths = GetStringArray(Call, TEXT("paths"));
				const int32 Limit = static_cast<int32>(Call.GetInt(TEXT("limit"), 100));

				UClass* FilterClass = ClassFilter.IsEmpty() ? nullptr : FUnrealMcpObjectRef::ResolveClass(ClassFilter);
				if (!ClassFilter.IsEmpty() && !FilterClass)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("classFilter '%s' did not resolve to a class."), *ClassFilter));

				TArray<TSharedPtr<FJsonValue>> Results;
				int32 Matched = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor)
						continue;
					if (FilterClass && !Actor->IsA(FilterClass))
						continue;
					if (!LabelFilter.IsEmpty() && !Actor->GetActorLabel().Contains(LabelFilter, ESearchCase::IgnoreCase))
						continue;
					if (!PathFilter.IsEmpty() && !Actor->GetPathName().Contains(PathFilter, ESearchCase::IgnoreCase))
						continue;

					++Matched;
					if (Results.Num() >= Limit && Limit > 0)
						continue; // keep counting total matches, but stop materializing entries

					TSharedPtr<FJsonObject> Entry = FUnrealMcpObjectRef::ActorIdentity(Actor);
					if (Paths.Num() > 0)
						Entry->SetObjectField(TEXT("data"), FUnrealMcpPropertyJson::SerializeObject(Actor, Paths));
					Results.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("count"), Results.Num());
				Structured->SetNumberField(TEXT("total"), Matched);
				Structured->SetArrayField(TEXT("actors"), Results);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Found %d actor(s) (returned %d)."), Matched, Results.Num()), Structured);
			});

		// ----------------------------------------------------------------------------------------
		// actor-modify — FProperty writes including transform (§3.2 mapping).
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-modify"))
			.Title(TEXT("Modify Actor"))
			.Description(TEXT("Write reflected properties on an actor. Transform keys 'location' {x,y,z}, "
			                  "'rotation' {pitch,yaw,roll}, and 'scale' {x,y,z} are routed to the transform "
			                  "APIs; all other keys are set by name through FProperty reflection."))
			.ParamString(TEXT("actor"), TEXT("Actor label / name / path to modify."), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("properties"), TEXT("object"), TEXT("Property bag of name -> value to write (incl. transform keys)."), EUnrealMcpParamRequirement::Required, MakePropertyBagSchema(TEXT("Property bag of name -> value to write.")))
			.DestructiveHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FUnrealMcpToolResult Err; bool bFailed = false;
				AActor* Actor = ResolveActorOrError(Call.GetString(TEXT("actor")), Err, bFailed);
				if (bFailed) return Err;

				TSharedPtr<FJsonObject> Props = GetPropertiesArg(Call);
				if (!Props.IsValid())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'properties' object."));

				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ActorModify", "MCP: Modify Actor"));
				TArray<FString> Errors;
				const int32 Applied = FUnrealMcpPropertyJson::ApplyProperties(Actor, Props, Errors);
				return MakeModifyResult(FString::Printf(TEXT("actor '%s'"), *Actor->GetActorLabel()), Applied, Errors);
			});

		// ----------------------------------------------------------------------------------------
		// actor-set-parent — attach / detach.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-set-parent"))
			.Title(TEXT("Set Actor Parent"))
			.Description(TEXT("Attach an actor to a parent actor (optionally at a named socket), or detach it "
			                  "when 'parent' is omitted/empty. World transform is preserved by default."))
			.ParamString(TEXT("actor"), TEXT("Child actor label / name / path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("parent"), TEXT("Parent actor label / name / path. Omit or leave empty to detach."))
			.ParamString(TEXT("socket"), TEXT("Optional socket/bone name on the parent to attach to."))
			.ParamBool(TEXT("keepWorldTransform"), TEXT("Preserve the child's world transform across the (de)attach. Defaults to true."))
			.DestructiveHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FUnrealMcpToolResult Err; bool bFailed = false;
				AActor* Child = ResolveActorOrError(Call.GetString(TEXT("actor")), Err, bFailed);
				if (bFailed) return Err;

				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ActorSetParent", "MCP: Set Actor Parent"));
				const bool bKeepWorld = Call.GetBool(TEXT("keepWorldTransform"), true);
				const FString ParentRef = Call.GetString(TEXT("parent"));

				if (ParentRef.IsEmpty())
				{
					Child->DetachFromActor(bKeepWorld
						? FDetachmentTransformRules::KeepWorldTransform
						: FDetachmentTransformRules::KeepRelativeTransform);
					return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Detached '%s' from its parent."), *Child->GetActorLabel()));
				}

				AActor* Parent = FUnrealMcpObjectRef::ResolveActor(ParentRef);
				if (!Parent)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No parent actor matched '%s'."), *ParentRef));
				if (Parent == Child)
					return FUnrealMcpToolResult::Error(TEXT("An actor cannot be parented to itself."));
				// Reject a cycle up front: attaching to a descendant would be silently dropped by the engine.
				if (Parent->IsAttachedTo(Child))
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Cannot attach '%s' to '%s': '%s' is already a descendant (would create a cycle)."),
						*Child->GetActorLabel(), *Parent->GetActorLabel(), *Parent->GetActorLabel()));

				const FAttachmentTransformRules Rules = bKeepWorld
					? FAttachmentTransformRules::KeepWorldTransform
					: FAttachmentTransformRules::KeepRelativeTransform;
				const FName Socket(*Call.GetString(TEXT("socket")));
				Child->AttachToActor(Parent, Rules, Socket);
				// AttachToActor returns void and silently no-ops on rejection (e.g. missing root component);
				// verify the attachment actually took rather than reporting a false success.
				if (Child->GetAttachParentActor() != Parent)
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Engine rejected attaching '%s' to '%s' (missing root component or invalid attachment)."),
						*Child->GetActorLabel(), *Parent->GetActorLabel()));
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Attached '%s' to '%s'."), *Child->GetActorLabel(), *Parent->GetActorLabel()));
			});

		// ----------------------------------------------------------------------------------------
		// actor-component-add
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-component-add"))
			.Title(TEXT("Add Component"))
			.Description(TEXT("Add a new component (by class path/name) to an actor as an instance component. "
			                  "Scene components attach to the actor's root, or become the root if it has none."))
			.ParamString(TEXT("actor"), TEXT("Owning actor label / name / path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("componentClass"), TEXT("Component class path/name (e.g. 'StaticMeshComponent')."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("name"), TEXT("Optional component name. Auto-generated when omitted."))
			.DestructiveHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FUnrealMcpToolResult Err; bool bFailed = false;
				AActor* Actor = ResolveActorOrError(Call.GetString(TEXT("actor")), Err, bFailed);
				if (bFailed) return Err;

				const FString ClassRef = Call.GetString(TEXT("componentClass"));
				UClass* CompClass = FUnrealMcpObjectRef::ResolveClass(ClassRef);
				if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is not a UActorComponent class."), *ClassRef));
				if (CompClass->HasAnyClassFlags(CLASS_Abstract))
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("'%s' is abstract and cannot be instantiated."), *ClassRef));

				const FString NameArg = Call.GetString(TEXT("name"));
				FName CompName;
				if (NameArg.IsEmpty())
				{
					CompName = MakeUniqueObjectName(Actor, CompClass, CompClass->GetFName());
				}
				else
				{
					CompName = FName(*NameArg);
					// Reject a name collision BEFORE NewObject: reusing an existing subobject name under the
					// actor re-allocates it in place — a different class triggers a StaticAllocateObject fatal
					// assert (editor crash); the same class silently clobbers. Surface a structured error.
					if (StaticFindObjectFast(nullptr, Actor, CompName))
						return FUnrealMcpToolResult::Error(FString::Printf(
							TEXT("A component named '%s' already exists on '%s'; choose a different name."),
							*NameArg, *Actor->GetActorLabel()));
				}

				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ComponentAdd", "MCP: Add Component"));
				Actor->Modify();
				UActorComponent* NewComp = NewObject<UActorComponent>(Actor, CompClass, CompName, RF_Transactional);
				if (!NewComp)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("Failed to construct component '%s'."), *ClassRef));

				Actor->AddInstanceComponent(NewComp);
				if (USceneComponent* SceneComp = Cast<USceneComponent>(NewComp))
				{
					if (USceneComponent* Root = Actor->GetRootComponent())
						SceneComp->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
					else
						Actor->SetRootComponent(SceneComp);
				}
				NewComp->OnComponentCreated();
				NewComp->RegisterComponent();
				Actor->MarkPackageDirty();

				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("Added %s '%s' to '%s'."), *CompClass->GetName(), *NewComp->GetName(), *Actor->GetActorLabel()),
					FUnrealMcpObjectRef::ComponentIdentity(NewComp));
			});

		// ----------------------------------------------------------------------------------------
		// actor-component-destroy
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-component-destroy"))
			.Title(TEXT("Destroy Component"))
			.Description(TEXT("Remove a component (by name) from an actor."))
			.ParamString(TEXT("actor"), TEXT("Owning actor label / name / path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("component"), TEXT("Component name to destroy."), EUnrealMcpParamRequirement::Required)
			.DestructiveHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FUnrealMcpToolResult Err; bool bFailed = false;
				AActor* Actor = ResolveActorOrError(Call.GetString(TEXT("actor")), Err, bFailed);
				if (bFailed) return Err;

				const FString CompRef = Call.GetString(TEXT("component"));
				UActorComponent* Comp = FUnrealMcpObjectRef::ResolveComponent(Actor, CompRef);
				if (!Comp)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No component matched '%s' on '%s'."), *CompRef, *Actor->GetActorLabel()));

				// Only instance components can be destroyed cleanly: a native/inherited component would no-op
				// RemoveInstanceComponent yet still proceed to DestroyComponent, leaving a broken (possibly
				// rootless) actor while reporting success. Reject it explicitly instead.
				if (!Actor->GetInstanceComponents().Contains(Comp))
					return FUnrealMcpToolResult::Error(FString::Printf(
						TEXT("Component '%s' on '%s' is not an instance component (native/inherited components cannot be destroyed this way)."),
						*CompRef, *Actor->GetActorLabel()));

				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ComponentDestroy", "MCP: Destroy Component"));
				const FString CompName = Comp->GetName();
				Actor->Modify();
				Actor->RemoveInstanceComponent(Comp);
				Comp->DestroyComponent();
				Actor->MarkPackageDirty();
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Destroyed component '%s' on '%s'."), *CompName, *Actor->GetActorLabel()));
			});

		// ----------------------------------------------------------------------------------------
		// actor-component-get — scoped reads (§3.2).
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-component-get"))
			.Title(TEXT("Get Component Data"))
			.Description(TEXT("Read a component's reflected properties, optionally scoped to the dotted 'paths' filter."))
			.ParamString(TEXT("actor"), TEXT("Owning actor label / name / path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("component"), TEXT("Component name to read."), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("paths"), TEXT("array"), TEXT("Dotted property paths to include (scoped read). Full object when omitted."), EUnrealMcpParamRequirement::Optional, MakeStringArraySchema(TEXT("Dotted property paths to include (scoped read).")))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FUnrealMcpToolResult Err; bool bFailed = false;
				AActor* Actor = ResolveActorOrError(Call.GetString(TEXT("actor")), Err, bFailed);
				if (bFailed) return Err;

				const FString CompRef = Call.GetString(TEXT("component"));
				UActorComponent* Comp = FUnrealMcpObjectRef::ResolveComponent(Actor, CompRef);
				if (!Comp)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No component matched '%s' on '%s'."), *CompRef, *Actor->GetActorLabel()));

				const TArray<FString> Paths = GetStringArray(Call, TEXT("paths"));
				TSharedPtr<FJsonObject> Data = FUnrealMcpObjectRef::ComponentIdentity(Comp);
				Data->SetObjectField(TEXT("data"), FUnrealMcpPropertyJson::SerializeObject(Comp, Paths));
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Read component '%s'."), *Comp->GetName()), Data);
			});

		// ----------------------------------------------------------------------------------------
		// actor-component-modify — FProperty writes (incl. relative transform) on a component.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-component-modify"))
			.Title(TEXT("Modify Component"))
			.Description(TEXT("Write reflected properties on a component. Scene components additionally accept "
			                  "'relativeLocation' {x,y,z}, 'relativeRotation' {pitch,yaw,roll}, and "
			                  "'relativeScale3D' {x,y,z} routed to the relative-transform APIs."))
			.ParamString(TEXT("actor"), TEXT("Owning actor label / name / path."), EUnrealMcpParamRequirement::Required)
			.ParamString(TEXT("component"), TEXT("Component name to modify."), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("properties"), TEXT("object"), TEXT("Property bag of name -> value to write."), EUnrealMcpParamRequirement::Required, MakePropertyBagSchema(TEXT("Property bag of name -> value to write.")))
			.DestructiveHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				FUnrealMcpToolResult Err; bool bFailed = false;
				AActor* Actor = ResolveActorOrError(Call.GetString(TEXT("actor")), Err, bFailed);
				if (bFailed) return Err;

				const FString CompRef = Call.GetString(TEXT("component"));
				UActorComponent* Comp = FUnrealMcpObjectRef::ResolveComponent(Actor, CompRef);
				if (!Comp)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No component matched '%s' on '%s'."), *CompRef, *Actor->GetActorLabel()));

				TSharedPtr<FJsonObject> Props = GetPropertiesArg(Call);
				if (!Props.IsValid())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'properties' object."));

				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ComponentModify", "MCP: Modify Component"));
				TArray<FString> Errors;
				const int32 Applied = FUnrealMcpPropertyJson::ApplyProperties(Comp, Props, Errors);
				return MakeModifyResult(FString::Printf(TEXT("component '%s'"), *Comp->GetName()), Applied, Errors);
			});

		// ----------------------------------------------------------------------------------------
		// actor-component-list-all — paginated concrete UActorComponent classes.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("actor-component-list-all"))
			.Title(TEXT("List Component Classes"))
			.Description(TEXT("List concrete (non-abstract) UActorComponent classes available to add, filtered "
			                  "by an optional name substring and paginated via offset/limit."))
			.ParamString(TEXT("search"), TEXT("Case-insensitive substring matched against class names."))
			.ParamInt(TEXT("offset"), TEXT("Number of matching classes to skip. Defaults to 0."))
			.ParamInt(TEXT("limit"), TEXT("Maximum number of classes to return. Defaults to 100."))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Search = Call.GetString(TEXT("search"));
				const int32 Offset = FMath::Max(0, static_cast<int32>(Call.GetInt(TEXT("offset"), 0)));
				const int32 Limit = static_cast<int32>(Call.GetInt(TEXT("limit"), 100));

				// Collect first so total/pagination are stable and class order is deterministic by name.
				TArray<UClass*> Matches;
				for (TObjectIterator<UClass> It; It; ++It)
				{
					UClass* Class = *It;
					if (!Class || !Class->IsChildOf(UActorComponent::StaticClass()))
						continue;
					if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
						continue;
					if (!Search.IsEmpty() && !Class->GetName().Contains(Search, ESearchCase::IgnoreCase))
						continue;
					Matches.Add(Class);
				}
				Matches.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });

				TArray<TSharedPtr<FJsonValue>> Page;
				for (int32 i = Offset; i < Matches.Num() && (Limit <= 0 || Page.Num() < Limit); ++i)
				{
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Matches[i]->GetName());
					Entry->SetStringField(TEXT("path"), Matches[i]->GetPathName());
					Page.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetNumberField(TEXT("total"), Matches.Num());
				Structured->SetNumberField(TEXT("offset"), Offset);
				Structured->SetNumberField(TEXT("count"), Page.Num());
				Structured->SetArrayField(TEXT("components"), Page);
				return FUnrealMcpToolResult::Success(
					FString::Printf(TEXT("%d component class(es) total; returned %d."), Matches.Num(), Page.Num()), Structured);
			});

		// ----------------------------------------------------------------------------------------
		// object-get-data — read a generic UObject by path (or live actor by label).
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("object-get-data"))
			.Title(TEXT("Get Object Data"))
			.Description(TEXT("Read the reflected properties of any UObject resolved by object path "
			                  "('/Game/Folder/Asset.Asset') or, for scene objects, by actor label. Optionally "
			                  "scoped to the dotted 'paths' filter."))
			.ParamString(TEXT("object"), TEXT("Object path or actor label/name/path."), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("paths"), TEXT("array"), TEXT("Dotted property paths to include (scoped read). Full object when omitted."), EUnrealMcpParamRequirement::Optional, MakeStringArraySchema(TEXT("Dotted property paths to include (scoped read).")))
			.ReadOnlyHint(true)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Ref = Call.GetString(TEXT("object"));
				if (Ref.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'object' reference."));

				UObject* Object = FUnrealMcpObjectRef::ResolveObject(Ref, FUnrealMcpObjectRef::GetEditorWorld());
				if (!Object)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No object matched '%s'."), *Ref));

				const TArray<FString> Paths = GetStringArray(Call, TEXT("paths"));
				TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
				Structured->SetStringField(TEXT("name"), Object->GetName());
				Structured->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
				Structured->SetStringField(TEXT("path"), Object->GetPathName());
				Structured->SetObjectField(TEXT("data"), FUnrealMcpPropertyJson::SerializeObject(Object, Paths));
				return FUnrealMcpToolResult::Success(FString::Printf(TEXT("Read object '%s'."), *Object->GetName()), Structured);
			});

		// ----------------------------------------------------------------------------------------
		// object-modify — FProperty writes on a generic UObject by path.
		// ----------------------------------------------------------------------------------------
		Registry.Tool(TEXT("object-modify"))
			.Title(TEXT("Modify Object"))
			.Description(TEXT("Write reflected properties on any UObject resolved by object path or actor label. "
			                  "Transform keys are honored when the object is an actor or scene component."))
			.ParamString(TEXT("object"), TEXT("Object path or actor label/name/path."), EUnrealMcpParamRequirement::Required)
			.Param(TEXT("properties"), TEXT("object"), TEXT("Property bag of name -> value to write."), EUnrealMcpParamRequirement::Required, MakePropertyBagSchema(TEXT("Property bag of name -> value to write.")))
			.DestructiveHint(false)
			.Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
			{
				const FString Ref = Call.GetString(TEXT("object"));
				if (Ref.IsEmpty())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'object' reference."));

				UObject* Object = FUnrealMcpObjectRef::ResolveObject(Ref, FUnrealMcpObjectRef::GetEditorWorld());
				if (!Object)
					return FUnrealMcpToolResult::Error(FString::Printf(TEXT("No object matched '%s'."), *Ref));

				TSharedPtr<FJsonObject> Props = GetPropertiesArg(Call);
				if (!Props.IsValid())
					return FUnrealMcpToolResult::Error(TEXT("Missing required 'properties' object."));

				const FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "ObjectModify", "MCP: Modify Object"));
				TArray<FString> Errors;
				const int32 Applied = FUnrealMcpPropertyJson::ApplyProperties(Object, Props, Errors);
				return MakeModifyResult(FString::Printf(TEXT("object '%s'"), *Object->GetName()), Applied, Errors);
			});
	}
}
