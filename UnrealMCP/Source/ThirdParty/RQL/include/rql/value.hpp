// Runtime values flowing through the executor (spec §1.1). A Value is a small tagged struct that
// refers to LIVE host data: objects by pointer (+ descriptor), lists/maps through adapter
// interfaces, structs as owned copies (write-back is done by the mutator).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rql {

class TypeDescriptor;
class Value;

/// Ordered collection adapter (std::vector, arrays, TArray, custom containers...).
class IListAdapter {
public:
    virtual ~IListAdapter() = default;
    virtual size_t size() const = 0;
    virtual Value at(size_t i) const = 0;
    /// List type descriptor (List<T>); nullptr for runtime-built lists (literals, auto-map results).
    virtual const TypeDescriptor* type() const = 0;
    /// Stable identity of the underlying container (for cycles/distinct/==).
    virtual const void* identity() const { return this; }
    virtual bool writable() const { return false; }
    virtual void set(size_t i, const Value& v);
    /// True for runtime-owned lists (no live view into host data); used instead of RTTI.
    virtual bool owned() const { return false; }
};

enum class MapLookup : uint8_t { BadKey, NotFound, Found };

/// Key→value collection adapter (std::map, unordered_map, TMap...). Iteration MUST be deterministic.
class IMapAdapter {
public:
    virtual ~IMapAdapter() = default;
    virtual size_t size() const = 0;
    /// Visit entries in iteration order; return false from fn to stop.
    virtual void for_each(const std::function<bool(const Value& key, const Value& value)>& fn) const = 0;
    /// Look up a key after coercing it to the native key type (BadKey when not coercible).
    virtual MapLookup find(const Value& key, Value& out) const = 0;
    virtual const TypeDescriptor* type() const = 0;
    virtual const void* identity() const { return this; }
    virtual bool writable() const { return false; }
    virtual bool accepts_key(const Value& key) const;
    virtual void set(const Value& key, const Value& value);
    virtual void erase(const Value& key);
    /// True for runtime-owned maps (no live view into host data); used instead of RTTI.
    virtual bool owned() const { return false; }
    /// Rejection reason for assigning an (already coerced) value to a key, empty = accept. Runs while a
    /// mutation is planned, before any write (spec §13 virtual maps).
    virtual std::string validate_entry(const Value& key, const Value& value) const;
    /// Declared value type of one entry (mixed-type maps), nullptr = the map's value type.
    virtual const TypeDescriptor* entry_type(const Value& key) const;
    /// Writes only replace EXISTING entries (virtual maps): an absent key is rejected while planning,
    /// because a rollback could not remove it again.
    virtual bool fixed_keys() const { return false; }
    /// The value of an object literal { key: expr } (spec §4.5): coerced into maps / constructed objects on writes.
    virtual bool object_literal() const { return false; }
};

struct ErrorInfo {
    std::string code;
    std::string detail;
};

struct EntryData;
struct RowData;

class Value {
public:
    enum class Kind : uint8_t { Missing, Null, Unknown, Error, Bool, Number, String, Enum, Object, Struct, List, Map, Entry, Row };

    Value() = default;  // missing

    static Value missing() { return Value(); }
    static Value null() { Value v; v.kind_ = Kind::Null; return v; }
    static Value unknown() { Value v; v.kind_ = Kind::Unknown; return v; }
    static Value error(std::string code, std::string detail = {});
    static Value boolean(bool b) { Value v; v.kind_ = Kind::Bool; v.b_ = b; return v; }
    /// type: scalar descriptor of the native member type (Int32, Single, Double...); nullptr = double literal.
    static Value number(double d, const TypeDescriptor* type = nullptr) { Value v; v.kind_ = Kind::Number; v.num_ = d; v.type_ = type; return v; }
    static Value string(std::string s) { Value v; v.kind_ = Kind::String; v.str_ = std::move(s); return v; }
    static Value enumeration(const TypeDescriptor* type, int64_t value) { Value v; v.kind_ = Kind::Enum; v.type_ = type; v.int_ = value; return v; }
    /// ptr must point to the object as its (most-derived registered) descriptor type.
    static Value object(const TypeDescriptor* type, void* ptr, std::shared_ptr<void> keep_alive = {});
    static Value structure(const TypeDescriptor* type, std::shared_ptr<void> data);
    static Value list(std::shared_ptr<IListAdapter> list);
    static Value map(std::shared_ptr<IMapAdapter> map);
    static Value list_of(std::vector<Value> items);  // owned list
    static Value entry(Value key, Value value);
    static Value row(std::shared_ptr<const std::vector<std::string>> columns, std::vector<Value> values);

