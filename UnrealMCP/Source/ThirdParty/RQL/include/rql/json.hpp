// Minimal, dependency-free JSON value / parser / writer used by the RQL C++ runtime.
// - UTF-8 strings (input is validated), \uXXXX escapes incl. surrogate pairs.
// - Numbers are doubles; the parser remembers whether the literal was written as an integer
//   (no fraction / exponent), which the plan codec needs to mirror strict integer checks.
// - Objects keep insertion order; duplicate keys: the LAST occurrence wins on lookup.
// - Output: integral numbers (|d| < 2^53) without fraction, others in shortest round-trip form.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rql {

class JsonParseError : public std::runtime_error {
public:
    JsonParseError(const std::string& message, size_t offset)
        : std::runtime_error(message + " at offset " + std::to_string(offset)), offset_(offset) {}
    size_t offset() const { return offset_; }

private:
    size_t offset_;
};

class Json {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(double d) : type_(Type::Number), n_(d) {}
    Json(int v) : type_(Type::Number), n_(v), int_text_(true) {}
    Json(int64_t v) : type_(Type::Number), n_(static_cast<double>(v)), int_text_(true) {}
    Json(const char* s) : type_(Type::String), s_(s) {}
    Json(std::string s) : type_(Type::String), s_(std::move(s)) {}
    Json(std::string_view s) : type_(Type::String), s_(s) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }
    static Json number(double d, bool integral_text = false) { Json j(d); j.int_text_ = integral_text; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const { return b_; }
    double as_number() const { return n_; }
    /// True when the number literal had no fraction/exponent in the parsed text (or was built from an int).
    bool integral_text() const { return int_text_; }
    const std::string& as_string() const { return s_; }

    /// Array elements / object values.
    size_t size() const { return items_.size(); }
    const Json& at(size_t i) const { return items_[i]; }
    Json& at(size_t i) { return items_[i]; }
    const std::vector<Json>& items() const { return items_; }
    /// Object keys (parallel to items()).
    const std::string& key(size_t i) const { return keys_[i]; }
    const std::vector<std::string>& keys() const { return keys_; }

    void push(Json v) { items_.push_back(std::move(v)); }
    /// Object: replace the last value with this key, or append.
    Json& set(std::string_view key, Json v);
    /// Object: append without checking duplicates (parser use).
    void append(std::string key, Json v) { keys_.push_back(std::move(key)); items_.push_back(std::move(v)); }
    /// Object lookup (last occurrence wins). nullptr if absent or not an object.
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    bool erase(std::string_view key);

    /// Serialize. indent < 0: compact.
    std::string dump(int indent = -1) const;
    void dump_to(std::string& out, int indent = -1, int level = 0) const;

    /// max_depth bounds container nesting (hostile inputs cannot overflow the stack).
    static Json parse(std::string_view text, int max_depth = 512);  // throws JsonParseError
    static bool try_parse(std::string_view text, Json& out, std::string& error, int max_depth = 512) noexcept;

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    bool int_text_ = false;
    std::string s_;
    std::vector<std::string> keys_;
    std::vector<Json> items_;
};

/// Structural equality: numbers by value, object key order ignored (duplicate keys not expected).
bool json_equal(const Json& a, const Json& b);

/// Append a JSON string literal (quoted, escaped; non-ASCII kept as UTF-8).
void json_escape_to(std::string& out, std::string_view s);

/// Shortest decimal text that round-trips to the same double (no locale dependence).
/// Integral values with |d| < 2^53 print without fraction. Non-finite → "NaN"/"Infinity"/"-Infinity".
std::string format_double(double d);
/// Shortest round-trip text of a float (single precision), e.g. 0.1f → "0.1".
std::string format_float(float f);
/// .NET "R"-style text (used for ToText/keys/ids): shortest round-trip digits, fixed notation for
/// decimal exponents in (-5, 15), otherwise d.dddE+XX. Non-finite → NaN/Infinity/-Infinity.
std::string format_number_text(double d);
/// Locale-independent strict decimal parse of [text]; false if not a full number.
bool parse_double(std::string_view text, double& out);

}  // namespace rql
