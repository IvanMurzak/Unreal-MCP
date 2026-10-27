// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "UnrealMcpRqlInclude.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class FProperty;
class UClass;
class UEnum;
class UFunction;
class UObject;
class UScriptStruct;
class UStruct;

/** Bracket a UFUNCTION call made by RQL: UnrealMcpRql::Fail() inside it makes EndCall return true with the message. */
void UnrealMcpRqlBeginCall();
bool UnrealMcpRqlEndCall(FString& OutFailure);

/** FString <-> UTF-8 std::string (RQL text is UTF-8). */
std::string UnrealMcpRqlToStd(const FString& Text);
FString UnrealMcpRqlFromStd(const std::string& Text);

/**
 * The Unreal provider of the RQL C++ runtime (RQL cpp/README.md "Provider seam"): type descriptors built at runtime from
 * UClass / UScriptStruct / UEnum / FProperty / UFunction, TArray / TSet / TMap adapters and UObject liveness.
 *
 * Descriptors are keyed by the reflected type (the UStruct / UEnum pointer is the RQL type key) and filled on first use:
 * reflection reaches thousands of classes, a query touches a few. Members and methods of a class are filled before the
 * first object of it becomes a value, so the engine only ever reads complete descriptors. UObject descriptors carry no
 * RQL type adapter (adapted views are cached per type), which is what makes the lazy fill safe.
 *
 * Exposed: class properties that are EditAnywhere / VisibleAnywhere / BlueprintVisible (writable unless EditConst or
 * BlueprintReadOnly-only), every struct property, plus computed members registered by hooks (Id, Name, Path on every
 * object, the actor transform, components, children, …). Callable: CallInEditor functions, every UFUNCTION of an API
 * class, and the methods hooks add. Game-thread only.
 */
class FUnrealMcpRqlTypes
{
public:
	/** Adds members / methods to the descriptor of one class (and, by inheritance, of its subclasses). */
	using FClassHook = TFunction<void(FUnrealMcpRqlTypes& Types, rql::TypeDescriptor& Descriptor)>;
	/**
	 * A text / list form of one struct for CoerceStruct (gameplay tags: "A.B.C", ["A.B", "C"]): true when it handled the
	 * value and wrote @p Out; false to fall through; throws std::runtime_error when the value has the form but does not fit.
	 */
	using FStructParser = TFunction<bool(FUnrealMcpRqlTypes& Types, const rql::Value& Value, void* Out)>;

	FUnrealMcpRqlTypes();
	~FUnrealMcpRqlTypes();

	rql::TypeRegistry& Registry() { return *RegistryPtr; }

	// ---- descriptors

	const rql::TypeDescriptor* ForClass(UClass* Class);
	const rql::TypeDescriptor* ForStruct(UScriptStruct* Struct);
	const rql::TypeDescriptor* ForEnum(UEnum* Enum);
	/** The declared RQL type of a property value; nullptr = unsupported (the property is skipped). */
	const rql::TypeDescriptor* ForProperty(const FProperty* Property, bool& bOutNullable);
	/** Reflected (or registered native) type by short name ("StaticMeshActor", "Vector", "LogEntry"); U/A/F/E prefixes tolerated. */
	const rql::TypeDescriptor* FindByName(const FString& Name);
	/**
	 * Whether some type has a member of this name (case-insensitive): a property of any loaded class or struct that the fill
	 * would expose (component and actor classes, Blueprint classes, user-defined struct fields by their authored name) or a
	 * member a descriptor gets from its hooks (hook members
	 * such as Label or Scalars; the hooked types are filled to find them). RQL's Engine::is_known_member: a mutation that a
	 * read of such a name emptied is a polymorphic no-match (ok, nothing changed), not a typo. Answers are cached per session.
	 */
	bool IsKnownMemberName(const FString& Name);

	/** Make sure a (possibly lazily created) descriptor has its members and methods. */
	void Fill(const rql::TypeDescriptor* Descriptor);
	UClass* ClassOf(const rql::TypeDescriptor* Descriptor) const;
	bool IsObjectType(const rql::TypeDescriptor* Descriptor) const { return ClassOf(Descriptor) != nullptr; }

	// ---- values

	/** The object as an RQL value of its most-derived class (null for nullptr); holds a weak pointer for liveness. */
	rql::Value WrapObject(UObject* Object);
	rql::Value WrapObjects(const TArray<UObject*>& Objects);
	/** A copy of struct memory as an RQL struct value. */
	rql::Value WrapStruct(UScriptStruct* Struct, const void* Data);
	/** The UObject behind an object value; nullptr for null and non-UObject values. */
	UObject* AsObject(const rql::Value& Value) const;
	/** Liveness without dereferencing a destroyed object (the weak pointer the value carries). */
	bool IsAlive(const rql::Value& Value) const;
	/**
	 * The session-independent id of an object (the `Id` member, stable ref "Type#<id>"): its object-array index and the
	 * slot's serial number, so a ref to a destroyed object never resolves to another object that reuses the slot.
	 */
	static int64 IdOf(const UObject* Object);
	/** The live object with this id (IdOf); nullptr when it is gone. No search. */
	static UObject* FindById(int64 Id);

