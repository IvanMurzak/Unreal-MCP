// Provider-based reflection for the RQL C++ runtime.
//
// Plain C++ has no runtime reflection, so hosts DESCRIBE their types explicitly:
//
//   rql::TypeRegistry reg;
//   reg.enumeration<NodeKind>("NodeKind").value("Alpha", NodeKind::Alpha);
//   reg.structure<Vec2>("Vec2").member("x", &Vec2::x).member("y", &Vec2::y);
//   reg.object<Node>("Node").member("Id", &Node::Id).readonly().member("Parent", &Node::Parent).reference();
//   reg.object<Special>("Special").base<Node>().member("Power", &Special::Power);
//
// Everything the executor needs goes through TypeDescriptor / MemberDescriptor (type-erased
// getters/setters over void*) and the IListAdapter / IMapAdapter interfaces. A different
// provider (e.g. an Unreal adapter building descriptors from UClass/FProperty and adapters over
// TArray/TMap) only has to fill the same structures — see cpp/README.md "Provider seam".
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rql/json.hpp"
#include "rql/value.hpp"

namespace rql {

/// RTTI-free compile-time type key: the address of a per-type tag (unique per program image; in a
/// multi-module build register and query types from the same module).
using TypeKey = const void*;
namespace detail {
template <class T>
struct TypeKeyTag {
    static constexpr char id = 0;
};
/// Detects an optional `TypeKey rql_dynamic_type() const` (usually virtual) on a class.
template <class T, class = void>
struct has_rql_dynamic_type : std::false_type {};
template <class T>
struct has_rql_dynamic_type<T, std::void_t<decltype(std::declval<const T&>().rql_dynamic_type())>> : std::true_type {};
}  // namespace detail
template <class T>
constexpr TypeKey type_key() noexcept {
    return &detail::TypeKeyTag<std::remove_cv_t<T>>::id;
}

enum class TypeKind : uint8_t { Scalar, Enum, Object, Struct, List, Map };
enum class ScalarKind : uint8_t { None, Bool, Integer, Single, Double, String, Any };

using Getter = std::function<Value(void* self)>;
using Setter = std::function<void(void* self, const Value& value)>;
/// Validation of a coerced value before any write: return a rejection reason, or empty to accept.
using Validator = std::function<std::string(void* self, const Value& value)>;

struct MemberDescriptor {
    std::string name;
    const TypeDescriptor* type = nullptr;
    bool nullable = false;   // optional<T>, pointers, smart pointers
    bool owning = true;      // false: non-owning reference (not followed by generic recurse)
    bool readonly = false;   // never writable through RQL
    Getter getter;
    Setter setter;           // empty: not writable
    std::function<bool()> assignable;  // optional runtime check (e.g. owning pointers are never assignable)
    std::string description;
    bool explicit_only = false;  // read only when a path names it (adapters, spec §13)
    bool copy_on_read = false;   // getter returns a copy: writes into it are assigned back via the setter (spec §13)
    /// Adapter validation before any write: returns a rejection reason, empty = accept.
    Validator validate;

    bool has_setter() const { return setter && !readonly && (!assignable || assignable()); }
};

struct ParamDescriptor {
    std::string name;
    const TypeDescriptor* type = nullptr;
    bool nullable = false;
    bool has_default = false;
    Value default_value;
};

/// Check of a planned call (target, coerced arguments, defaults filled in): a rejection reason, or empty to accept.
using CallValidator = std::function<std::string(void* self, const std::vector<Value>& args)>;

struct MethodDescriptor {
    std::string name;
    std::vector<ParamDescriptor> params;
    const TypeDescriptor* result = nullptr;  // nullptr = void
    std::function<Value(void* self, const std::vector<Value>& args)> invoke;
    bool callable = true;  // false: known but not allowlisted (call → permission-denied)
    std::string description;
    /// Optional (empty = no check). `call` runs it for every target before the first call, on dry runs too:
    /// a rejection (or a throwing check) fails the stage with invalid-value and nothing is called (spec §8).
    CallValidator validate;
};

class TypeDescriptor {
public:
    std::string name;         // short runtime name ($type, `is`, $ref prefix)
    std::string friendly;     // C#-like display name for scalars ("int", "string"); lists/maps compute it
    std::string description;
    TypeKind kind = TypeKind::Object;
    ScalarKind scalar = ScalarKind::None;
    double int_min = 0, int_max = 0;  // integral scalar range
    bool registered = false;

    // inheritance (single chain); casts convert between this type's pointer and the base's pointer
    const TypeDescriptor* base = nullptr;
    void* (*to_base)(void*) = nullptr;
    void* (*from_base)(void*) = nullptr;
    std::vector<std::string> also_is;  // extra names for `is` (interfaces)

    std::vector<MemberDescriptor> members;  // inherited first, declaration order
    std::vector<MethodDescriptor> methods;
    std::string id_name, display_name;
    bool id_explicit = false, display_explicit = false;

    std::vector<std::pair<std::string, int64_t>> enum_values;

    const TypeDescriptor* element = nullptr;  // list element / map value
    bool element_nullable = false;
    const TypeDescriptor* key = nullptr;      // map key

