#include "rql/value.hpp"

#include <stdexcept>

#include "rql/json.hpp"
#include "rql/reflection.hpp"

namespace rql {

void IListAdapter::set(size_t, const Value&) { throw std::runtime_error("list is not writable"); }
bool IMapAdapter::accepts_key(const Value& key) const {
    Value tmp;
    return find(key, tmp) != MapLookup::BadKey;
}
void IMapAdapter::set(const Value&, const Value&) { throw std::runtime_error("map is not writable"); }
void IMapAdapter::erase(const Value&) { throw std::runtime_error("map is not writable"); }
std::string IMapAdapter::validate_entry(const Value&, const Value&) const { return {}; }
const TypeDescriptor* IMapAdapter::entry_type(const Value&) const { return nullptr; }

void OwnedList::set(size_t i, const Value& v) {
    if (!mutable_) throw std::runtime_error("list is not writable");
    items_.at(i) = v;
}

// ------------------------------------------------------------------ Value

Value Value::error(std::string code, std::string detail) {
    Value v;
    v.kind_ = Kind::Error;
    auto info = std::make_shared<ErrorInfo>();
    info->code = std::move(code);
    info->detail = std::move(detail);
    v.holder_ = info;
    return v;
}

Value Value::object(const TypeDescriptor* type, void* ptr, std::shared_ptr<void> keep_alive) {
    Value v;
    v.kind_ = Kind::Object;
    v.type_ = type;
    v.ptr_ = ptr;
    v.holder_ = std::move(keep_alive);
    return v;
}

Value Value::structure(const TypeDescriptor* type, std::shared_ptr<void> data) {
    Value v;
    v.kind_ = Kind::Struct;
    v.type_ = type;
    v.ptr_ = data.get();
    v.holder_ = std::move(data);
    return v;
}

Value Value::list(std::shared_ptr<IListAdapter> list) {
    Value v;
    v.kind_ = Kind::List;
    v.type_ = list->type();
    v.holder_ = std::move(list);
    return v;
}

Value Value::map(std::shared_ptr<IMapAdapter> map) {
    Value v;
    v.kind_ = Kind::Map;
    v.type_ = map->type();
    v.holder_ = std::move(map);
    return v;
}

Value Value::list_of(std::vector<Value> items) { return list(std::make_shared<OwnedList>(std::move(items))); }

Value Value::entry(Value key, Value value) {
    Value v;
    v.kind_ = Kind::Entry;
    auto e = std::make_shared<EntryData>();
    e->key = std::move(key);
    e->value = std::move(value);
    v.ptr_ = e.get();
    v.type_ = builtin::entry_type();
    v.holder_ = std::move(e);
    return v;
}

Value Value::entry(Value key, Value value, std::shared_ptr<const EntryOrigin> origin) {
    Value v = entry(std::move(key), std::move(value));
    std::static_pointer_cast<EntryData>(v.holder_)->origin = std::move(origin);
    return v;
}

Value Value::row(std::shared_ptr<const std::vector<std::string>> columns, std::vector<Value> values) {
    Value v;
    v.kind_ = Kind::Row;
    auto r = std::make_shared<RowData>();
    r->columns = std::move(columns);
    r->values = std::move(values);
    v.ptr_ = r.get();
    v.type_ = builtin::row_type();
    v.holder_ = std::move(r);
    return v;
}

bool Value::is_single() const { return kind_ == Kind::Number && type_ && type_->scalar == ScalarKind::Single; }

const std::string& Value::error_code() const {
    static const std::string empty;
    return kind_ == Kind::Error ? static_cast<ErrorInfo*>(holder_.get())->code : empty;
}

const std::string& Value::error_detail() const {
    static const std::string empty;
    return kind_ == Kind::Error ? static_cast<ErrorInfo*>(holder_.get())->detail : empty;
}

const EntryData& Value::entry() const { return *static_cast<EntryData*>(holder_.get()); }
const RowData& Value::row() const { return *static_cast<RowData*>(holder_.get()); }

bool Value::identity(const void*& a, const void*& b) const {
    switch (kind_) {
        case Kind::Object: a = ptr_; b = type_; return true;
        case Kind::List: a = list().identity(); b = reinterpret_cast<const void*>(1); return true;
        case Kind::Map: a = map().identity(); b = reinterpret_cast<const void*>(2); return true;
        case Kind::Entry:
        case Kind::Row:
        case Kind::Error: a = holder_.get(); b = reinterpret_cast<const void*>(3); return true;
        case Kind::Missing: a = nullptr; b = reinterpret_cast<const void*>(4); return true;
        case Kind::Unknown: a = nullptr; b = reinterpret_cast<const void*>(5); return true;
        default: return false;
    }
}

int RowData::index_of(const std::string& name) const {
    for (size_t i = 0; i < columns->size(); ++i)
        if ((*columns)[i] == name) return static_cast<int>(i);
    for (size_t i = 0; i < columns->size(); ++i)
        if (iequals((*columns)[i], name)) return static_cast<int>(i);
    return -1;
}

// ------------------------------------------------------------------ OwnedMap

void OwnedMap::for_each(const std::function<bool(const Value&, const Value&)>& fn) const {
    for (auto& kv : entries_)
        if (!fn(kv.first, kv.second)) return;
}

size_t OwnedMap::index_of(const Value& key) const {
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Value& k = entries_[i].first;
        bool eq = false;
        if (k.is_string() && key.is_string()) eq = k.as_string() == key.as_string();
        else if (k.is_number() && key.is_number()) eq = k.as_number() == key.as_number();
        else if (k.is_string() && key.is_number()) eq = k.as_string() == to_text(key);
        else if (k.is_enum() && key.is_enum()) eq = k.type() == key.type() && k.enum_value() == key.enum_value();
        else if (k.is_object() && key.is_object()) eq = k.ptr() == key.ptr();
        if (eq) return i;
    }
    return entries_.size();
}

