// Type adapters (spec §13): host extensions that customise how values of a type family are read
// (virtual / hidden / explicit-only members), shown (encode), written (coerce + validation),
// compared, identified (stable id) and checked for liveness — without changing the plan format.
//
// Adapters are registered per Engine (Engine::add_adapter). The engine derives an adapted view of each
// TypeDescriptor it meets (members after adapters, per-capability adapter lists), so one TypeRegistry
// can serve engines with different adapter sets. An engine without adapters reads descriptors directly.
//
// Applicability: an adapter targets a descriptor (exact type, or a base: applies to subclasses) or
// an interface name (matched against TypeDescriptor::also_is). Order, most specific first: base-chain
// distance d (rank d), then interfaces (rank 10000); ties by priority (higher first), then
// registration order. There is no open-generic tier in C++ (spec §13 allows omitting it). Engine-internal
// values (projected rows, map entries, errors) never have adapters.
//
// This is the seam an Unreal provider uses for FVector/FRotator encoding & literals, UObject
// IsValid / pending-kill liveness, and GUID-based stable ids.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rql/json.hpp"
#include "rql/reflection.hpp"
#include "rql/value.hpp"

namespace rql {

/// Outcome of a capability that may decline: not handled (defer), succeeded, or failed with a reason.
enum class AdapterOutcome : uint8_t { NotHandled, Ok, Failed };

namespace detail {
class Encoder;
}

/// What an encode callback can use (valid only during the callback).
class EncodeContext {
public:
    EncodeContext(detail::Encoder& encoder, int depth) noexcept : encoder_(&encoder), depth_(depth) {}
    /// Nesting depth of the value being encoded (0 = a top-level cell).
    int depth() const { return depth_; }
    /// Encode another value with full adapter support (e.g. a member of this value).
    Json encode_value(const Value& v) const;
    /// The standard object summary {"$ref": ..., "name": ...}.
    Json summary(const Value& object) const;

private:
    detail::Encoder* encoder_;
    int depth_;
};

/// The member table of one runtime type while adapters shape it. Errors (configuring a member that
/// does not exist, virtual setters on value types) throw std::logic_error inside the engine, which
/// fails the query with `internal-error` — they are host bugs.
/// A string-keyed map defined by an adapter over its owner (spec §13 "Virtual maps"): the owner's keys,
/// per-key get, optional per-key set and validation. Reads and writes use the ordinary map paths, so
/// coercion, validation-before-write, rollback and readonly-member fall out. Entries are never removed.
class VirtualMapAdapter final : public IMapAdapter {
public:
    using Keys = std::function<std::vector<std::string>(void* owner)>;
    using Get = std::function<Value(void* owner, const std::string& key)>;
    using Set = std::function<void(void* owner, const std::string& key, const Value& value)>;
    using Validate = std::function<std::string(void* owner, const std::string& key, const Value& value)>;
    /// Mixed-type maps: the declared type of one entry (values are coerced to it before validation).
    using EntryType = std::function<const TypeDescriptor*(void* owner, const std::string& key)>;
    /// Optional fast membership test (otherwise lookups scan keys()).
    using Has = std::function<bool(void* owner, const std::string& key)>;

    /// Everything that defines the map, shared (immutable) by every view of it.
    struct Spec {
        const TypeDescriptor* type = nullptr;  // Dictionary<string, T>
        Keys keys;
        Get get;
        Set set;  // empty = read-only
        Validate validate;
        EntryType entry_type;
        Has has;
    };

    VirtualMapAdapter(void* owner, std::shared_ptr<const Spec> spec) : owner_(owner), spec_(std::move(spec)) {}

    size_t size() const override { return spec_->keys(owner_).size(); }
    void for_each(const std::function<bool(const Value&, const Value&)>& fn) const override;
    MapLookup find(const Value& key, Value& out) const override;
    const TypeDescriptor* type() const override { return spec_->type; }
    bool writable() const override { return static_cast<bool>(spec_->set); }
    bool fixed_keys() const override { return true; }
    bool accepts_key(const Value& key) const override { return key.is_string() || key.is_number(); }
    void set(const Value& key, const Value& value) override;
    void erase(const Value& key) override;
    std::string validate_entry(const Value& key, const Value& value) const override;
    const TypeDescriptor* entry_type(const Value& key) const override {
        return spec_->entry_type ? spec_->entry_type(owner_, key_text(key)) : nullptr;
    }

private:
    void* owner_;
    std::shared_ptr<const Spec> spec_;
    static std::string key_text(const Value& key) { return key.is_string() ? key.as_string() : to_text(key); }
};

class MemberTable {
public:
    MemberTable(TypeRegistry& registry, const TypeDescriptor& type, std::vector<MemberDescriptor>& members,
                std::vector<MethodDescriptor>& methods)
        : registry_(registry), type_(type), members_(members), methods_(methods) {}