    std::function<std::shared_ptr<void>(const void*)> copy;  // struct copy
    /// Default construction (object literals, spec §4.5): a new instance typed as THIS descriptor's type.
    /// Structs get one automatically; objects opt in with ClassBuilder::constructible().
    std::function<std::shared_ptr<void>()> create;
    /// `create` returns host-owned objects (ClassBuilder::factory): the engine never retains them.
    bool host_owned = false;

    /// Native type key (class/enum descriptors).
    TypeKey native_key = nullptr;
    /// Optional dynamic-type hook for polymorphic hierarchies (no RTTI): given a pointer typed as THIS
    /// descriptor's type, return the type key of the most-derived object. Inherited by subtypes.
    std::function<TypeKey(void* self)> dynamic_key;

    /// Exact (ordinal) name first, then a unique case-insensitive match.
    const MemberDescriptor* find(const std::string& member) const;
    /// Self, bases and also_is names, case-insensitive.
    bool is_a(const std::string& type_name) const;
    /// Whether this type or a base lists `interface_name` in also_is (case-insensitive).
    bool implements(const std::string& interface_name) const;
    bool derives_from(const TypeDescriptor* other) const;
    /// Convert a pointer of this type to `target` (a base or self); nullptr if unrelated.
    void* upcast(void* p, const TypeDescriptor* target) const;
    /// Convert a pointer typed as `base_type` (a base of this) to this type; nullptr if unrelated.
    void* downcast_from(const TypeDescriptor* base_type, void* p) const;

    const MemberDescriptor* id_member() const { return id_member_in(members); }  // [explicit] or "Id" of integer/string type
    const MemberDescriptor* display_member() const { return display_member_in(members); }  // [explicit] or "Name" of string type
    /// Same rules applied to another (e.g. adapter-shaped) member list of this type.
    const MemberDescriptor* id_member_in(const std::vector<MemberDescriptor>& list) const;
    const MemberDescriptor* display_member_in(const std::vector<MemberDescriptor>& list) const;
    /// Identity members are always read-only through RQL.
    bool member_writable(const MemberDescriptor& m) const;

    bool enum_name(int64_t v, std::string& out) const;
    bool enum_parse(const std::string& text, int64_t& out) const;  // case-insensitive name or integer text
    bool enum_defined(int64_t v) const;
    std::string enum_names() const;  // "A, B, C"

    std::string friendly_name() const;
    std::string signature(const MethodDescriptor& m) const;
    bool is_value_type() const { return kind == TypeKind::Scalar || kind == TypeKind::Enum || kind == TypeKind::Struct; }
};

/// Exact (ordinal) name first, then a unique case-insensitive match; nullptr otherwise.
const MemberDescriptor* find_member_in(const std::vector<MemberDescriptor>& members, const std::string& name);
/// Writable through RQL: has a setter, not read-only, and not the identity member (always read-only).
inline bool member_writable(const MemberDescriptor& m, const MemberDescriptor* id) { return m.has_setter() && &m != id; }
inline bool TypeDescriptor::member_writable(const MemberDescriptor& m) const { return rql::member_writable(m, id_member()); }
/// The value already has `type`: struct/enum of that type, an object of it or a subtype, or a scalar of the
/// same kind (numbers: the same native scalar type; untyped literals count as double). Bypasses coercion.
bool is_instance_of(const Value& v, const TypeDescriptor* type);

/// Built-in scalar descriptors (process-wide).
namespace builtin {
const TypeDescriptor* bool_type();
const TypeDescriptor* string_type();
const TypeDescriptor* double_type();
const TypeDescriptor* single_type();
const TypeDescriptor* any_type();
const TypeDescriptor* number_type(bool integral, bool is_signed, size_t size);
const TypeDescriptor* entry_type();  // MapEntry (members Key, Value)
const TypeDescriptor* row_type();    // RowValue
template <class T>
const TypeDescriptor* number_of() {
    if constexpr (std::is_floating_point_v<T>) return sizeof(T) == 4 ? single_type() : double_type();
    else return number_type(true, std::is_signed_v<T>, sizeof(T));
}
}  // namespace builtin

class TypeRegistry;
template <class T> class ClassBuilder;
template <class T> class EnumBuilder;

// ---------------------------------------------------------------- Traits: native <-> Value

/// Primary template: registered class types (object or struct). Specializations below cover
/// scalars, std::string, enums, optional, raw/smart pointers, vector and (unordered_)map.
template <class T, class Enable = void>
struct Traits {
    static const TypeDescriptor* type(TypeRegistry& r);
    static constexpr bool nullable = false;
    static bool assignable(TypeRegistry& r);
    static Value get(TypeRegistry& r, T& v);
    static void set(TypeRegistry& r, T& dst, const Value& v);
    static bool key_from(TypeRegistry&, const Value&, T&) { return false; }
};

class TypeRegistry {
public:
    TypeRegistry() = default;
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    template <class T> ClassBuilder<T> object(std::string name);
    template <class T> ClassBuilder<T> structure(std::string name);
    template <class T> EnumBuilder<T> enumeration(std::string name);

