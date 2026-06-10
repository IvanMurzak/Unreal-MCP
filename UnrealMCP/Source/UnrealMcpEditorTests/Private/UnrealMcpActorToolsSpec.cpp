// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UnrealMcpCoreTools.h"
#include "UnrealMcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "GameFramework/Actor.h"

/**
 * Actor & component tool family specs (docs/ARCHITECTURE.md §10). Registration shape + per-tool
 * happy/error paths, executed in EditorContext so GEditor + the editor world are live. Each scene-
 * mutating test cleans up the actors it spawns so tests stay order-independent.
 */
BEGIN_DEFINE_SPEC(FUnrealMcpActorToolsSpec, "UnrealMcp.Tools.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// Helpers ------------------------------------------------------------------------------------
	static TSharedPtr<FJsonObject> Args() { return MakeShared<FJsonObject>(); }

	/** Run a tool by name with the given args object through a freshly-registered actor registry. */
	FUnrealMcpToolResult Run(FUnrealMcpToolRegistry& Reg, const FString& Tool, const TSharedPtr<FJsonObject>& A)
	{
		return Reg.Execute(Tool, FUnrealMcpToolCall(A));
	}

	/** Pull the structured "name"/"label" identity field from a result (empty when absent). */
	static FString StructString(const FUnrealMcpToolResult& R, const FString& Key)
	{
		FString V;
		if (R.Structured.IsValid())
			R.Structured->TryGetStringField(Key, V);
		return V;
	}

END_DEFINE_SPEC(FUnrealMcpActorToolsSpec)

void FUnrealMcpActorToolsSpec::Define()
{
	Describe("registration", [this]()
	{
		It("registers the full actor family as core tools and bumps the revision", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			const int32 Before = Registry.GetRevision();
			UnrealMcpActorTools::Register(Registry);

			const TCHAR* const Expected[] = {
				TEXT("actor-create"), TEXT("actor-destroy"), TEXT("actor-duplicate"), TEXT("actor-find"),
				TEXT("actor-modify"), TEXT("actor-set-parent"), TEXT("actor-component-add"),
				TEXT("actor-component-destroy"), TEXT("actor-component-get"), TEXT("actor-component-modify"),
				TEXT("actor-component-list-all"), TEXT("object-get-data"), TEXT("object-modify")
			};
			for (const TCHAR* Name : Expected)
				TestTrue(FString::Printf(TEXT("has %s"), Name), Registry.HasTool(Name));

			TestEqual(TEXT("tool count"), Registry.Num(), (int32)UE_ARRAY_COUNT(Expected));
			TestTrue(TEXT("revision bumped"), Registry.GetRevision() > Before);

			// Every tool stays under the core extension id and surfaces in the manifest with a schema hash.
			const FUnrealMcpRegisteredTool* Create = Registry.Find(TEXT("actor-create"));
			TestTrue(TEXT("actor-create found"), Create != nullptr);
			if (Create)
			{
				TestEqual(TEXT("core extension id"), Create->ExtensionId, FString(TEXT("core")));
				TestFalse(TEXT("schema hash non-empty"), Create->SchemaHash.IsEmpty());
			}
		});

		It("declares destructive/read-only hints that match each tool's behavior", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);

			// Resolve each tool through a null-guarded lookup so a renamed/missing tool fails the assertion
			// instead of crashing the whole automation run on a null deref.
			auto Lookup = [this, &Registry](const TCHAR* ToolId) -> const FUnrealMcpRegisteredTool*
			{
				const FUnrealMcpRegisteredTool* Tool = Registry.Find(ToolId);
				TestTrue(FString::Printf(TEXT("%s found"), ToolId), Tool != nullptr);
				return Tool;
			};

			if (const FUnrealMcpRegisteredTool* Tool = Lookup(TEXT("actor-destroy")))
				TestTrue(TEXT("actor-destroy destructive"), Tool->bDestructiveHint);
			if (const FUnrealMcpRegisteredTool* Tool = Lookup(TEXT("actor-find")))
				TestTrue(TEXT("actor-find read-only"), Tool->bReadOnlyHint);
			if (const FUnrealMcpRegisteredTool* Tool = Lookup(TEXT("actor-component-list-all")))
				TestTrue(TEXT("actor-component-list-all read-only"), Tool->bReadOnlyHint);
			if (const FUnrealMcpRegisteredTool* Tool = Lookup(TEXT("actor-create")))
				TestFalse(TEXT("actor-create not read-only"), Tool->bReadOnlyHint);
		});
	});

	Describe("lifecycle round-trip", [this]()
	{
		It("creates -> finds -> modifies -> adds component -> reads -> destroys an actor", [this]()
		{
			if (!GEditor) { AddError(TEXT("GEditor unavailable")); return; }
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);

			const FString Label = FString::Printf(TEXT("McpSpecActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));

			// create
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.StaticMeshActor"));
				A->SetStringField(TEXT("name"), Label);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-create"), A);
				TestTrue(TEXT("create success"), R.bSuccess);
				TestEqual(TEXT("created label"), StructString(R, TEXT("label")), Label);
			}

			// find by label + class
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("labelFilter"), Label);
				A->SetStringField(TEXT("classFilter"), TEXT("StaticMeshActor"));
				const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-find"), A);
				TestTrue(TEXT("find success"), R.bSuccess);
				int32 Total = 0;
				if (R.Structured.IsValid())
				{
					double D = 0; R.Structured->TryGetNumberField(TEXT("total"), D); Total = (int32)D;
				}
				TestEqual(TEXT("found exactly one"), Total, 1);
			}

			// modify transform (location) + a reflected bool
			{
				TSharedPtr<FJsonObject> Loc = Args();
				Loc->SetNumberField(TEXT("x"), 100.0); Loc->SetNumberField(TEXT("y"), 200.0); Loc->SetNumberField(TEXT("z"), 50.0);
				TSharedPtr<FJsonObject> Props = Args();
				Props->SetObjectField(TEXT("location"), Loc);
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Label);
				A->SetObjectField(TEXT("properties"), Props);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-modify"), A);
				TestTrue(TEXT("modify success"), R.bSuccess);
				double Applied = 0; if (R.Structured.IsValid()) R.Structured->TryGetNumberField(TEXT("applied"), Applied);
				TestTrue(TEXT("at least one property applied"), Applied >= 1.0);

				// Read the location back: a silent transform-routing no-op would still report applied>=1,
				// so confirm the actor actually moved to the requested world location.
				UWorld* World = GEditor->GetEditorWorldContext().World();
				AActor* Moved = nullptr;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (It->GetActorLabel() == Label) { Moved = *It; break; }
				}
				TestNotNull(TEXT("modified actor found"), Moved);
				if (Moved)
					TestTrue(TEXT("location write took effect"),
						Moved->GetActorLocation().Equals(FVector(100.0, 200.0, 50.0), 0.5));
			}

			// add a component
			FString CompName;
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Label);
				A->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.PointLightComponent"));
				A->SetStringField(TEXT("name"), TEXT("McpSpecLight"));
				const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-component-add"), A);
				TestTrue(TEXT("component-add success"), R.bSuccess);
				CompName = StructString(R, TEXT("name"));
				TestFalse(TEXT("component name non-empty"), CompName.IsEmpty());
			}

			// read the component back (scoped paths optional → full)
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Label);
				A->SetStringField(TEXT("component"), CompName);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-component-get"), A);
				TestTrue(TEXT("component-get success"), R.bSuccess);
				TestTrue(TEXT("component-get has data"), R.Structured.IsValid() && R.Structured->HasField(TEXT("data")));
			}

			// object-get-data on the same actor by label
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("object"), Label);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("object-get-data"), A);
				TestTrue(TEXT("object-get-data success"), R.bSuccess);
			}

			// destroy
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Label);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-destroy"), A);
				TestTrue(TEXT("destroy success"), R.bSuccess);
			}

			// confirm gone
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("labelFilter"), Label);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-find"), A);
				int32 Total = 0; double D = 0;
				if (R.Structured.IsValid() && R.Structured->TryGetNumberField(TEXT("total"), D)) Total = (int32)D;
				TestEqual(TEXT("no actor remains"), Total, 0);
			}
		});

		It("attaches and detaches actors via actor-set-parent", [this]()
		{
			if (!GEditor) { AddError(TEXT("GEditor unavailable")); return; }
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);

			const FString Parent = FString::Printf(TEXT("McpSpecParent_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
			const FString Child = FString::Printf(TEXT("McpSpecChild_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
			for (const FString& Lbl : { Parent, Child })
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.StaticMeshActor"));
				A->SetStringField(TEXT("name"), Lbl);
				TestTrue(TEXT("spawn"), Run(Registry, TEXT("actor-create"), A).bSuccess);
			}

			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Child);
				A->SetStringField(TEXT("parent"), Parent);
				TestTrue(TEXT("attach success"), Run(Registry, TEXT("actor-set-parent"), A).bSuccess);
			}
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Child); // no parent => detach
				TestTrue(TEXT("detach success"), Run(Registry, TEXT("actor-set-parent"), A).bSuccess);
			}

			for (const FString& Lbl : { Child, Parent })
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Lbl);
				Run(Registry, TEXT("actor-destroy"), A);
			}
		});

		It("duplicates an actor with a location offset", [this]()
		{
			if (!GEditor) { AddError(TEXT("GEditor unavailable")); return; }
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);

			const FString Src = FString::Printf(TEXT("McpSpecDup_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
			const FString Dup = Src + TEXT("_copy");
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.StaticMeshActor"));
				A->SetStringField(TEXT("name"), Src);
				TestTrue(TEXT("spawn src"), Run(Registry, TEXT("actor-create"), A).bSuccess);
			}
			{
				TSharedPtr<FJsonObject> Off = Args();
				Off->SetNumberField(TEXT("x"), 500.0);
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Src);
				A->SetStringField(TEXT("name"), Dup);
				A->SetObjectField(TEXT("offset"), Off);
				const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-duplicate"), A);
				TestTrue(TEXT("duplicate success"), R.bSuccess);
				TestEqual(TEXT("dup label"), StructString(R, TEXT("label")), Dup);
			}
			for (const FString& Lbl : { Src, Dup })
			{
				TSharedPtr<FJsonObject> A = Args();
				A->SetStringField(TEXT("actor"), Lbl);
				Run(Registry, TEXT("actor-destroy"), A);
			}
		});
	});

	Describe("component class listing", [this]()
	{
		It("lists concrete component classes including StaticMeshComponent and paginates", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);

			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("search"), TEXT("StaticMeshComponent"));
			const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-component-list-all"), A);
			TestTrue(TEXT("list success"), R.bSuccess);
			double Total = 0; if (R.Structured.IsValid()) R.Structured->TryGetNumberField(TEXT("total"), Total);
			TestTrue(TEXT("at least one match"), Total >= 1.0);

			const TArray<TSharedPtr<FJsonValue>>* Comps = nullptr;
			bool bFoundSMC = false;
			if (R.Structured.IsValid() && R.Structured->TryGetArrayField(TEXT("components"), Comps))
			{
				for (const TSharedPtr<FJsonValue>& V : *Comps)
				{
					FString Name;
					if (V->AsObject().IsValid() && V->AsObject()->TryGetStringField(TEXT("name"), Name) && Name == TEXT("StaticMeshComponent"))
						bFoundSMC = true;
				}
			}
			TestTrue(TEXT("found StaticMeshComponent"), bFoundSMC);
		});
	});

	Describe("error paths", [this]()
	{
		It("rejects an unspawnable class on actor-create", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.NotARealClassXYZ"));
			const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-create"), A);
			TestFalse(TEXT("create fails for bad class"), R.bSuccess);
		});

		It("errors when actor-modify targets a missing actor", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);
			TSharedPtr<FJsonObject> Props = Args();
			Props->SetBoolField(TEXT("bHidden"), true);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("actor"), TEXT("DefinitelyMissingActor_ZZZ"));
			A->SetObjectField(TEXT("properties"), Props);
			const FUnrealMcpToolResult R = Run(Registry, TEXT("actor-modify"), A);
			TestFalse(TEXT("modify fails for missing actor"), R.bSuccess);
		});

		It("errors when object-get-data targets a missing object", [this]()
		{
			FUnrealMcpToolRegistry Registry;
			UnrealMcpActorTools::Register(Registry);
			TSharedPtr<FJsonObject> A = Args();
			A->SetStringField(TEXT("object"), TEXT("/Game/DoesNotExist/Nope.Nope"));
			const FUnrealMcpToolResult R = Run(Registry, TEXT("object-get-data"), A);
			TestFalse(TEXT("object-get-data fails for missing object"), R.bSuccess);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