    /// The runtime type whose members are being configured.
    const TypeDescriptor& type() const { return type_; }
    TypeRegistry& registry() const { return registry_; }
    const std::vector<MemberDescriptor>& members() const { return members_; }
    bool contains(const std::string& name) const;

    /// Add (or replace) a virtual member. `self` passed to get/set/validate is a pointer to the
    /// ADAPTER's target type (upcast from the configured type). set empty = read-only. Virtual
    /// members are non-owning by default (computed values are not structural children).
    MemberTable& add(std::string name, const TypeDescriptor* type, Getter get, Setter set = {}, Validator validate = {},
                     std::string description = {}, bool owning = false, bool nullable = false);

    /// Typed read-only virtual member: get(T&) -> R (R any Traits-supported type).
    template <class T, class G>
    MemberTable& computed(std::string name, G get, std::string description = {}) {
        using R = std::decay_t<decltype(get(std::declval<T&>()))>;
        TypeRegistry* r = &registry_;
        return add(std::move(name), Traits<R>::type(*r), [r, get](void* self) -> Value {
            R tmp = get(*static_cast<T*>(self));
            return snapshot(Traits<R>::get(*r, tmp));
        }, {}, {}, std::move(description), false, Traits<R>::nullable);
    }

    /// Typed writable virtual member: get(T&) -> R, set(T&, R) (may throw → rollback),
    /// validate(T&, const R&) -> std::string reason (empty = ok; pass nullptr for none).
    template <class T, class G, class S, class V>
    MemberTable& property(std::string name, G get, S set, V validate, std::string description = {}) {
        using R = std::decay_t<decltype(get(std::declval<T&>()))>;
        TypeRegistry* r = &registry_;
        Validator val;
        if constexpr (!std::is_same_v<V, std::nullptr_t>) {
            val = [r, validate](void* self, const Value& v) -> std::string {
                R tmp{};
                Traits<R>::set(*r, tmp, v);
                return validate(*static_cast<T*>(self), tmp);
            };
        }
        return add(
            std::move(name), Traits<R>::type(*r),
            [r, get](void* self) -> Value {
                R tmp = get(*static_cast<T*>(self));
                return snapshot(Traits<R>::get(*r, tmp));
            },
            [r, set](void* self, const Value& v) {
                R tmp{};
                Traits<R>::set(*r, tmp, v);
                set(*static_cast<T*>(self), std::move(tmp));
            },
            std::move(val), std::move(description), false, Traits<R>::nullable);
    }

    /// Remove members entirely (unreadable AND unwritable). Every name must exist (else internal-error).
    MemberTable& hide(const std::vector<std::string>& names);
    /// Keep only the named members (whitelist). Every name must exist (else internal-error).
    MemberTable& only(const std::vector<std::string>& names);
    MemberTable& read_only(const std::vector<std::string>& names);
    /// Non-owning references: generic recurse and ref search do not follow them.
    MemberTable& reference(const std::vector<std::string>& names);
    /// Read only when a path names the member: skipped by generic recurse, inline/struct encoding and
    /// ref search; `describe` lists it with "explicitOnly": true.
    MemberTable& explicit_only(const std::vector<std::string>& names);
    MemberTable& describe(const std::string& name, std::string description);

    /// Hide every member matching the predicate (e.g. deprecated members).
    MemberTable& hide_where(const std::function<bool(const MemberDescriptor&)>& hide);
    /// Getters returning a copy: writes into the value are assigned back through the setter (spec §13 Write-back).
    MemberTable& copy_on_read(const std::vector<std::string>& names);
    /// The (struct) type is a handle to shared state: writes to copies are live — no write-back, get-only
    /// owners are fine, virtual setters are allowed, values may be mutation targets. Call before add().
    MemberTable& writes_through();
    /// Member used for stable refs "Type#id" / shown as `name` in summaries (instead of "Id" / "Name").
    MemberTable& identity(const std::string& name);
    MemberTable& display(const std::string& name);
    bool is_writes_through() const { return writes_through_; }
    const std::string& identity_name() const { return identity_name_; }
    const std::string& display_name() const { return display_name_; }