    /// Descriptor for a class/enum type (placeholder until registered).
    template <class T>
    TypeDescriptor* descriptor() {
        return descriptor_for(type_key<T>());
    }
    TypeDescriptor* descriptor_for(TypeKey t);
    const TypeDescriptor* find_native(TypeKey t) const;
    const TypeDescriptor* list_type(const TypeDescriptor* element, bool element_nullable);
    const TypeDescriptor* map_type(const TypeDescriptor* key, const TypeDescriptor* value, bool value_nullable);
    /// Registered object/struct/enum descriptors in registration order.
    std::vector<const TypeDescriptor*> registered() const;
    size_t registered_count() const noexcept { return order_.size(); }

    /// Wrap a native pointer as an object Value with its most-derived registered type.
    template <class T>
    Value wrap(T* p, std::shared_ptr<void> keep_alive = {}) {
        using U = std::remove_cv_t<T>;
        if (!p) return Value::null();
        TypeDescriptor* d = descriptor<U>();
        void* ptr = const_cast<void*>(static_cast<const void*>(p));
        // Most-derived type without RTTI: a `rql_dynamic_type()` member, else the nearest dynamic_type hook.
        TypeKey dyn_key = nullptr;
        if constexpr (detail::has_rql_dynamic_type<U>::value) {
            dyn_key = p->rql_dynamic_type();
        } else {
            for (const TypeDescriptor* t = d; t; t = t->base) {
                if (t->dynamic_key) {
                    if (void* bp = d->upcast(ptr, t)) dyn_key = t->dynamic_key(bp);
                    break;
                }
            }
        }
        if (dyn_key && dyn_key != d->native_key) {
            const TypeDescriptor* dyn = find_native(dyn_key);
            if (dyn && dyn != d && dyn->registered) {
                if (void* down = dyn->downcast_from(d, ptr)) return finish_wrap(dyn, down, std::move(keep_alive));
            }
        }
        return finish_wrap(d, ptr, std::move(keep_alive));
    }

    /// Object Value → native pointer of type U (upcast through registered bases). Throws on mismatch.
    template <class U>
    U* cast(const Value& v) {
        if (!v.is_object()) throw std::runtime_error("expected an object");
        void* p = v.type()->upcast(v.ptr(), descriptor<std::remove_cv_t<U>>());
        if (!p) throw std::runtime_error("object of type " + v.type()->name + " is not a " + descriptor<std::remove_cv_t<U>>()->name);
        return static_cast<U*>(p);
    }

private:
    Value finish_wrap(const TypeDescriptor* d, void* p, std::shared_ptr<void> keep);