	/** The RQL value of a property value. @p Type: the property's ForProperty type when the caller has it (else computed). */
	rql::Value Read(const FProperty* Property, const void* ValuePtr, const rql::TypeDescriptor* Type = nullptr);
	/** Convert and store; throws std::runtime_error with a message for the agent when the value does not fit. */
	void Write(const FProperty* Property, void* ValuePtr, const rql::Value& Value);
	/** Write a property of an object with the editor notifications (PreEditChange / PostEditChangeProperty). */
	void WriteObjectProperty(UObject* Object, FProperty* Property, const rql::Value& Value);
	/**
	 * @p Object, or the component that replaced it during this call: an edit of a Blueprint actor's component reruns the
	 * construction script, which rebuilds the construction-script components, so a stream item can point at the old one.
	 */
	UObject* LiveObject(UObject* Object) const;
	/**
	 * The one struct coercion (property writes, method arguments, the math-struct adapters): a struct value of the type, a
	 * math-struct literal ("(1, 2, 3)", [1, 2, 3], "#ff8800") or a {Member: value} literal written over @p Out. Throws
	 * std::runtime_error when the value does not fit.
	 */
	void CoerceStruct(UScriptStruct* Struct, const rql::Value& Value, void* Out);
	/**
	 * The engine's own text form of a reflected struct (ExportText / the editor's Copy, e.g. FAnchors
	 * "(Minimum=(X=0.5,Y=0),Maximum=(X=0.5,Y=0))") as a struct value: RQL's MutationPolicy::coerce_text for structs that no
	 * adapter coerces. nullopt when @p Type is not a reflected struct or the text is not a "(...)" form; @p OutReason set
	 * when it is one that does not import.
	 */
	std::optional<rql::Value> ParseStructText(const rql::TypeDescriptor& Type, const std::string& Text, std::string& OutReason);

	// ---- extension points (registered before the first query; inherited by subclasses)

	void AddClassHook(UClass* Class, FClassHook Hook);
	/** Reshape the members of one struct after its reflected members are filled (exact struct, no inheritance). */
	void AddStructHook(UScriptStruct* Struct, FClassHook Hook);
	void AddStructParser(UScriptStruct* Struct, FStructParser Parser);
	/**
	 * A mutable descriptor of a host-defined object type (no UClass behind it, e.g. a Blueprint graph pin), keyed by
	 * @p Key, registered so FindByName and rql-schema know it. Values: rql::Value::object(descriptor, ptr, keep_alive).
	 */
	rql::TypeDescriptor& NativeObjectType(const void* Key, const FString& Name, const FString& Description);
	void AddApiClass(UClass* Class, const FString& TypeName);
	/** Whether @p Class was registered with AddApiClass (its class default object is an operations object). */
	bool IsApiClass(const UClass* Class) const { return Class && ApiClasses.Contains(const_cast<UClass*>(Class)); }
	void ExposeProperties(UStruct* Owner, const TArray<FName>& Names, bool bWritable);

	/** Add or replace a member of a descriptor (hooks use it). */
	static void SetMember(rql::TypeDescriptor& Descriptor, rql::MemberDescriptor Member);
	static void AddMethod(rql::TypeDescriptor& Descriptor, rql::MethodDescriptor Method);
	static rql::ParamDescriptor Param(const char* Name, const rql::TypeDescriptor* Type, bool bNullable = false);
	static rql::ParamDescriptor OptionalParam(const char* Name, const rql::TypeDescriptor* Type, rql::Value Default, bool bNullable = false);

	/** Native (non-reflected) descriptors registered by the host (log entries, class infos), for FindByName. */
	void AddNativeType(const rql::TypeDescriptor* Descriptor);

private:
	std::unique_ptr<rql::TypeRegistry> RegistryPtr;

	TMap<UClass*, rql::TypeDescriptor*> Classes;
	TMap<UScriptStruct*, rql::TypeDescriptor*> Structs;
	TMap<UEnum*, rql::TypeDescriptor*> Enums;
	TMap<const rql::TypeDescriptor*, UClass*> ClassByDescriptor;
	TMap<const rql::TypeDescriptor*, UScriptStruct*> StructByDescriptor;
	TSet<const rql::TypeDescriptor*> Filled;
	TArray<const rql::TypeDescriptor*> NativeTypes;

	TMultiMap<UClass*, FClassHook> Hooks;
	TMultiMap<UScriptStruct*, FClassHook> StructHooks;
	TMap<UScriptStruct*, FStructParser> StructParsers;
	TMap<UClass*, FString> ApiClasses;
	TMap<UStruct*, TMap<FName, bool>> Exposed;
	TMap<FString, bool> KnownMemberNames; // IsKnownMemberName; FString keys compare case-insensitively
	TMap<const UObject*, TWeakObjectPtr<UObject>> Replaced; // LiveObject: rebuilt component -> its replacement

	rql::TypeDescriptor& Shell(const void* Key, const FString& Name, rql::TypeKind Kind);
	void FillClass(rql::TypeDescriptor& Descriptor, UClass* Class);
	void FillStruct(rql::TypeDescriptor& Descriptor, UScriptStruct* Struct);
	void AddPropertyMember(rql::TypeDescriptor& Descriptor, FProperty* Property, bool bOnObject, bool bForceWritable);
	void AddFunctionMethod(rql::TypeDescriptor& Descriptor, UFunction* Function);
	bool IsCallable(UClass* Class, UFunction* Function) const;
};