    // ---- methods (spec §13 "Methods"): invoked by `call` like host-allowlisted methods

    /// Add (or replace, same name and arity) a callable method. `self` is a pointer to the adapter's
    /// target type; args arrive coerced to the parameter types (defaults filled in). `returns` nullptr =
    /// no result. The receipt / describe show "name(type param, ...)". `validate` (optional) checks a planned
    /// call before any call is made, on dry runs too: a rejection reason, or empty to accept (spec §8).
    MemberTable& method(std::string name, std::vector<ParamDescriptor> params, const TypeDescriptor* returns,
                        std::function<Value(void* self, const std::vector<Value>& args)> invoke, std::string description = {},
                        CallValidator validate = {});
    /// Typed: f(T&, const std::vector<Value>& args) -> Value; validate(T&, args) -> reason or empty.
    template <class T, class F>
    MemberTable& method(std::string name, std::vector<ParamDescriptor> params, const TypeDescriptor* returns, F f,
                        std::string description = {}, std::function<std::string(T&, const std::vector<Value>&)> validate = {}) {
        CallValidator v;
        if (validate) v = [validate](void* self, const std::vector<Value>& args) { return validate(*static_cast<T*>(self), args); };
        return method(std::move(name), std::move(params), returns,
                      [f](void* self, const std::vector<Value>& args) -> Value { return f(*static_cast<T*>(self), args); },
                      std::move(description), std::move(v));
    }
    /// Remove callable methods by name (all overloads). Every name must exist (else internal-error).
    MemberTable& hide_methods(const std::vector<std::string>& names);

    // ---- virtual maps (spec §13 "Virtual maps")

    /// Add a virtual map member (the one implementation path): spec.type is filled in from
    /// `value_type` (Dictionary<string, T>; builtin::any_type() for mixed maps with spec.entry_type).
    /// Callbacks receive a pointer to the adapter's target type.
    MemberTable& map(std::string name, const TypeDescriptor* value_type, VirtualMapAdapter::Spec spec, std::string description = {});
    /// Add a read-only (set empty) or writable virtual map member of `value_type` values.
    MemberTable& map(std::string name, const TypeDescriptor* value_type, VirtualMapAdapter::Keys keys, VirtualMapAdapter::Get get,
                     VirtualMapAdapter::Set set = {}, VirtualMapAdapter::Validate validate = {}, std::string description = {},
                     VirtualMapAdapter::Has has = {});
    /// Mixed-type virtual map (schemas: Dictionary<string, object>): entry_type(self, key) declares each
    /// entry's type; written values are coerced to it before validation.
    MemberTable& mixed_map(std::string name, VirtualMapAdapter::Keys keys, VirtualMapAdapter::EntryType entry_type,
                           VirtualMapAdapter::Get get, VirtualMapAdapter::Set set = {}, VirtualMapAdapter::Validate validate = {},
                           std::string description = {});
    /// Typed: keys(T&) -> std::vector<std::string>, get(T&, key) -> V, set(T&, key, V) (nullptr = read-only),
    /// validate(T&, key, const V&) -> std::string reason (nullptr = none), has(T&, key) -> bool (nullptr = scan keys).
    template <class T, class K, class G, class S = std::nullptr_t, class V = std::nullptr_t, class H = std::nullptr_t>
    MemberTable& map(std::string name, K keys, G get, S set = nullptr, V validate = nullptr, std::string description = {},
                     H has = nullptr) {
        using R = std::decay_t<decltype(get(std::declval<T&>(), std::declval<const std::string&>()))>;
        TypeRegistry* r = &registry_;
        auto to_r = [r](const Value& v) {
            R tmp{};
            Traits<R>::set(*r, tmp, v);
            return tmp;
        };
        VirtualMapAdapter::Spec spec;
        spec.keys = [keys](void* self) { return keys(*static_cast<T*>(self)); };
        spec.get = [r, get](void* self, const std::string& key) -> Value {
            R tmp = get(*static_cast<T*>(self), key);
            return snapshot(Traits<R>::get(*r, tmp));
        };
        if constexpr (!std::is_same_v<S, std::nullptr_t>)
            spec.set = [to_r, set](void* self, const std::string& key, const Value& v) { set(*static_cast<T*>(self), key, to_r(v)); };
        if constexpr (!std::is_same_v<V, std::nullptr_t>)
            spec.validate = [to_r, validate](void* self, const std::string& key, const Value& v) -> std::string {
                return validate(*static_cast<T*>(self), key, to_r(v));
            };
        if constexpr (!std::is_same_v<H, std::nullptr_t>)
            spec.has = [has](void* self, const std::string& key) { return has(*static_cast<T*>(self), key); };
        return map(std::move(name), Traits<R>::type(*r), std::move(spec), std::move(description));
    }