    std::deque<std::unique_ptr<TypeDescriptor>> owned_;
    std::unordered_map<TypeKey, TypeDescriptor*> by_type_;
    std::vector<const TypeDescriptor*> order_;
    std::map<std::pair<const TypeDescriptor*, bool>, const TypeDescriptor*> lists_;
    std::map<std::tuple<const TypeDescriptor*, const TypeDescriptor*, bool>, const TypeDescriptor*> maps_;
    template <class T> friend class ClassBuilder;
    template <class T> friend class EnumBuilder;
    void mark_registered(TypeDescriptor* d) { if (!d->registered) { d->registered = true; order_.push_back(d); } }
};

// ---------------------------------------------------------------- primary Traits (classes)

template <class T, class E>
const TypeDescriptor* Traits<T, E>::type(TypeRegistry& r) { return r.descriptor<T>(); }

template <class T, class E>
bool Traits<T, E>::assignable(TypeRegistry& r) {
    if constexpr (std::is_copy_assignable_v<T>) return r.descriptor<T>()->kind == TypeKind::Struct;
    else return false;
}

template <class T, class E>
Value Traits<T, E>::get(TypeRegistry& r, T& v) {
    const TypeDescriptor* d = r.descriptor<T>();
    if (d->kind == TypeKind::Struct) {
        if constexpr (std::is_copy_constructible_v<T>)
            return Value::structure(d, std::static_pointer_cast<void>(std::make_shared<T>(v)));
    }
    return r.wrap(&v);  // object held by value: refer to the live sub-object
}

template <class T, class E>
void Traits<T, E>::set(TypeRegistry& r, T& dst, const Value& v) {
    if constexpr (std::is_copy_assignable_v<T>) {
        const TypeDescriptor* d = r.descriptor<T>();
        if (d->kind == TypeKind::Struct && v.is_struct() && v.type() == d) {
            dst = *static_cast<const T*>(v.ptr());
            return;
        }
    }
    (void)r; (void)dst; (void)v;
    throw std::runtime_error("value is not assignable to this member");
}

// ---------------------------------------------------------------- scalar Traits

namespace detail {
bool key_number(const Value& key, double& out);  // number, or numeric string
bool key_integral(const Value& key, double lo, double hi, double& out);
}  // namespace detail

template <>
struct Traits<bool, void> {
    static const TypeDescriptor* type(TypeRegistry&) { return builtin::bool_type(); }
    static constexpr bool nullable = false;
    static bool assignable(TypeRegistry&) { return true; }
    static Value get(TypeRegistry&, const bool& v) { return Value::boolean(v); }
    static void set(TypeRegistry&, bool& dst, const Value& v) { dst = v.as_bool(); }
    static bool key_from(TypeRegistry&, const Value& k, bool& out) {
        if (!k.is_number() && !k.is_bool()) return false;
        out = k.is_bool() ? k.as_bool() : k.as_number() != 0;
        return true;
    }
};

template <class T>
struct Traits<T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>> {
    static const TypeDescriptor* type(TypeRegistry&) { return builtin::number_of<T>(); }
    static constexpr bool nullable = false;
    static bool assignable(TypeRegistry&) { return true; }
    static Value get(TypeRegistry&, const T& v) { return Value::number(static_cast<double>(v), builtin::number_of<T>()); }
    static void set(TypeRegistry&, T& dst, const Value& v) { dst = static_cast<T>(v.as_number()); }
    static bool key_from(TypeRegistry&, const Value& k, T& out) {
        double d = 0;
        if constexpr (std::is_integral_v<T>) {
            const TypeDescriptor* t = builtin::number_of<T>();
            if (!detail::key_integral(k, t->int_min, t->int_max, d)) return false;
        } else {
            if (!detail::key_number(k, d)) return false;
        }
        out = static_cast<T>(d);
        return true;
    }
};

template <>
struct Traits<std::string, void> {
    static const TypeDescriptor* type(TypeRegistry&) { return builtin::string_type(); }
    static constexpr bool nullable = false;
    static bool assignable(TypeRegistry&) { return true; }
    static Value get(TypeRegistry&, const std::string& v) { return Value::string(v); }
    static void set(TypeRegistry&, std::string& dst, const Value& v) { dst = v.as_string(); }
    static bool key_from(TypeRegistry&, const Value& k, std::string& out) {
        if (k.is_string()) { out = k.as_string(); return true; }
        if (k.is_number()) { out = to_text(k); return true; }
        return false;
    }
};

template <class T>
struct Traits<T, std::enable_if_t<std::is_enum_v<T>>> {
    static const TypeDescriptor* type(TypeRegistry& r) { return r.descriptor<T>(); }
    static constexpr bool nullable = false;
    static bool assignable(TypeRegistry&) { return true; }
    static Value get(TypeRegistry& r, const T& v) { return Value::enumeration(r.descriptor<T>(), static_cast<int64_t>(v)); }
    static void set(TypeRegistry&, T& dst, const Value& v) { dst = static_cast<T>(v.enum_value()); }
    static bool key_from(TypeRegistry& r, const Value& k, T& out) {
        int64_t e = 0;
        if (k.is_enum() && k.type() == r.descriptor<T>()) e = k.enum_value();
        else if (k.is_number()) e = static_cast<int64_t>(k.as_number());
        else if (k.is_string()) { if (!r.descriptor<T>()->enum_parse(k.as_string(), e)) return false; }
        else return false;
        out = static_cast<T>(e);
        return true;
    }
};

template <class U>
struct Traits<std::optional<U>, void> {
    static const TypeDescriptor* type(TypeRegistry& r) { return Traits<U>::type(r); }
    static constexpr bool nullable = true;
    static bool assignable(TypeRegistry& r) { return std::is_default_constructible_v<U> && Traits<U>::assignable(r); }
    static Value get(TypeRegistry& r, std::optional<U>& v) { return v ? Traits<U>::get(r, *v) : Value::null(); }
    static void set(TypeRegistry& r, std::optional<U>& dst, const Value& v) {
        if (v.is_null()) { dst.reset(); return; }
        if constexpr (std::is_default_constructible_v<U>) {
            U tmp{};
            Traits<U>::set(r, tmp, v);
            dst = std::move(tmp);
        } else {
            throw std::runtime_error("optional value type is not default constructible");
        }
    }
    static bool key_from(TypeRegistry&, const Value&, std::optional<U>&) { return false; }
};

template <class U>
struct Traits<U*, std::enable_if_t<std::is_class_v<U>>> {
    using B = std::remove_cv_t<U>;
    static const TypeDescriptor* type(TypeRegistry& r) { return r.descriptor<B>(); }
    static constexpr bool nullable = true;
    static bool assignable(TypeRegistry&) { return true; }
    static Value get(TypeRegistry& r, U* p) { return r.wrap(const_cast<B*>(p)); }
    static void set(TypeRegistry& r, U*& dst, const Value& v) { dst = v.is_null() ? nullptr : r.cast<B>(v); }
    static bool key_from(TypeRegistry& r, const Value& k, U*& out) {
        if (!k.is_object()) return false;
        void* p = k.type()->upcast(k.ptr(), r.descriptor<B>());
        if (!p) return false;
        out = static_cast<U*>(p);
        return true;
    }
};

template <class U, class D>
struct Traits<std::unique_ptr<U, D>, void> {
    static const TypeDescriptor* type(TypeRegistry& r) { return r.descriptor<U>(); }
    static constexpr bool nullable = true;
    static bool assignable(TypeRegistry&) { return false; }  // ownership cannot be transferred by RQL
    static Value get(TypeRegistry& r, std::unique_ptr<U, D>& p) { return r.wrap(p.get()); }
    static void set(TypeRegistry&, std::unique_ptr<U, D>&, const Value&) {
        throw std::runtime_error("owning unique_ptr members cannot be assigned");
    }
    static bool key_from(TypeRegistry&, const Value&, std::unique_ptr<U, D>&) { return false; }
};

template <class U>
struct Traits<std::shared_ptr<U>, void> {
    static const TypeDescriptor* type(TypeRegistry& r) { return r.descriptor<U>(); }
    static constexpr bool nullable = true;
    static bool assignable(TypeRegistry&) { return true; }
    static Value get(TypeRegistry& r, std::shared_ptr<U>& p) { return r.wrap(p.get(), std::static_pointer_cast<void>(std::const_pointer_cast<std::remove_cv_t<U>>(p))); }
    static void set(TypeRegistry& r, std::shared_ptr<U>& dst, const Value& v) {
        if (v.is_null()) { dst.reset(); return; }
        U* p = r.cast<std::remove_cv_t<U>>(v);
        if (!v.holder()) throw std::runtime_error("object is not shared-owned; cannot store it in a shared_ptr");
        dst = std::shared_ptr<U>(v.holder(), p);
    }
    static bool key_from(TypeRegistry&, const Value&, std::shared_ptr<U>&) { return false; }
};

// ---------------------------------------------------------------- container adapters

template <class U, class A>
class VectorAdapter final : public IListAdapter {
public:
    VectorAdapter(TypeRegistry* r, std::vector<U, A>* v, const TypeDescriptor* t) : r_(r), v_(v), t_(t) {}
    size_t size() const override { return v_->size(); }
    Value at(size_t i) const override {
        if constexpr (std::is_same_v<U, bool>) return Value::boolean(static_cast<bool>((*v_)[i]));
        else return Traits<U>::get(*r_, (*v_)[i]);
    }
    const TypeDescriptor* type() const override { return t_; }
    const void* identity() const override { return v_; }
    bool writable() const override {
        if constexpr (std::is_same_v<U, bool>) return true;
        else return Traits<U>::assignable(*r_);
    }
    void set(size_t i, const Value& v) override {
        if constexpr (std::is_same_v<U, bool>) (*v_)[i] = v.as_bool();
        else Traits<U>::set(*r_, (*v_)[i], v);
    }

private:
    TypeRegistry* r_;
    std::vector<U, A>* v_;
    const TypeDescriptor* t_;
};

template <class U, class A>
struct Traits<std::vector<U, A>, void> {
    static const TypeDescriptor* type(TypeRegistry& r) { return r.list_type(Traits<U>::type(r), Traits<U>::nullable); }
    static constexpr bool nullable = false;
    static bool assignable(TypeRegistry& r) {
        if constexpr (std::is_same_v<U, bool>) return true;
        else return std::is_default_constructible_v<U> && Traits<U>::assignable(r);
    }
    static Value get(TypeRegistry& r, std::vector<U, A>& v) {
        return Value::list(std::make_shared<VectorAdapter<U, A>>(&r, &v, type(r)));
    }
    static void set(TypeRegistry& r, std::vector<U, A>& dst, const Value& v) {
        if (!v.is_list()) throw std::runtime_error("expected a list");
        std::vector<U, A> out;
        auto& l = v.list();
        size_t n = l.size();
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            Value el = l.at(i);
            if constexpr (std::is_same_v<U, bool>) {
                out.push_back(el.as_bool());
            } else if constexpr (std::is_default_constructible_v<U>) {
                U tmp{};
                Traits<U>::set(r, tmp, el);
                out.push_back(std::move(tmp));
            } else {
                throw std::runtime_error("list element type is not default constructible");
            }
        }
        dst = std::move(out);
    }
    static bool key_from(TypeRegistry&, const Value&, std::vector<U, A>&) { return false; }
};

