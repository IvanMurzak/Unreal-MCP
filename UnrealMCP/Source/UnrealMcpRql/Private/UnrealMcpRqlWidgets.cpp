// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#include "UnrealMcpRqlSession.h"
#include "UnrealMcpRqlSources.h"
#include "UnrealMcpRqlUtil.h"
#include "Tools/UnrealMcpObjectRef.h"

#include "BaseWidgetBlueprint.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Misc/EngineVersionComparison.h"

#include <vector>

/**
 * UMG widget trees (docs/rql.md "Widget Blueprints"). A Widget Blueprint's widgets live in its WidgetTree, which
 * reflection does not expose; the widgets themselves are UObjects whose properties (and `Slot`, the layout of the widget
 * in its parent panel) are reflected as usual. Adding and removing widgets changes the Blueprint's structure: it is
 * marked structurally modified (compile with `call Compile()`), and every change is undoable.
 */
namespace UnrealMcpRqlWidgetsLocal
{
	using namespace UnrealMcpRqlUtil;

	UBaseWidgetBlueprint* BlueprintOf(const UObject* Object)
	{
		return Object ? const_cast<UBaseWidgetBlueprint*>(Object->GetTypedOuter<UBaseWidgetBlueprint>()) : nullptr;
	}

	/**
	 * A widget's variable GUID, as the Designer registers it (from UE 5.6 the compiler checks every widget has one; UE 5.5
	 * widget Blueprints keep no such map).
	 */
	void RegisterVariable(UWidgetBlueprint* Blueprint, FName Name)
	{
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
		Blueprint->OnVariableAdded(Name);
#endif
	}

	void UnregisterVariable(UWidgetBlueprint* Blueprint, FName Name)
	{
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
		Blueprint->OnVariableRemoved(Name);
#endif
	}

	UWidgetTree* RequireTree(UBaseWidgetBlueprint* Blueprint)
	{
		if (!Blueprint || !Blueprint->WidgetTree)
			Throw(FString::Printf(TEXT("%s has no widget tree"), Blueprint ? *Blueprint->GetName() : TEXT("the Widget Blueprint")));
		return Blueprint->WidgetTree;
	}

	TArray<UObject*> AllWidgets(const UBaseWidgetBlueprint* Blueprint)
	{
		TArray<UObject*> Out;
		if (Blueprint && Blueprint->WidgetTree)
		{
			TArray<UWidget*> Widgets;
			Blueprint->WidgetTree->GetAllWidgets(Widgets);
			for (UWidget* Widget : Widgets)
				Out.Add(Widget);
		}
		return Out;
	}