    /// Internal: the adapter being applied (its target receives `self`).
    void set_current_target(const TypeDescriptor* target) { current_target_ = target; }

private:
    TypeRegistry& registry_;
    const TypeDescriptor& type_;
    std::vector<MemberDescriptor>& members_;
    std::vector<MethodDescriptor>& methods_;
    const TypeDescriptor* current_target_ = nullptr;
    bool writes_through_ = false;
    std::string identity_name_, display_name_;

    /// self → adapter-target pointer conversion (identity when no upcast is needed).
    std::function<void*(void*)> upcaster() const;
    MemberDescriptor& member(const std::string& name);
    /// Keep (keep = true) or drop (keep = false) exactly the named members.
    MemberTable& retain(const std::vector<std::string>& names, bool keep);
};

/// A type adapter. Every capability is optional (an empty std::function defers to the next adapter,
/// then to RQL's default). Callbacks may be noexcept; exceptions thrown by them are contained by the
/// engine (→ internal-error, or rollback for setters).
struct TypeAdapter {
    std::string name;                    // for explain() and messages
    const TypeDescriptor* target = nullptr;  // exact type or base class
    std::string interface_name;          // alternatively: any type whose also_is contains this name
    int priority = 0;                    // tie-break among equal specificity (higher first)
    std::string description;             // one line for schemas / describe (first non-empty wins)
    std::optional<Json> literal_schema;  // JSON Schema of accepted literals, shown by describe as "literal"


    /// Shape the member table (applied least → most specific).
    std::function<void(MemberTable&)> configure_members;
    /// Replace the encoding of a value of the type; nullopt defers.
    std::function<std::optional<Json>(const Value& value, const EncodeContext& ctx)> encode;
    /// Convert a query-supplied value to `target` (the adapter's type or a subtype) on writes.
    std::function<AdapterOutcome(const Value& value, const TypeDescriptor& target, Value& result, std::string& reason)> coerce;
    /// Equality / ordering of a value of the type with any other value; nullopt defers.
    std::function<std::optional<bool>(const Value& value, const Value& other)> equals;
    std::function<std::optional<int>(const Value& value, const Value& other)> compare;
    /// Stable id for $ref ("Type#id") and ref resolution; nullopt defers to the id member / handles.
    std::function<std::optional<std::string>(const Value& value)> stable_id;
    /// Liveness verdict (destroyed objects → stale-ref); nullopt defers to Engine::is_alive. When it
    /// answers for a session handle ("Type@n") the stored pointer is trusted without a reachability
    /// search, so it must decide liveness WITHOUT dereferencing a possibly destroyed object (e.g. via a
    /// weak handle / object registry, as Unreal's IsValid on a UObject* tracked by the GC).
    std::function<std::optional<bool>(const Value& value)> is_alive;
    /// Component-wise arithmetic (spec §4.2, §13 "Arithmetic"): the numeric components of a value (always the same count
    /// for the type, in a fixed order; widen single-precision components with widen_single), nullopt when the type does
    /// not take part. Opt in only for types whose arithmetic is component-wise (vectors, rotators, colors), never
    /// quaternions or matrices. Requires from_components.
    std::function<std::optional<std::vector<double>>(const Value& value)> components;
    /// A value of `type` built from components in the order `components` returns them; nullopt when it cannot be built.
    std::function<std::optional<Value>(const TypeDescriptor& type, const std::vector<double>& components)> from_components;
};

/// Engine-wide member type exclusion (spec §13): a member is dropped when its type, element type or key
/// type matches (e.g. native handles or modules that must never be touched).
using TypeExclusion = std::function<bool(const TypeDescriptor& type)>;

/// Typed view of an object/struct value as T (upcast through registered bases); nullptr if not a T.
template <class T>
T* value_as(TypeRegistry& registry, const Value& v) {
    if (!(v.is_object() || v.is_struct()) || !v.type()) return nullptr;
    return static_cast<T*>(v.type()->upcast(v.ptr(), registry.descriptor<T>()));
}

/// Specificity rank of `adapter` for `type` (lower = more specific): base-chain distance d (0 = the
/// type itself), interfaces 10000; -1 = not applicable.
int adapter_rank(const TypeAdapter& adapter, const TypeDescriptor& type);

}  // namespace rql