/// Adapter for std::map-like (ordered) and std::unordered_map-like (sorted by key at iteration) maps.
template <class M, bool Ordered>
class MapAdapter final : public IMapAdapter {
public:
    using K = typename M::key_type;
    using V = typename M::mapped_type;
    MapAdapter(TypeRegistry* r, M* m, const TypeDescriptor* t) : r_(r), m_(m), t_(t) {}
    size_t size() const override { return m_->size(); }
    void for_each(const std::function<bool(const Value&, const Value&)>& fn) const override {
        if constexpr (Ordered) {
            for (auto& kv : *m_) {
                K k = kv.first;
                if (!fn(Traits<K>::get(*r_, k), Traits<V>::get(*r_, kv.second))) return;
            }
        } else {
            std::vector<typename M::value_type*> sorted;
            for (auto& kv : *m_) sorted.push_back(&kv);
            std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->first < b->first; });
            for (auto* kv : sorted) {
                K k = kv->first;
                if (!fn(Traits<K>::get(*r_, k), Traits<V>::get(*r_, kv->second))) return;
            }
        }
    }
    MapLookup find(const Value& key, Value& out) const override {
        K k{};
        if (!Traits<K>::key_from(*r_, key, k)) return MapLookup::BadKey;
        auto it = m_->find(k);
        if (it == m_->end()) return MapLookup::NotFound;
        out = Traits<V>::get(*r_, it->second);
        return MapLookup::Found;
    }
    const TypeDescriptor* type() const override { return t_; }
    const void* identity() const override { return m_; }
    bool writable() const override { return std::is_default_constructible_v<V> && Traits<V>::assignable(*r_); }
    bool accepts_key(const Value& key) const override { K k{}; return Traits<K>::key_from(*r_, key, k); }
    void set(const Value& key, const Value& value) override {
        K k{};
        if (!Traits<K>::key_from(*r_, key, k)) throw std::runtime_error("invalid map key");
        if constexpr (std::is_default_constructible_v<V>) {
            V tmp{};
            Traits<V>::set(*r_, tmp, value);
            (*m_)[k] = std::move(tmp);
        } else {
            throw std::runtime_error("map value type is not default constructible");
        }
    }
    void erase(const Value& key) override {
        K k{};
        if (Traits<K>::key_from(*r_, key, k)) m_->erase(k);
    }

