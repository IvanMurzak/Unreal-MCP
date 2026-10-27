#include "rql/reflection.hpp"

#include <cmath>

namespace rql {

// ------------------------------------------------------------------ builtins

namespace builtin {
namespace {
TypeDescriptor make_scalar(const char* name, const char* friendly, ScalarKind k, double lo = 0, double hi = 0) {
    TypeDescriptor d;
    d.name = name;
    d.friendly = friendly;
    d.kind = TypeKind::Scalar;
    d.scalar = k;
    d.int_min = lo;
    d.int_max = hi;
    d.registered = true;
    return d;
}

struct Builtins {
    TypeDescriptor b = make_scalar("Boolean", "bool", ScalarKind::Bool);
    TypeDescriptor s = make_scalar("String", "string", ScalarKind::String);
    TypeDescriptor d = make_scalar("Double", "double", ScalarKind::Double);
    TypeDescriptor f = make_scalar("Single", "float", ScalarKind::Single);
    TypeDescriptor any = make_scalar("Object", "object", ScalarKind::Any);
    TypeDescriptor i8 = make_scalar("SByte", "sbyte", ScalarKind::Integer, -128.0, 127.0);
    TypeDescriptor u8 = make_scalar("Byte", "byte", ScalarKind::Integer, 0, 255.0);
    TypeDescriptor i16 = make_scalar("Int16", "short", ScalarKind::Integer, -32768.0, 32767.0);
    TypeDescriptor u16 = make_scalar("UInt16", "ushort", ScalarKind::Integer, 0, 65535.0);
    TypeDescriptor i32 = make_scalar("Int32", "int", ScalarKind::Integer, -2147483648.0, 2147483647.0);
    TypeDescriptor u32 = make_scalar("UInt32", "uint", ScalarKind::Integer, 0, 4294967295.0);
    // 64-bit ranges are limited to what a double represents exactly enough for range checks.
    TypeDescriptor i64 = make_scalar("Int64", "long", ScalarKind::Integer, -9223372036854775808.0, 9223372036854774784.0);
    TypeDescriptor u64 = make_scalar("UInt64", "ulong", ScalarKind::Integer, 0, 18446744073709549568.0);
    TypeDescriptor entry;
    TypeDescriptor row;