	/**
	 * A new widget of @p ClassName named @p Name, added under @p Parent (a panel of the same tree), or as the root of an
	 * empty tree. Validates everything before creating the widget.
	 */
	UWidget* AddWidget(UBaseWidgetBlueprint* Blueprint, UWidget* Parent, const FString& ClassName, const FString& Name)
	{
		UWidgetTree* Tree = RequireTree(Blueprint);
		UClass* Class = FUnrealMcpObjectRef::ResolveClass(ClassName);
		if (!Class || !Class->IsChildOf(UWidget::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
			Throw(FString::Printf(TEXT("'%s' is not a widget class (e.g. \"Button\", \"TextBlock\", \"VerticalBox\", \"CanvasPanel\")"), *ClassName));
		UPanelWidget* Panel = nullptr;
		if (Parent)
		{
			Panel = Cast<UPanelWidget>(Parent);
			if (!Panel)
				Throw(FString::Printf(TEXT("%s is a %s, not a panel: widgets go into panels (CanvasPanel, VerticalBox, Border, ...)"),
					*Parent->GetName(), *Parent->GetClass()->GetName()));
			if (BlueprintOf(Panel) != Blueprint)
				Throw(TEXT("the parent panel belongs to another Widget Blueprint"));
			if (!Panel->CanAddMoreChildren())
				Throw(FString::Printf(TEXT("%s holds one child and already has it"), *Panel->GetName()));
		}
		else if (Tree->RootWidget)
		{
			Throw(FString::Printf(TEXT("%s already has a root widget (%s): add into a panel with AddWidget(class, name, parent)"),
				*Blueprint->GetName(), *Tree->RootWidget->GetName()));
		}
		const FString Trimmed = Name.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			if (!FName::IsValidXName(Trimmed, INVALID_OBJECTNAME_CHARACTERS))
				Throw(FString::Printf(TEXT("'%s' is not a valid widget name"), *Trimmed));
			if (Tree->FindWidget(FName(*Trimmed)) || FindObject<UObject>(Tree, *Trimmed))
				Throw(FString::Printf(TEXT("%s already has a widget named '%s'"), *Blueprint->GetName(), *Trimmed));
			// A widget is a variable of the Blueprint: no variable, function or graph may have its name either.
			if (FKismetNameValidator(Blueprint).IsValid(Trimmed) != EValidatorResult::Ok)
				Throw(FString::Printf(TEXT("%s already has a variable, function or graph named '%s'"), *Blueprint->GetName(), *Trimmed));
		}

		Blueprint->Modify();
		Tree->SetFlags(RF_Transactional);
		Tree->Modify();
		UWidget* Widget = Tree->ConstructWidget<UWidget>(Class, Trimmed.IsEmpty() ? NAME_None : FName(*Trimmed));
		if (!Widget)
			Throw(FString::Printf(TEXT("could not create a %s"), *Class->GetName()));
		UWidgetBlueprint* Designer = Cast<UWidgetBlueprint>(Blueprint);
		// A User Widget that is (or contains) this Blueprint would nest itself without end: the Designer refuses it too.
		UUserWidget* UserWidget = Cast<UUserWidget>(Widget);
		if (UserWidget && Designer && !Designer->IsWidgetFreeFromCircularReferences(UserWidget))
		{
			Widget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
			Throw(FString::Printf(TEXT("a %s inside %s would be a circular reference"), *Class->GetName(), *Blueprint->GetName()));
		}
		Widget->SetFlags(RF_Transactional);
		// The editor registers every new widget as a variable of the Blueprint (its GUID: the compiler needs it).
		if (Designer)
			RegisterVariable(Designer, Widget->GetFName());
		if (Panel)
		{
			Panel->Modify();
			if (!Panel->AddChild(Widget))
				Throw(FString::Printf(TEXT("%s did not accept the widget"), *Panel->GetName()));
		}
		else
		{
			Tree->RootWidget = Widget;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Widget;
	}

	void RemoveWidget(UWidget* Widget)
	{
		UBaseWidgetBlueprint* Blueprint = BlueprintOf(Widget);
		UWidgetTree* Tree = RequireTree(Blueprint);
		Blueprint->Modify();
		Tree->Modify();
		Widget->Modify();
		if (UPanelWidget* Parent = Widget->GetParent())
			Parent->Modify();
		// RemoveWidget detaches it from its parent panel (or clears the root) and keeps the children with it.
		// The widget and every widget under it leave the Blueprint's variables.
		TArray<UWidget*> Removed{ Widget };
		UWidgetTree::GetChildWidgets(Widget, Removed); // panel children and named-slot contents
		if (!Tree->RemoveWidget(Widget))
			Throw(FString::Printf(TEXT("%s is not in the widget tree"), *Widget->GetName()));
		UWidgetBlueprint* Designer = Cast<UWidgetBlueprint>(Blueprint);
		for (UWidget* Gone : Removed)
		{
			const FName Name = Gone->GetFName();
			// The compiler visits every widget outered to the tree: move the removed ones out, as the Designer's Delete does.
			Gone->Modify();
			Gone->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
			if (Designer)
				UnregisterVariable(Designer, Name);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	void WidgetBlueprintMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* WidgetType = T.ForClass(UWidget::StaticClass());
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		FUnrealMcpRqlTypes::SetMember(D, Member("RootWidget", WidgetType, [Types](void* Self)
		{
			const UBaseWidgetBlueprint* Blueprint = static_cast<UBaseWidgetBlueprint*>(Self);
			return Types->WrapObject(Blueprint->WidgetTree ? Blueprint->WidgetTree->RootWidget.Get() : nullptr);
		}, {}, "the root of the widget tree", /*bOwning*/ true, /*bNullable*/ true));
		FUnrealMcpRqlTypes::SetMember(D, Member("Widgets", T.Registry().list_type(WidgetType, true), [Types](void* Self)
		{
			return Types->WrapObjects(AllWidgets(static_cast<UBaseWidgetBlueprint*>(Self)));
		}, {}, "every widget of the tree (each: its reflected properties, Slot = its layout in the parent)", /*bOwning*/ false));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddWidget",
			{ FUnrealMcpRqlTypes::Param("Class", String), FUnrealMcpRqlTypes::OptionalParam("Name", String, rql::Value::string("")),
			  FUnrealMcpRqlTypes::OptionalParam("Parent", WidgetType, rql::Value::null(), /*bNullable*/ true) },
			WidgetType, [Types](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UWidget* Parent = Args[2].is_null() ? nullptr : Cast<UWidget>(Types->AsObject(Args[2]));
				if (!Args[2].is_null() && !Parent)
					ThrowMismatch(TEXT("a widget of this Blueprint"), Args[2]);
				return Types->WrapObject(AddWidget(static_cast<UBaseWidgetBlueprint*>(Self), Parent, TextArg(Args[0]), TextArg(Args[1])));
			}, "add a widget (\"TextBlock\", \"Button\", a User Widget Blueprint path) into a panel (Parent), or as the root of an empty tree; returns it"));
	}

	void WidgetMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		const rql::TypeDescriptor* WidgetType = T.ForClass(UWidget::StaticClass());
		const rql::TypeDescriptor* String = rql::builtin::string_type();
		FUnrealMcpRqlTypes::SetMember(D, Member("Parent", T.ForClass(UPanelWidget::StaticClass()),
			[Types](void* Self) { return Types->WrapObject(static_cast<UWidget*>(Self)->GetParent()); }, {},
			"the panel holding this widget (null for the root)", false, true));
		FUnrealMcpRqlTypes::SetMember(D, Member("WidgetBlueprint", T.ForClass(UBaseWidgetBlueprint::StaticClass()),
			[Types](void* Self) { return Types->WrapObject(BlueprintOf(static_cast<UWidget*>(Self))); }, {}, "", false, true));
		FUnrealMcpRqlTypes::AddMethod(D, Method("Destroy", {}, nullptr, [](void* Self, const std::vector<rql::Value>&) -> rql::Value
		{
			UWidget* Widget = static_cast<UWidget*>(Self);
			if (!BlueprintOf(Widget))
				Throw(TEXT("only widgets of a Widget Blueprint's tree can be removed (not of a running widget)"));
			RemoveWidget(Widget);
			return rql::Value::null();
		}, "remove the widget (and its children) from the tree"));
		FUnrealMcpRqlTypes::AddMethod(D, Method("AddChild",
			{ FUnrealMcpRqlTypes::Param("Class", String), FUnrealMcpRqlTypes::OptionalParam("Name", String, rql::Value::string("")) },
			WidgetType, [Types](void* Self, const std::vector<rql::Value>& Args) -> rql::Value
			{
				UWidget* Widget = static_cast<UWidget*>(Self);
				UBaseWidgetBlueprint* Blueprint = BlueprintOf(Widget);
				if (!Blueprint)
					Throw(TEXT("only widgets of a Widget Blueprint's tree can take children here"));
				return Types->WrapObject(AddWidget(Blueprint, Widget, TextArg(Args[0]), TextArg(Args[1])));
			}, "add a widget of a class into this panel; returns it"));
	}