private:
    TypeRegistry* r_;
    M* m_;
    const TypeDescriptor* t_;
};

template <class M, bool Ordered>
struct MapTraits {
    using K = typename M::key_type;
    using V = typename M::mapped_type;
    static const TypeDescriptor* type(TypeRegistry& r) { return r.map_type(Traits<K>::type(r), Traits<V>::type(r), Traits<V>::nullable); }
    static constexpr bool nullable = false;
    static bool assignable(TypeRegistry& r) { return std::is_default_constructible_v<V> && Traits<V>::assignable(r); }
    static Value get(TypeRegistry& r, M& m) { return Value::map(std::make_shared<MapAdapter<M, Ordered>>(&r, &m, type(r))); }
    static void set(TypeRegistry& r, M& dst, const Value& v) {
        if (!v.is_map()) throw std::runtime_error("expected a map");
        M out;
        bool ok = true;
        v.map().for_each([&](const Value& k, const Value& val) {
            K nk{};
            if (!Traits<K>::key_from(r, k, nk)) { ok = false; return false; }
            if constexpr (std::is_default_constructible_v<V>) {
                V nv{};
                Traits<V>::set(r, nv, val);
                out[nk] = std::move(nv);
            } else {
                ok = false;
            }
            return ok;
        });
        if (!ok) throw std::runtime_error("map entries are not assignable");
        dst = std::move(out);
    }
    static bool key_from(TypeRegistry&, const Value&, M&) { return false; }
};

template <class K, class V, class C, class A>
struct Traits<std::map<K, V, C, A>, void> : MapTraits<std::map<K, V, C, A>, true> {};
template <class K, class V, class H, class E, class A>
struct Traits<std::unordered_map<K, V, H, E, A>, void> : MapTraits<std::unordered_map<K, V, H, E, A>, false> {};

// ---------------------------------------------------------------- builders

template <class T>
class ClassBuilder {
public:
    ClassBuilder(TypeRegistry& r, TypeDescriptor& d) : r_(&r), d_(&d) {}

    /// Inherit members/methods/identity of a registered base B (call before own members).
    template <class B>
    ClassBuilder& base() {
        static_assert(std::is_base_of_v<B, T>, "B must be a base of T");
        TypeDescriptor* b = r_->descriptor<B>();
        d_->base = b;
        d_->to_base = [](void* p) -> void* { return static_cast<B*>(static_cast<T*>(p)); };
        d_->from_base = [](void* p) -> void* { return static_cast<T*>(static_cast<B*>(p)); };
        auto up = d_->to_base;
        for (const MemberDescriptor& m : b->members) {
            MemberDescriptor c = m;
            Getter g = m.getter;
            c.getter = [g, up](void* self) { return g(up(self)); };
            if (m.setter) {
                Setter s = m.setter;
                c.setter = [s, up](void* self, const Value& v) { s(up(self), v); };
            }
            add(std::move(c));
        }
        for (const MethodDescriptor& m : b->methods) {
            MethodDescriptor c = m;
            auto inv = m.invoke;
            c.invoke = [inv, up](void* self, const std::vector<Value>& a) { return inv(up(self), a); };
            if (m.validate) {
                CallValidator val = m.validate;
                c.validate = [val, up](void* self, const std::vector<Value>& a) { return val(up(self), a); };
            }
            d_->methods.push_back(std::move(c));
        }
        if (!d_->id_explicit) { d_->id_name = b->id_name; d_->id_explicit = b->id_explicit; }
        if (!d_->display_explicit) { d_->display_name = b->display_name; d_->display_explicit = b->display_explicit; }
        return *this;
    }

    /// Field (pointer to data member; may belong to a base class of T).
    template <class M, class C>
    ClassBuilder& member(std::string name, M C::*pm) {
        static_assert(std::is_base_of_v<C, T>, "member must belong to T or a base of T");
        TypeRegistry* r = r_;
        MemberDescriptor m;
        m.name = std::move(name);
        m.type = Traits<std::remove_cv_t<M>>::type(*r);
        m.nullable = Traits<std::remove_cv_t<M>>::nullable;
        m.getter = [r, pm](void* self) -> Value {
            T* t = static_cast<T*>(self);
            return Traits<std::remove_cv_t<M>>::get(*r, const_cast<std::remove_cv_t<M>&>(t->*pm));
        };
        if constexpr (!std::is_const_v<M>) {
            m.setter = [r, pm](void* self, const Value& v) {
                T* t = static_cast<T*>(self);
                Traits<M>::set(*r, t->*pm, v);
            };
            m.assignable = [r]() { return Traits<M>::assignable(*r); };
        }
        add(std::move(m));
        return *this;
    }

    /// Computed read-only property: g(T&) returns the value (lists/maps are snapshotted).
    template <class G>
    ClassBuilder& property(std::string name, G g) {
        using R = std::decay_t<decltype(g(std::declval<T&>()))>;
        TypeRegistry* r = r_;
        MemberDescriptor m;
        m.name = std::move(name);
        m.type = Traits<R>::type(*r);
        m.nullable = Traits<R>::nullable;
        m.getter = [r, g](void* self) -> Value {
            R tmp = g(*static_cast<T*>(self));
            return snapshot(Traits<R>::get(*r, tmp));
        };
        add(std::move(m));
        return *this;
    }