MapLookup OwnedMap::find(const Value& key, Value& out) const {
    size_t i = index_of(key);
    if (i == entries_.size()) return MapLookup::NotFound;
    out = entries_[i].second;
    return MapLookup::Found;
}

void OwnedMap::set(const Value& key, const Value& value) {
    if (!mutable_) throw std::runtime_error("map is not writable");
    size_t i = index_of(key);
    if (i == entries_.size()) entries_.emplace_back(key, value);
    else entries_[i].second = value;
}

void OwnedMap::erase(const Value& key) {
    if (!mutable_) throw std::runtime_error("map is not writable");
    size_t i = index_of(key);
    if (i < entries_.size()) entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
}

// ------------------------------------------------------------------ helpers

std::string to_text(const Value& v) {
    switch (v.kind()) {
        case Value::Kind::Null:
        case Value::Kind::Missing:
        case Value::Kind::Unknown: return "";
        case Value::Kind::String: return v.as_string();
        case Value::Kind::Bool: return v.as_bool() ? "true" : "false";
        case Value::Kind::Number:
            if (v.is_single()) return format_float(static_cast<float>(v.as_number()));
            return format_number_text(v.as_number());
        case Value::Kind::Enum: {
            std::string n;
            if (v.type()->enum_name(v.enum_value(), n)) return n;
            return std::to_string(v.enum_value());
        }
        case Value::Kind::Error: return "error(" + v.error_code() + ")";
        default: return type_name_of(v);
    }
}

std::string describe_value(const Value& v) {
    switch (v.kind()) {
        case Value::Kind::Null: return "null";
        case Value::Kind::Missing: return "missing";
        case Value::Kind::Unknown: return "unknown";
        case Value::Kind::Error: return "error(" + v.error_code() + ")";
        case Value::Kind::String: return "\"" + v.as_string() + "\"";
        default: return type_name_of(v);
    }
}

std::string type_name_of(const Value& v) {
    switch (v.kind()) {
        case Value::Kind::Bool: return "Boolean";
        case Value::Kind::String: return "String";
        case Value::Kind::Number: return v.type() ? v.type()->name : "Double";
        case Value::Kind::Enum:
        case Value::Kind::Object:
        case Value::Kind::Struct:
        case Value::Kind::Entry:
        case Value::Kind::Row: return v.type()->name;
        case Value::Kind::List: return "List";
        case Value::Kind::Map: return "Dictionary";
        default: return "";
    }
}

std::string friendly_type_of(const Value& v) {
    switch (v.kind()) {
        case Value::Kind::List: return v.type() ? v.type()->friendly_name() : "List<object>";
        case Value::Kind::Map: return v.type() ? v.type()->friendly_name() : "Dictionary<object, object>";
        case Value::Kind::Number:
            return v.type() ? v.type()->friendly_name() : "double";
        case Value::Kind::String: return "string";
        case Value::Kind::Bool: return "bool";
        default: return v.type() ? v.type()->friendly_name() : type_name_of(v);
    }
}

Value owned_copy(const Value& v, bool mutable_copy) {
    if (v.is_list()) {
        std::vector<Value> items;
        auto& l = v.list();
        size_t n = l.size();
        items.reserve(n);
        for (size_t i = 0; i < n; ++i) items.push_back(snapshot(l.at(i)));
        return Value::list(std::make_shared<OwnedList>(std::move(items), mutable_copy));
    }
    if (v.is_map()) {
        std::vector<std::pair<Value, Value>> entries;
        v.map().for_each([&](const Value& k, const Value& val) {
            entries.emplace_back(k, snapshot(val));
            return true;
        });
        return Value::map(std::make_shared<OwnedMap>(v.map().type(), std::move(entries), false, mutable_copy));
    }
    return v;
}

Value snapshot(const Value& v) {
    if ((v.is_list() && v.list().owned()) || (v.is_map() && v.map().owned())) return v;
    return owned_copy(v, false);
}

namespace {
inline char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline char upper_c(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
}  // namespace

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower_c(a[i]) != lower_c(b[i])) return false;
    return true;
}

std::string ascii_lower(std::string s) {
    for (auto& c : s) c = lower_c(c);
    return s;
}

std::string ascii_upper(std::string s) {
    for (auto& c : s) c = upper_c(c);
    return s;
}

size_t utf16_length(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) == 0x80) continue;  // continuation byte
        n += (c >= 0xF0) ? 2 : 1;
    }
    return n;
}

}  // namespace rql
