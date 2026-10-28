// Type adapters (spec §13): member tables, ranking and per-engine type views.
#include "rql/adapters.hpp"

#include <algorithm>
#include <stdexcept>

#include "exec.hpp"

namespace rql {

// ------------------------------------------------------------------ MemberTable

bool MemberTable::contains(const std::string& name) const {
    for (auto& m : members_)
        if (m.name == name) return true;
    return false;
}

MemberDescriptor& MemberTable::member(const std::string& name) {
    for (auto& m : members_)
        if (m.name == name) return m;
    throw std::logic_error("type " + type_.name + " has no member '" + name + "' to configure");
}

MemberTable& MemberTable::add(std::string name, const TypeDescriptor* type, Getter get, Setter set, Validator validate,
                              std::string description, bool owning, bool nullable) {
    if (name.empty()) throw std::logic_error("virtual member name required");
    if (!get) throw std::logic_error("virtual member '" + name + "' needs a getter");
    if (set && type_.kind == TypeKind::Struct && !writes_through_)
        throw std::logic_error("virtual setters on value type " + type_.name +
                               " would write to a copy; expose a writable field instead (or mark the type writes_through if it is a handle)");
    MemberDescriptor m;
    m.name = std::move(name);
    m.type = type;
    m.nullable = nullable;
    m.owning = owning;
    m.description = std::move(description);
    // Callbacks receive a pointer typed as the adapter's target (a base of, or the configured type);
    // wrap them only when an upcast is actually needed.
    const TypeDescriptor* from = &type_;
    const TypeDescriptor* to = current_target_ ? current_target_ : &type_;
    if (from == to) {
        m.getter = std::move(get);
        m.setter = std::move(set);
        m.validate = std::move(validate);
    } else {
        auto up = [from, to](void* self) -> void* {
            void* p = from->upcast(self, to);
            if (!p) throw std::logic_error("cannot view " + from->name + " as " + to->name);
            return p;
        };
        m.getter = [up, get = std::move(get)](void* self) { return get(up(self)); };
        if (set) m.setter = [up, set = std::move(set)](void* self, const Value& v) { set(up(self), v); };
        if (validate) m.validate = [up, validate = std::move(validate)](void* self, const Value& v) { return validate(up(self), v); };
    }
    for (auto& existing : members_) {
        if (existing.name == m.name) {
            existing = std::move(m);  // replace in place (keeps the declaration order)
            return *this;
        }
    }
    members_.push_back(std::move(m));
    return *this;
}

std::function<void*(void*)> MemberTable::upcaster() const {
    const TypeDescriptor* from = &type_;
    const TypeDescriptor* to = current_target_ ? current_target_ : &type_;
    if (from == to) return {};
    return [from, to](void* self) -> void* {
        void* p = from->upcast(self, to);
        if (!p) throw std::logic_error("cannot view " + from->name + " as " + to->name);
        return p;
    };
}

MemberTable& MemberTable::method(std::string name, std::vector<ParamDescriptor> params, const TypeDescriptor* returns,
                                 std::function<Value(void* self, const std::vector<Value>& args)> invoke, std::string description,
                                 CallValidator validate) {
    if (name.empty()) throw std::logic_error("method name required");
    if (!invoke) throw std::logic_error("method '" + name + "' needs an implementation");
    MethodDescriptor m;
    m.name = std::move(name);
    m.params = std::move(params);
    m.result = returns;
    m.description = std::move(description);
    if (auto up = upcaster()) {
        m.invoke = [up, invoke = std::move(invoke)](void* self, const std::vector<Value>& args) { return invoke(up(self), args); };
        if (validate)
            m.validate = [up, validate = std::move(validate)](void* self, const std::vector<Value>& args) { return validate(up(self), args); };
    } else {
        m.invoke = std::move(invoke);
        m.validate = std::move(validate);
    }
    for (auto& existing : methods_) {
        if (existing.name == m.name && existing.params.size() == m.params.size()) {
            existing = std::move(m);
            return *this;
        }
    }
    methods_.push_back(std::move(m));
    return *this;
}

MemberTable& MemberTable::hide_methods(const std::vector<std::string>& names) {
    for (auto& n : names)
        if (!std::any_of(methods_.begin(), methods_.end(), [&](const MethodDescriptor& m) { return m.name == n; }))
            throw std::logic_error("type " + type_.name + " has no callable method '" + n + "' to hide");
    methods_.erase(std::remove_if(methods_.begin(), methods_.end(),
                                  [&](const MethodDescriptor& m) { return std::find(names.begin(), names.end(), m.name) != names.end(); }),
                   methods_.end());
    return *this;
}

MemberTable& MemberTable::map(std::string name, const TypeDescriptor* value_type, VirtualMapAdapter::Spec spec, std::string description) {
    if (!spec.keys || !spec.get) throw std::logic_error("virtual map '" + name + "' needs keys and get");
    if (spec.set && type_.kind == TypeKind::Struct && !writes_through_)
        throw std::logic_error("virtual map setters on value type " + type_.name + " would write to a copy");
    spec.type = registry_.map_type(builtin::string_type(), value_type, false);
    auto shared = std::make_shared<const VirtualMapAdapter::Spec>(std::move(spec));
    // The map member itself is read-only (entries are written through the map); `self` is already the
    // adapter-target pointer because add() upcasts for us.
    return add(std::move(name), shared->type, [shared](void* self) { return Value::map(std::make_shared<VirtualMapAdapter>(self, shared)); }, {},
               {}, std::move(description));
}

MemberTable& MemberTable::map(std::string name, const TypeDescriptor* value_type, VirtualMapAdapter::Keys keys, VirtualMapAdapter::Get get,
                              VirtualMapAdapter::Set set, VirtualMapAdapter::Validate validate, std::string description,
                              VirtualMapAdapter::Has has) {
    VirtualMapAdapter::Spec spec;
    spec.keys = std::move(keys);
    spec.get = std::move(get);
    spec.set = std::move(set);
    spec.validate = std::move(validate);
    spec.has = std::move(has);
    return map(std::move(name), value_type, std::move(spec), std::move(description));
}

MemberTable& MemberTable::mixed_map(std::string name, VirtualMapAdapter::Keys keys, VirtualMapAdapter::EntryType entry_type,
                                    VirtualMapAdapter::Get get, VirtualMapAdapter::Set set, VirtualMapAdapter::Validate validate,
                                    std::string description) {
    if (!entry_type) throw std::logic_error("mixed virtual map '" + name + "' needs entry types");
    VirtualMapAdapter::Spec spec;
    spec.keys = std::move(keys);
    spec.get = std::move(get);
    spec.set = std::move(set);
    spec.validate = std::move(validate);
    spec.entry_type = std::move(entry_type);
    return map(std::move(name), builtin::any_type(), std::move(spec), std::move(description));
}

MemberTable& MemberTable::hide_where(const std::function<bool(const MemberDescriptor&)>& hide) {
    members_.erase(std::remove_if(members_.begin(), members_.end(), [&](const MemberDescriptor& m) { return hide(m); }), members_.end());
    return *this;
}

MemberTable& MemberTable::copy_on_read(const std::vector<std::string>& names) {
    for (auto& n : names) member(n).copy_on_read = true;
    return *this;
}

MemberTable& MemberTable::writes_through() {
    if (type_.kind != TypeKind::Struct) throw std::logic_error(type_.name + " is not a value type; writes_through applies to handle structs");
    writes_through_ = true;
    return *this;
}

MemberTable& MemberTable::identity(const std::string& name) {
    member(name);
    identity_name_ = name;
    return *this;
}

MemberTable& MemberTable::display(const std::string& name) {
    member(name);
    display_name_ = name;
    return *this;
}

// ------------------------------------------------------------------ VirtualMapAdapter

void VirtualMapAdapter::for_each(const std::function<bool(const Value&, const Value&)>& fn) const {
    const std::vector<std::string> keys = spec_->keys(owner_);
    for (auto& k : keys)
        if (!fn(Value::string(k), spec_->get(owner_, k))) return;
}

MapLookup VirtualMapAdapter::find(const Value& key, Value& out) const {
    if (!accepts_key(key)) return MapLookup::BadKey;
    const std::string k = key_text(key);
    bool present;
    if (spec_->has) {
        present = spec_->has(owner_, k);
    } else {
        const std::vector<std::string> keys = spec_->keys(owner_);
        present = std::find(keys.begin(), keys.end(), k) != keys.end();
    }
    if (!present) return MapLookup::NotFound;
    out = spec_->get(owner_, k);
    return MapLookup::Found;
}

void VirtualMapAdapter::set(const Value& key, const Value& value) {
    if (!spec_->set) throw std::runtime_error("read-only map");
    if (value.is_missing()) throw std::runtime_error("entries of a virtual map cannot be removed");
    spec_->set(owner_, key_text(key), value);
}

void VirtualMapAdapter::erase(const Value&) { throw std::runtime_error("entries of a virtual map cannot be removed"); }

std::string VirtualMapAdapter::validate_entry(const Value& key, const Value& value) const {
    return spec_->validate ? spec_->validate(owner_, key_text(key), value) : std::string();
}

MemberTable& MemberTable::retain(const std::vector<std::string>& names, bool keep) {
    // A misspelt name would silently leave a member exposed (e.g. hide({"Pasword"})): all-or-error.
    for (auto& n : names) member(n);
    members_.erase(std::remove_if(members_.begin(), members_.end(),
                                  [&](const MemberDescriptor& m) {
                                      bool named = std::find(names.begin(), names.end(), m.name) != names.end();
                                      return named != keep;
                                  }),
                   members_.end());
    return *this;
}

MemberTable& MemberTable::hide(const std::vector<std::string>& names) { return retain(names, false); }
MemberTable& MemberTable::only(const std::vector<std::string>& names) { return retain(names, true); }

MemberTable& MemberTable::read_only(const std::vector<std::string>& names) {
    for (auto& n : names) member(n).readonly = true;
    return *this;
}

MemberTable& MemberTable::reference(const std::vector<std::string>& names) {
    for (auto& n : names) member(n).owning = false;
    return *this;
}

MemberTable& MemberTable::explicit_only(const std::vector<std::string>& names) {
    for (auto& n : names) member(n).explicit_only = true;
    return *this;
}

MemberTable& MemberTable::describe(const std::string& name, std::string description) {
    member(name).description = std::move(description);
    return *this;
}

// ------------------------------------------------------------------ ranking and adapted types

int adapter_rank(const TypeAdapter& adapter, const TypeDescriptor& type) {
    if (adapter.target) {
        int d = 0;
        for (const TypeDescriptor* t = &type; t; t = t->base, ++d)
            if (t == adapter.target) return d;
        return -1;
    }
    if (!adapter.interface_name.empty() && type.implements(adapter.interface_name)) return 10000;
    return -1;
}

Json EncodeContext::encode_value(const Value& v) const { return encoder_->encode(v, depth_ + 1); }
Json EncodeContext::summary(const Value& object) const { return encoder_->summary(object); }

namespace detail {

std::vector<const TypeAdapter*> applicable_adapters(const std::vector<std::unique_ptr<TypeAdapter>>& all, const TypeDescriptor& type) {
    std::vector<const TypeAdapter*> out;
    // Engine-internal values (projected rows, map entries) never have adapters.
    if (&type == builtin::row_type() || &type == builtin::entry_type()) return out;
    struct Hit {
        const TypeAdapter* a;
        int rank;
        size_t index;
    };
    std::vector<Hit> hits;
    for (size_t i = 0; i < all.size(); ++i) {
        int r = adapter_rank(*all[i], type);
        if (r >= 0) hits.push_back({all[i].get(), r, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
        if (x.rank != y.rank) return x.rank < y.rank;
        if (x.a->priority != y.a->priority) return x.a->priority > y.a->priority;
        return x.index < y.index;
    });
    for (auto& h : hits) out.push_back(h.a);
    return out;
}

bool type_excluded(const TypeDescriptor* t, const TypeExclusion& excluded, int depth) {
    if (!t || depth > 16) return false;
    if (excluded(*t)) return true;
    if (t->kind == TypeKind::List || t->kind == TypeKind::Map)
        return type_excluded(t->element, excluded, depth + 1) || type_excluded(t->key, excluded, depth + 1);
    return false;
}

bool excludes_members(const TypeDescriptor& type, const TypeExclusion& excluded) {
    if (!excluded || (type.kind != TypeKind::Object && type.kind != TypeKind::Struct)) return false;
    return std::any_of(type.members.begin(), type.members.end(), [&](const MemberDescriptor& m) { return type_excluded(m.type, excluded); });
}

std::unique_ptr<AdaptedType> build_adapted(TypeRegistry& registry, const TypeDescriptor& type,
                                           const std::vector<std::unique_ptr<TypeAdapter>>& adapters, const TypeExclusion& excluded) {
    std::vector<const TypeAdapter*> applicable = applicable_adapters(adapters, type);
    const bool shaped = type.kind == TypeKind::Object || type.kind == TypeKind::Struct;
    // Engine-wide type exclusion first (spec §13 Exclusion): one remove_if, a size compare tells whether it applied.
    std::vector<MemberDescriptor> kept;
    bool excludes = false;
    if (excluded && shaped) {
        kept = type.members;
        kept.erase(std::remove_if(kept.begin(), kept.end(), [&](const MemberDescriptor& m) { return type_excluded(m.type, excluded); }),
                   kept.end());
        excludes = kept.size() != type.members.size();
    }
    if (applicable.empty() && !excludes) return nullptr;  // the descriptor is used directly
    auto a = std::make_unique<AdaptedType>();
    a->adapters = std::move(applicable);
    for (const TypeAdapter* ad : a->adapters) {
        if (ad->encode) a->by_cap[static_cast<size_t>(Capability::Encode)].push_back(ad);
        if (ad->coerce) a->by_cap[static_cast<size_t>(Capability::Coerce)].push_back(ad);
        if (ad->equals) a->by_cap[static_cast<size_t>(Capability::Equals)].push_back(ad);
        if (ad->compare) a->by_cap[static_cast<size_t>(Capability::Compare)].push_back(ad);
        if (ad->stable_id) a->by_cap[static_cast<size_t>(Capability::StableId)].push_back(ad);
        if (ad->is_alive) a->by_cap[static_cast<size_t>(Capability::IsAlive)].push_back(ad);
        if (ad->components) a->by_cap[static_cast<size_t>(Capability::Components)].push_back(ad);
        if (a->description.empty() && !ad->description.empty()) a->description = ad->description;
        if (!a->literal_schema && ad->literal_schema) a->literal_schema = ad->literal_schema;
    }
    if (a->description.empty()) a->description = type.description;
    bool configures = excludes;
    for (const TypeAdapter* ad : a->adapters)
        if (ad->configure_members) configures = true;
    if (configures && shaped) {
        a->owns_members = true;
        a->members = excludes ? std::move(kept) : type.members;
        a->methods = type.methods;
        MemberTable table(registry, type, a->members, a->methods);
        // Least → most specific, so a subclass adapter refines what a base adapter configured.
        for (size_t i = a->adapters.size(); i-- > 0;) {
            const TypeAdapter* ad = a->adapters[i];
            if (!ad->configure_members) continue;
            table.set_current_target(ad->target);
            ad->configure_members(table);
        }
        a->writes_through = table.is_writes_through();
        // Identity / display are recomputed after adapters (pointers into the final member list), never
        // explicit-only; an adapter may choose them by name.
        auto by_name = [&](const std::string& n) -> const MemberDescriptor* {
            for (auto& m : a->members)
                if (m.name == n) return m.explicit_only ? nullptr : &m;
            return nullptr;
        };
        a->id = table.identity_name().empty() ? type.id_member_in(a->members) : by_name(table.identity_name());
        a->display = table.display_name().empty() ? type.display_member_in(a->members) : by_name(table.display_name());
        for (auto& m : a->members) {
            if (m.explicit_only) a->has_explicit_only = true;
            else a->implicit.push_back(m);
        }
        if (!a->has_explicit_only) a->implicit.clear();
    }
    return a;
}

}  // namespace detail
}  // namespace rql