    /// Computed property with setter s(T&, R).
    template <class G, class S>
    ClassBuilder& property(std::string name, G g, S s) {
        using R = std::decay_t<decltype(g(std::declval<T&>()))>;
        property(name, g);
        TypeRegistry* r = r_;
        MemberDescriptor& m = d_->members.back();
        m.setter = [r, s](void* self, const Value& v) {
            R tmp{};
            Traits<R>::set(*r, tmp, v);
            s(*static_cast<T*>(self), std::move(tmp));
        };
        m.assignable = [r]() { return Traits<R>::assignable(*r); };
        return *this;
    }

    /// Modifiers for the last added member.
    ClassBuilder& readonly(bool v = true) { last().readonly = v; return *this; }
    /// Non-owning reference (parent links, targets): readable, not followed by generic recurse.
    ClassBuilder& reference() { last().owning = false; return *this; }
    ClassBuilder& describe(std::string text) {
        if (d_->members.empty()) d_->description = std::move(text);
        else last().description = std::move(text);
        return *this;
    }
    ClassBuilder& type_description(std::string text) { d_->description = std::move(text); return *this; }

    ClassBuilder& id(std::string member) { d_->id_name = std::move(member); d_->id_explicit = true; return *this; }
    ClassBuilder& display(std::string member) { d_->display_name = std::move(member); d_->display_explicit = true; return *this; }
    /// Disable automatic id/display detection ("Id" / "Name").
    ClassBuilder& no_identity() { d_->id_name.clear(); d_->id_explicit = true; d_->display_name.clear(); d_->display_explicit = true; return *this; }
    ClassBuilder& also_is(std::string type_name) { d_->also_is.push_back(std::move(type_name)); return *this; }
    /// Allow object literals (spec §4.5) to construct instances: value-initialized T, members not named
    /// in the literal keep their defaults. Constructed objects stored through raw pointers are kept alive
    /// by the engine that built them; shared_ptr members share ownership.
    ClassBuilder& constructible() {
        static_assert(std::is_default_constructible_v<T>, "constructible() requires a default constructor");
        d_->create = []() { return std::static_pointer_cast<void>(std::make_shared<T>()); };
        d_->host_owned = false;
        return *this;
    }
    /// Host factory for object literals (e.g. Unreal's NewObject): f() returns a new T* the HOST owns and
    /// keeps alive (GC, scene, ...). The engine never retains or frees it.
    template <class F>
    ClassBuilder& factory(F f) {
        d_->create = [f]() -> std::shared_ptr<void> {
            T* p = f();
            if (!p) throw std::runtime_error("host factory returned null");
            return std::shared_ptr<void>(static_cast<void*>(p), [](void*) {});  // non-owning
        };
        d_->host_owned = true;
        return *this;
    }
    /// Dynamic-type hook (RTTI-free polymorphism): f(const T&) returns rql::type_key<MostDerived>().
    /// Not needed when T has a `TypeKey rql_dynamic_type() const` member.
    template <class F>
    ClassBuilder& dynamic_type(F f) {
        d_->dynamic_key = [f](void* self) -> TypeKey { return f(*static_cast<const T*>(self)); };
        return *this;
    }

    /// Callable method (allowlisted for the `call` stage).
    template <class R, class C, class... A>
    ClassBuilder& method(std::string name, R (C::*fn)(A...), std::vector<std::string> param_names = {}, std::string description = {}) {
        return add_method<R, A...>(std::move(name), [fn](T& t, A... a) -> R { return (t.*fn)(std::forward<A>(a)...); },
                                   std::move(param_names), std::move(description));
    }
    template <class R, class C, class... A>
    ClassBuilder& method(std::string name, R (C::*fn)(A...) const, std::vector<std::string> param_names = {}, std::string description = {}) {
        return add_method<R, A...>(std::move(name), [fn](T& t, A... a) -> R { return (t.*fn)(std::forward<A>(a)...); },
                                   std::move(param_names), std::move(description));
    }
    template <class R, class C, class... A>
    ClassBuilder& method(std::string name, R (C::*fn)(A...) noexcept, std::vector<std::string> param_names = {}, std::string description = {}) {
        return add_method<R, A...>(std::move(name), [fn](T& t, A... a) -> R { return (t.*fn)(std::forward<A>(a)...); },
                                   std::move(param_names), std::move(description));
    }
    template <class R, class C, class... A>
    ClassBuilder& method(std::string name, R (C::*fn)(A...) const noexcept, std::vector<std::string> param_names = {}, std::string description = {}) {
        return add_method<R, A...>(std::move(name), [fn](T& t, A... a) -> R { return (t.*fn)(std::forward<A>(a)...); },
                                   std::move(param_names), std::move(description));
    }
    /// Validator of the last method: v(T&, coerced args) returns a rejection reason, empty to accept (spec §8).
    ClassBuilder& validate_call(std::function<std::string(T&, const std::vector<Value>&)> v) {
        if (!d_->methods.empty() && v)
            d_->methods.back().validate = [v](void* self, const std::vector<Value>& args) { return v(*static_cast<T*>(self), args); };
        return *this;
    }
    /// Mark the last method as known but not callable (call → permission-denied).
    ClassBuilder& not_callable() {
        if (!d_->methods.empty()) d_->methods.back().callable = false;
        return *this;
    }

private:
    TypeRegistry* r_;
    TypeDescriptor* d_;