	void PanelMembers(FUnrealMcpRqlTypes& T, rql::TypeDescriptor& D)
	{
		FUnrealMcpRqlTypes* Types = &T;
		FUnrealMcpRqlTypes::SetMember(D, Member("Children", T.Registry().list_type(T.ForClass(UWidget::StaticClass()), true),
			[Types](void* Self)
			{
				TArray<UObject*> Out;
				for (UWidget* Child : static_cast<UPanelWidget*>(Self)->GetAllChildren())
					Out.Add(Child);
				return Types->WrapObjects(Out);
			}, {}, "the widgets in this panel, in order", /*bOwning*/ true));
	}
}

void UnrealMcpRqlAddWidgets(FUnrealMcpRqlSession& Session)
{
	using namespace UnrealMcpRqlWidgetsLocal;
	FUnrealMcpRqlTypes& T = Session.Types();
	FUnrealMcpRqlTypes* Types = &T;
	T.AddClassHook(UBaseWidgetBlueprint::StaticClass(), &WidgetBlueprintMembers);
	T.AddClassHook(UWidget::StaticClass(), &WidgetMembers);
	T.AddClassHook(UPanelWidget::StaticClass(), &PanelMembers);
	Session.AddSource(TEXT("widgetBlueprints"), T.ForClass(UBaseWidgetBlueprint::StaticClass()), /*bList*/ true,
		TEXT("the project's Widget Blueprints (RootWidget, Widgets; call AddWidget / Compile); panels list Children, widgets have Parent and Slot"),
		[Types]() { return UnrealMcpRqlAssetList(*Types, UBaseWidgetBlueprint::StaticClass(), FString()); });
	// The widgets of a Widget Blueprint are templates (archetypes of the generated class's widget tree) and its design
	// data: editable like the Designer edits them, unlike other class defaults.
	Session.AllowTemplateWrites([](const UObject* Object) { return BlueprintOf(Object) != nullptr; });
	// A widget's properties are its Blueprint's design: an edit marks the Blueprint modified (compile to apply it).
	Session.AddCommitted([Types](const std::vector<rql::MutationRecord>& Records)
	{
		TSet<UBaseWidgetBlueprint*> Touched;
		for (const rql::MutationRecord& Record : Records)
		{
			UObject* Owner = Types->AsObject(Record.owner);
			if (UBaseWidgetBlueprint* Blueprint = Owner && !Record.method ? BlueprintOf(Owner) : nullptr)
				Touched.Add(Blueprint);
		}
		for (UBaseWidgetBlueprint* Blueprint : Touched)
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	});
}