    Builtins() {
        entry.name = entry.friendly = "MapEntry";
        entry.kind = TypeKind::Object;
        entry.registered = true;
        MemberDescriptor k;
        k.name = "Key";
        k.type = &any;
        k.getter = [](void* self) { return static_cast<EntryData*>(self)->key; };
        MemberDescriptor v;
        v.name = "Value";
        v.type = &any;
        v.nullable = true;
        v.getter = [](void* self) { return static_cast<EntryData*>(self)->value; };
        entry.members = {k, v};
        entry.id_explicit = entry.display_explicit = true;
        row.name = row.friendly = "RowValue";
        row.kind = TypeKind::Object;
        row.registered = true;
        row.id_explicit = row.display_explicit = true;
    }
};

Builtins& all() {
    static Builtins b;
    return b;
}
}  // namespace

const TypeDescriptor* bool_type() { return &all().b; }
const TypeDescriptor* string_type() { return &all().s; }
const TypeDescriptor* double_type() { return &all().d; }
const TypeDescriptor* single_type() { return &all().f; }
const TypeDescriptor* any_type() { return &all().any; }
const TypeDescriptor* entry_type() { return &all().entry; }
const TypeDescriptor* row_type() { return &all().row; }
const TypeDescriptor* number_type(bool integral, bool is_signed, size_t size) {
    auto& b = all();
    if (!integral) return size == 4 ? &b.f : &b.d;
    switch (size) {
        case 1: return is_signed ? &b.i8 : &b.u8;
        case 2: return is_signed ? &b.i16 : &b.u16;
        case 4: return is_signed ? &b.i32 : &b.u32;
        default: return is_signed ? &b.i64 : &b.u64;
    }
}
}  // namespace builtin

namespace detail {
bool key_number(const Value& key, double& out) {
    if (key.is_number()) { out = key.as_number(); return true; }
    if (key.is_string()) return parse_double(key.as_string(), out);
    return false;
}
bool key_integral(const Value& key, double lo, double hi, double& out) {
    double d = 0;
    if (!key_number(key, d) || !std::isfinite(d)) return false;
    d = std::nearbyint(d);  // Convert.ChangeType rounds half to even
    if (d < lo || d > hi) return false;
    out = d;
    return true;
}
}  // namespace detail

// ------------------------------------------------------------------ TypeDescriptor

const MemberDescriptor* find_member_in(const std::vector<MemberDescriptor>& members, const std::string& name) {
    for (auto& m : members)
        if (m.name == name) return &m;
    const MemberDescriptor* hit = nullptr;
    for (auto& m : members) {
        if (iequals(m.name, name)) {
            if (hit) return nullptr;  // ambiguous
            hit = &m;
        }
    }
    return hit;
}

bool is_instance_of(const Value& v, const TypeDescriptor* type) {
    if (!type) return false;
    if (type->kind != TypeKind::Scalar && !v.type()) return false;
    switch (type->kind) {
        case TypeKind::Struct: return v.is_struct() && v.type() == type;
        case TypeKind::Enum: return v.is_enum() && v.type() == type;
        case TypeKind::Object: return v.is_object() && v.type()->derives_from(type);
        case TypeKind::Scalar:
            switch (type->scalar) {
                case ScalarKind::Any: return true;
                case ScalarKind::Bool: return v.is_bool();
                case ScalarKind::String: return v.is_string();
                // Numbers carry their native scalar type; untyped literals are doubles.
                default: return v.is_number() && (v.type() ? v.type() == type : type == builtin::double_type());
            }
        default: return false;
    }
}

const MemberDescriptor* TypeDescriptor::find(const std::string& member) const { return find_member_in(members, member); }

bool TypeDescriptor::implements(const std::string& interface_name) const {
    for (const TypeDescriptor* t = this; t; t = t->base)
        for (auto& n : t->also_is)
            if (iequals(n, interface_name)) return true;
    return false;
}

bool TypeDescriptor::is_a(const std::string& type_name) const {
    for (const TypeDescriptor* t = this; t; t = t->base)
        if (iequals(t->name, type_name)) return true;
    return implements(type_name);
}

bool TypeDescriptor::derives_from(const TypeDescriptor* other) const {
    for (const TypeDescriptor* t = this; t; t = t->base)
        if (t == other) return true;
    return false;
}

void* TypeDescriptor::upcast(void* p, const TypeDescriptor* target) const {
    const TypeDescriptor* t = this;
    while (t && t != target) {
        if (!t->to_base) return nullptr;
        p = t->to_base(p);
        t = t->base;
    }
    return t ? p : nullptr;
}

void* TypeDescriptor::downcast_from(const TypeDescriptor* base_type, void* p) const {
    std::vector<const TypeDescriptor*> chain;  // this ... (excluding base_type)
    const TypeDescriptor* t = this;
    while (t && t != base_type) {
        chain.push_back(t);
        t = t->base;
    }
    if (!t) return nullptr;
    for (size_t i = chain.size(); i-- > 0;) {
        if (!chain[i]->from_base) return nullptr;
        p = chain[i]->from_base(p);
    }
    return p;
}

// Identity and display are read implicitly (every $ref and summary), so never explicit-only members (spec §13).
const MemberDescriptor* TypeDescriptor::id_member_in(const std::vector<MemberDescriptor>& list) const {
    if (kind != TypeKind::Object) return nullptr;
    if (id_explicit) {
        if (id_name.empty()) return nullptr;
        for (auto& m : list)
            if (m.name == id_name) return m.explicit_only ? nullptr : &m;
        return nullptr;
    }
    for (auto& m : list) {
        if (!m.explicit_only && m.name == "Id" && m.type && m.type->kind == TypeKind::Scalar &&
            (m.type->scalar == ScalarKind::Integer || m.type->scalar == ScalarKind::String))
            return &m;
    }
    return nullptr;
}

const MemberDescriptor* TypeDescriptor::display_member_in(const std::vector<MemberDescriptor>& list) const {
    if (kind != TypeKind::Object) return nullptr;
    if (display_explicit) {
        if (display_name.empty()) return nullptr;
        for (auto& m : list)
            if (m.name == display_name) return m.explicit_only ? nullptr : &m;
        return nullptr;
    }
    for (auto& m : list)
        if (!m.explicit_only && m.name == "Name" && m.type && m.type->scalar == ScalarKind::String) return &m;
    return nullptr;
}

bool TypeDescriptor::enum_name(int64_t v, std::string& out) const {
    for (auto& e : enum_values) {
        if (e.second == v) {
            out = e.first;
            return true;
        }
    }
    return false;
}

bool TypeDescriptor::enum_defined(int64_t v) const {
    for (auto& e : enum_values)
        if (e.second == v) return true;
    return false;
}

namespace {

/// An entry name without its entry prefix (text up to and including the first '_'); empty when it has none.
std::string without_entry_prefix(const std::string& name) {
    size_t i = name.find('_');
    return i != std::string::npos && i > 0 && i + 1 < name.size() ? name.substr(i + 1) : std::string();
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

}  // namespace

TypeDescriptor::EnumMatch TypeDescriptor::enum_resolve(const std::string& text) const {
    EnumMatch m;
    size_t b = 0, e = text.size();
    while (b < e && is_space(text[b])) ++b;
    while (e > b && is_space(text[e - 1])) --e;
    const std::string s = text.substr(b, e - b);
    // Integer text is the underlying value.
    size_t digits = !s.empty() && (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (digits < s.size() && s.find_first_not_of("0123456789", digits) == std::string::npos) {
        try {
            m.value = std::stoll(s[0] == '+' ? s.substr(1) : s);
            m.status = EnumMatch::Found;
            return m;
        } catch (...) {  // out of range: not a value
        }
    }
    // One step: the entries whose name (or alias, not in the prefix step) matches; names listed in declaration order.
    std::vector<std::pair<std::string, int64_t>> found;
    auto step = [&](const std::string& t, bool ignore_case, bool prefix) {
        found.clear();
        auto add = [&](const std::string& entry_name, int64_t v) {
            for (auto& f : found)
                if (f.first == entry_name) return;
            found.emplace_back(entry_name, v);
        };
        auto eq = [&](const std::string& n) { return !n.empty() && (ignore_case ? iequals(n, t) : n == t); };
        for (auto& ev : enum_values)
            if (eq(prefix ? without_entry_prefix(ev.first) : ev.first)) add(ev.first, ev.second);
        if (!prefix)
            for (auto& a : enum_aliases)
                if (eq(a.first)) {
                    std::string entry;
                    if (enum_name(a.second, entry)) add(entry, a.second);
                }
        return !found.empty();
    };
    bool any = step(s, false, false);
    if (!any) {
        size_t q = s.rfind("::");
        size_t cut = q != std::string::npos ? q + 2 : s.rfind('.') + 1;  // npos + 1 == 0: unqualified
        if (cut > 0 && cut < s.size()) {
            const std::string part = s.substr(cut);
            any = step(part, false, false) || step(part, true, false) || step(part, true, true);
        }
    }
    if (!any) any = step(s, true, false) || step(s, true, true);
    if (!any) {
        m.why = enum_expected();
        return m;
    }
    // Distinct entries: names sharing one value are one entry.
    for (auto& f : found) {
        if (f.second != found[0].second) {
            m.status = EnumMatch::Ambiguous;
            m.why = "\"" + s + "\" is ambiguous for " + name + ": ";
            for (size_t i = 0; i < found.size(); ++i) m.why += (i ? ", " : "") + found[i].first;
            return m;
        }
    }
    m.status = EnumMatch::Found;
    m.value = found[0].second;
    return m;
}

bool TypeDescriptor::enum_parse(const std::string& text, int64_t& out) const {
    EnumMatch m = enum_resolve(text);
    if (m.status != EnumMatch::Found) return false;
    out = m.value;
    return true;
}

std::string TypeDescriptor::enum_names() const {
    std::string s;
    for (size_t i = 0; i < enum_values.size(); ++i) {
        if (i) s += ", ";
        s += enum_values[i].first;
    }
    return s;
}

std::string TypeDescriptor::friendly_name() const {
    if (kind == TypeKind::List)
        return "List<" + (element ? element->friendly_name() : std::string("object")) +
               (element_nullable && element && element->is_value_type() ? "?" : "") + ">";
    if (kind == TypeKind::Map)
        return "Dictionary<" + (key ? key->friendly_name() : std::string("object")) + ", " +
               (element ? element->friendly_name() : std::string("object")) +
               (element_nullable && element && element->is_value_type() ? "?" : "") + ">";
    return friendly.empty() ? name : friendly;
}

std::string TypeDescriptor::signature(const MethodDescriptor& m) const {
    std::string s = m.name + "(";
    for (size_t i = 0; i < m.params.size(); ++i) {
        if (i) s += ", ";
        auto& p = m.params[i];
        s += (p.type ? p.type->friendly_name() : std::string("object")) + (p.nullable && p.type && p.type->is_value_type() ? "?" : "") + " " + p.name;
    }
    return s + ")";
}

// ------------------------------------------------------------------ TypeRegistry

TypeDescriptor* TypeRegistry::descriptor_for(TypeKey t) {
    auto it = by_type_.find(t);
    if (it != by_type_.end()) return it->second;
    owned_.push_back(std::make_unique<TypeDescriptor>());
    TypeDescriptor* d = owned_.back().get();
    d->name = "?";
    d->native_key = t;
    by_type_.emplace(t, d);
    return d;
}

const TypeDescriptor* TypeRegistry::find_native(TypeKey t) const {
    auto it = by_type_.find(t);
    return it == by_type_.end() ? nullptr : it->second;
}

const TypeDescriptor* TypeRegistry::list_type(const TypeDescriptor* element, bool element_nullable) {
    auto key = std::make_pair(element, element_nullable);
    auto it = lists_.find(key);
    if (it != lists_.end()) return it->second;
    owned_.push_back(std::make_unique<TypeDescriptor>());
    TypeDescriptor* d = owned_.back().get();
    d->name = "List";
    d->kind = TypeKind::List;
    d->element = element;
    d->element_nullable = element_nullable;
    d->registered = true;
    lists_.emplace(key, d);
    return d;
}

const TypeDescriptor* TypeRegistry::map_type(const TypeDescriptor* key, const TypeDescriptor* value, bool value_nullable) {
    auto k = std::make_tuple(key, value, value_nullable);
    auto it = maps_.find(k);
    if (it != maps_.end()) return it->second;
    owned_.push_back(std::make_unique<TypeDescriptor>());
    TypeDescriptor* d = owned_.back().get();
    d->name = "Dictionary";
    d->kind = TypeKind::Map;
    d->key = key;
    d->element = value;
    d->element_nullable = value_nullable;
    d->registered = true;
    maps_.emplace(k, d);
    return d;
}

std::vector<const TypeDescriptor*> TypeRegistry::registered() const { return order_; }

Value TypeRegistry::finish_wrap(const TypeDescriptor* d, void* p, std::shared_ptr<void> keep) {
    if (d->kind == TypeKind::Struct && d->copy) return Value::structure(d, d->copy(p));
    return Value::object(d, p, std::move(keep));
}

}  // namespace rql