    MemberDescriptor& last() {
        // Builder misuse (modifier before any member) must not throw: modify a scratch descriptor.
        static thread_local MemberDescriptor scratch;
        return d_->members.empty() ? scratch : d_->members.back();
    }

    void add(MemberDescriptor m) {
        for (auto& existing : d_->members) {
            if (existing.name == m.name) { existing = std::move(m); return; }  // override keeps base order
        }
        d_->members.push_back(std::move(m));
    }

    template <class A>
    static std::decay_t<A> arg_from(TypeRegistry& r, const Value& v) {
        using D = std::decay_t<A>;
        if constexpr (std::is_default_constructible_v<D>) {
            D tmp{};
            Traits<D>::set(r, tmp, v);
            return tmp;
        } else {
            throw std::runtime_error("parameter type is not default constructible");
        }
    }

    template <class R, class... A, class F, size_t... I>
    static Value invoke_impl(TypeRegistry& r, F& f, T& self, const std::vector<Value>& args, std::index_sequence<I...>) {
        if constexpr (std::is_void_v<R>) {
            f(self, arg_from<A>(r, args[I])...);
            return Value::null();
        } else if constexpr (std::is_reference_v<R>) {
            auto&& res = f(self, arg_from<A>(r, args[I])...);
            return Traits<std::remove_cv_t<std::remove_reference_t<R>>>::get(r, const_cast<std::remove_cv_t<std::remove_reference_t<R>>&>(res));
        } else {
            std::remove_cv_t<R> res = f(self, arg_from<A>(r, args[I])...);
            return snapshot(Traits<std::remove_cv_t<R>>::get(r, res));
        }
    }

    template <class R, class... A, class F>
    ClassBuilder& add_method(std::string name, F f, std::vector<std::string> names, std::string description) {
        TypeRegistry* r = r_;
        MethodDescriptor m;
        m.name = std::move(name);
        m.description = std::move(description);
        size_t idx = 0;
        (void)std::initializer_list<int>{(m.params.push_back(ParamDescriptor{
                                               idx < names.size() ? names[idx] : "arg" + std::to_string(idx),
                                               Traits<std::decay_t<A>>::type(*r), Traits<std::decay_t<A>>::nullable, false, Value()}),
                                           ++idx, 0)...};
        if constexpr (!std::is_void_v<R>) m.result = Traits<std::remove_cv_t<std::remove_reference_t<R>>>::type(*r);
        m.invoke = [r, f](void* self, const std::vector<Value>& args) mutable -> Value {
            if (args.size() != sizeof...(A)) throw std::runtime_error("wrong number of arguments");
            return invoke_impl<R, A...>(*r, f, *static_cast<T*>(self), args, std::index_sequence_for<A...>{});
        };
        d_->methods.push_back(std::move(m));
        return *this;
    }
};

template <class T>
class EnumBuilder {
public:
    EnumBuilder(TypeRegistry&, TypeDescriptor& d) : d_(&d) {}
    EnumBuilder& value(std::string name, T v) {
        d_->enum_values.emplace_back(std::move(name), static_cast<int64_t>(v));
        return *this;
    }
    EnumBuilder& describe(std::string text) { d_->description = std::move(text); return *this; }

private:
    TypeDescriptor* d_;
};

template <class T>
ClassBuilder<T> TypeRegistry::object(std::string name) {
    static_assert(std::is_class_v<T>, "object<T> requires a class type");
    TypeDescriptor* d = descriptor<T>();
    d->native_key = type_key<T>();
    d->name = std::move(name);
    d->friendly = d->name;
    d->kind = TypeKind::Object;
    mark_registered(d);
    return ClassBuilder<T>(*this, *d);
}

template <class T>
ClassBuilder<T> TypeRegistry::structure(std::string name) {
    static_assert(std::is_class_v<T> && std::is_copy_constructible_v<T>, "structure<T> requires a copyable class type");
    TypeDescriptor* d = descriptor<T>();
    d->native_key = type_key<T>();
    d->name = std::move(name);
    d->friendly = d->name;
    d->kind = TypeKind::Struct;
    d->copy = [](const void* p) { return std::static_pointer_cast<void>(std::make_shared<T>(*static_cast<const T*>(p))); };
    if constexpr (std::is_default_constructible_v<T>)
        d->create = []() { return std::static_pointer_cast<void>(std::make_shared<T>()); };
    mark_registered(d);
    return ClassBuilder<T>(*this, *d);
}

template <class T>
EnumBuilder<T> TypeRegistry::enumeration(std::string name) {
    static_assert(std::is_enum_v<T>, "enumeration<T> requires an enum type");
    TypeDescriptor* d = descriptor<T>();
    d->native_key = type_key<T>();
    d->name = std::move(name);
    d->friendly = d->name;
    d->kind = TypeKind::Enum;
    mark_registered(d);
    return EnumBuilder<T>(*this, *d);
}

}  // namespace rql