    Kind kind() const { return kind_; }
    bool is_missing() const { return kind_ == Kind::Missing; }
    bool is_null() const { return kind_ == Kind::Null; }
    bool is_unknown() const { return kind_ == Kind::Unknown; }
    bool is_error() const { return kind_ == Kind::Error; }
    /// missing, error or unknown.
    bool is_absent() const { return kind_ == Kind::Missing || kind_ == Kind::Error || kind_ == Kind::Unknown; }
    bool is_bool() const { return kind_ == Kind::Bool; }
    bool is_number() const { return kind_ == Kind::Number; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_enum() const { return kind_ == Kind::Enum; }
    bool is_object() const { return kind_ == Kind::Object; }
    bool is_struct() const { return kind_ == Kind::Struct; }
    bool is_list() const { return kind_ == Kind::List; }
    bool is_map() const { return kind_ == Kind::Map; }
    bool is_entry() const { return kind_ == Kind::Entry; }
    bool is_row() const { return kind_ == Kind::Row; }

    bool as_bool() const { return b_; }
    double as_number() const { return num_; }
    /// The number came from a single-precision (float) member: compare/encode in float precision.
    bool is_single() const;
    const std::string& as_string() const { return str_; }
    int64_t enum_value() const { return int_; }
    const std::string& error_code() const;
    const std::string& error_detail() const;
    /// Descriptor of objects/structs/enums (dynamic type) and typed numbers; nullptr otherwise.
    const TypeDescriptor* type() const { return type_; }
    void* ptr() const { return ptr_; }
    const std::shared_ptr<void>& holder() const { return holder_; }
    IListAdapter& list() const { return *static_cast<IListAdapter*>(holder_.get()); }
    IMapAdapter& map() const { return *static_cast<IMapAdapter*>(holder_.get()); }
    const EntryData& entry() const;
    const RowData& row() const;

    /// Identity used for cycle detection / distinct / ==: objects (pointer+type), lists, maps,
    /// entries, rows, errors. Values without identity (scalars, structs) return false.
    bool identity(const void*& a, const void*& b) const;

private:
    Kind kind_ = Kind::Missing;
    bool b_ = false;
    double num_ = 0;
    int64_t int_ = 0;
    std::string str_;
    const TypeDescriptor* type_ = nullptr;
    void* ptr_ = nullptr;
    std::shared_ptr<void> holder_;
};

struct EntryData {
    Value key;
    Value value;
};

struct RowData {
    std::shared_ptr<const std::vector<std::string>> columns;
    std::vector<Value> values;
    /// Column index: exact, then case-insensitive; -1 if absent.
    int index_of(const std::string& name) const;
};

/// Runtime-built list (literals, auto-map results, snapshots).
class OwnedList final : public IListAdapter {
public:
    /// mutable_copy: a private copy that may be written by index (copy-on-read write-back, spec §13).
    explicit OwnedList(std::vector<Value> items, bool mutable_copy = false) : items_(std::move(items)), mutable_(mutable_copy) {}
    size_t size() const override { return items_.size(); }
    Value at(size_t i) const override { return items_[i]; }
    const TypeDescriptor* type() const override { return nullptr; }
    bool owned() const override { return true; }
    bool writable() const override { return mutable_; }
    void set(size_t i, const Value& v) override;
    const std::vector<Value>& items() const { return items_; }

private:
    std::vector<Value> items_;
    bool mutable_;
};

/// Runtime-built map (snapshots, coerced map assignments). Keys compare like RQL `==` for strings/numbers.
class OwnedMap final : public IMapAdapter {
public:
    /// literal: the value of an object literal (string keys, spec §4.5); mutable_copy: writable private copy.
    OwnedMap(const TypeDescriptor* type, std::vector<std::pair<Value, Value>> entries, bool literal = false, bool mutable_copy = false)
        : type_(type), entries_(std::move(entries)), literal_(literal), mutable_(mutable_copy) {}
    size_t size() const override { return entries_.size(); }
    void for_each(const std::function<bool(const Value&, const Value&)>& fn) const override;
    MapLookup find(const Value& key, Value& out) const override;
    const TypeDescriptor* type() const override { return type_; }
    bool owned() const override { return true; }
    bool object_literal() const override { return literal_; }
    bool writable() const override { return mutable_; }
    void set(const Value& key, const Value& value) override;
    void erase(const Value& key) override;
    const std::vector<std::pair<Value, Value>>& entries() const { return entries_; }

private:
    const TypeDescriptor* type_;
    std::vector<std::pair<Value, Value>> entries_;
    bool literal_;
    bool mutable_;
    size_t index_of(const Value& key) const;
};

// ---------------------------------------------------------------- helpers

/// Text form used for string concatenation, map keys and ids (.NET-compatible number formatting).
std::string to_text(const Value& v);
/// Short human description for messages: null, missing, "str", type name...
std::string describe_value(const Value& v);
/// Deep copy of list/map views into owned values (used for receipts and rollback).
Value snapshot(const Value& v);
/// Deep owned copy of a list / map (other values unchanged); mutable_copy: writable by index / key.
Value owned_copy(const Value& v, bool mutable_copy);
/// Runtime short type name ($type): descriptor name, String, Double, Boolean, List, Dictionary...
std::string type_name_of(const Value& v);
/// Friendly type name (describe / inline collection sizes): List<Node>, Dictionary<string, double>...
std::string friendly_type_of(const Value& v);

// Case-insensitive (ASCII) helpers; UTF-8 bytes >= 0x80 compare ordinally.
bool iequals(const std::string& a, const std::string& b);
std::string ascii_lower(std::string s);
std::string ascii_upper(std::string s);
/// Length in UTF-16 code units (matches .NET string.Length for valid UTF-8).
size_t utf16_length(const std::string& s);

}  // namespace rql
