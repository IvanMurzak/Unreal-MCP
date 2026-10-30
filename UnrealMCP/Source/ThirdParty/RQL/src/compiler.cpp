// Expression compiler: plan expressions → closures over Exec (spec §3–4).
// Mirrors dotnet/Rql.Execution/Compiler.cs.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

#include "exec.hpp"

namespace rql {
namespace detail {

// ------------------------------------------------------------------ small helpers

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) s += sep;
        s += parts[i];
    }
    return s;
}

namespace {

bool icontains(const std::string& hay, const std::string& needle) {
    return ascii_lower(hay).find(ascii_lower(needle)) != std::string::npos;
}

struct ScopeGuard {
    Exec& x;
    Value current;
    PathPtr path;
    int64_t index;
    int64_t count;
    explicit ScopeGuard(Exec& e) : x(e), current(e.current), path(e.current_path), index(e.index), count(e.count) {
        e.outer.push_back(current);
        e.count = -1;  // $count is not defined in collection-method arguments (spec §3)
    }
    ~ScopeGuard() {
        x.outer.pop_back();
        x.current = current;
        x.current_path = path;
        x.index = index;
        x.count = count;
    }
};

int cmp_double(double a, double b) {
    if (a < b) return -1;
    if (a > b) return 1;
    if (a == b) return 0;
    if (std::isnan(a)) return std::isnan(b) ? 0 : -1;
    return 1;
}

}  // namespace

size_t osa_distance(const std::string& a, const std::string& b) {
    // Three rolling rows: the transposition step looks two rows back.
    std::vector<size_t> back(b.size() + 1), prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t v = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) v = std::min(v, back[j - 2] + 1);
            cur[j] = v;
        }
        std::swap(back, prev);
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string member_list_hint(const std::vector<std::string>& names) {
    constexpr size_t kMaxListed = 12;
    std::vector<std::string> shown(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(std::min(kMaxListed, names.size())));
    std::string s = "members: " + join(shown, " ");
    if (names.size() > kMaxListed) s += " \xE2\x80\xA6 (" + std::to_string(names.size() - kMaxListed) + " more)";
    return s;
}

/// Containment signals a typo or a partial name: the candidate contains the name (at least 3 characters), or the name
/// contains a candidate at least half its length (so "NoSuchType" does not suggest "Type").
static bool signals_typo(const std::string& name, const std::string& c) {
    return (name.size() >= 3 && icontains(c, name)) || (c.size() * 2 >= name.size() && icontains(name, c));
}

namespace {
bool ascii_upper_char(char c) { return c >= 'A' && c <= 'Z'; }

/// The lower-case words of a camelCase / PascalCase / snake_case name ("className" → class, name).
/// The lower-case words of a camelCase / PascalCase / snake_case name, ASCII rules only so every runtime splits alike:
/// "className" -> class, name; "URLPath" -> url, path; "max_hp" -> max, hp.
std::vector<std::string> name_words(const std::string& name) {
    std::vector<std::string> words;
    size_t start = 0;
    auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    for (size_t i = 1; i <= name.size(); ++i) {
        const bool boundary = i == name.size() || name[i] == '_' || (ascii_upper_char(name[i]) && !ascii_upper_char(name[i - 1])) ||
                              (ascii_upper_char(name[i]) && ascii_upper_char(name[i - 1]) && i + 1 < name.size() && lower(name[i + 1]));
        if (boundary) {
            if (i > start) {
                std::string w = name.substr(start, i - start);
                size_t b = w.find_first_not_of('_'), e = w.find_last_not_of('_');
                w = b == std::string::npos ? std::string() : w.substr(b, e - b + 1);
                if (!w.empty()) words.push_back(ascii_lower(w));
            }
            start = i;
        }
    }
    return words;
}
}  // namespace

std::string closest_parameter(const std::string& key, const std::vector<std::string>& names) {
    std::vector<std::string> close = suggest_closest(key, names, 1);
    if (!close.empty()) return close[0];
    if (key.size() < 3) return {};  // a two-letter key is near every short word
    const std::string lower = ascii_lower(key);
    const size_t limit = std::max<size_t>(1, key.size() / 3);
    std::string best;
    size_t best_d = static_cast<size_t>(-1);
    for (auto& n : names)
        for (auto& w : name_words(n)) {
            if (w.size() < 3) continue;
            size_t d = osa_distance(lower, w);
            if (d <= limit && d < best_d) {
                best = n;
                best_d = d;
            }
        }
    return best;
}

std::vector<std::string> suggest_closest(const std::string& name, const std::vector<std::string>& candidates, size_t max) {
    return suggest_members(name, candidates, {}, max);
}

bool try_number(const Value& v, double& d, bool& single) {
    if (!v.is_number()) { d = 0; single = false; return false; }
    d = v.as_number();
    single = v.is_single();
    return true;
}

bool elements(const Value& v, std::vector<Value>& out) {
    out.clear();
    if (v.is_list()) {
        auto& l = v.list();
        size_t n = l.size();
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) out.push_back(l.at(i));
        return true;
    }
    if (v.is_map()) {
        v.map().for_each([&](const Value& k, const Value& val) {
            out.push_back(Value::entry(k, val));
            return true;
        });
        return true;
    }
    return false;
}

size_t collection_size(const Value& v) {
    if (v.is_list()) return v.list().size();
    if (v.is_map()) return v.map().size();
    if (v.is_string()) return utf16_length(v.as_string());
    return 0;
}

std::string meta_hint(const std::string& name) {
    for (const char* m : {"type", "ref", "path", "index", "depth"})
        if (iequals(name, m)) return std::string("did you mean $") + m + " (the meta member)?";
    return {};
}

std::string path_text(const Expr& e) {
    if (e.kind != ExprKind::Path || e.on || e.segments.empty()) return {};
    std::string s;
    for (auto& seg : e.segments) {
        if ((seg.kind == SegmentKind::Member || seg.kind == SegmentKind::Meta) && !s.empty()) s += '.';
        s += seg.to_string();
    }
    return s;
}

namespace {
constexpr int kMaxNested = 200;

void walk_nested(Exec& x, const TypeDescriptor* type, const std::string& prefix, int level, int& budget, std::vector<std::string>& out) {
    const TypeView outer_view = x.view(type);
    for (auto& m : outer_view.members()) {
        const TypeDescriptor* inner = m.type;
        if (!inner || inner->kind != TypeKind::Struct) continue;
        const std::string path = prefix + m.name + ".";
        const TypeView inner_view = x.view(inner);
        for (auto& im : inner_view.members()) {
            if (budget-- <= 0) return;
            out.push_back(path + im.name);
        }
        if (level < 2) walk_nested(x, inner, path, level + 1, budget, out);
    }
}
}  // namespace

std::vector<std::string> nested_members(Exec& x, const std::vector<const TypeDescriptor*>& types) {
    std::vector<std::string> out;
    for (const TypeDescriptor* t : types) {
        if (!t) continue;
        int budget = kMaxNested;
        walk_nested(x, t, "", 1, budget, out);
    }
    // Several miss types may share a member type: keep each path once, in first-seen order.
    std::vector<std::string> unique;
    std::unordered_set<std::string> seen;
    for (auto& p : out)
        if (seen.insert(p).second) unique.push_back(std::move(p));
    return unique;
}

std::vector<std::string> suggest_members(const std::string& name, const std::vector<std::string>& members,
                                         const std::vector<std::string>& nested, size_t max) {
    struct C { std::string c; size_t d; bool sub; size_t depth; };
    std::vector<C> cs;
    const std::string lower = ascii_lower(name);
    const size_t limit = std::max<size_t>(1, name.size() / 3);
    auto consider = [&](const std::string& c, const std::string& leaf, size_t depth) {
        size_t d = osa_distance(lower, ascii_lower(leaf));
        const bool sub = signals_typo(name, leaf);
        if (sub || d <= limit) cs.push_back({c, d, sub, depth});
    };
    for (auto& c : members) consider(c, c, 0);
    for (auto& c : nested) consider(c, c.substr(c.rfind('.') + 1), static_cast<size_t>(std::count(c.begin(), c.end(), '.')));
    std::stable_sort(cs.begin(), cs.end(), [](const C& a, const C& b) {
        if (a.sub != b.sub) return a.sub;
        if (a.d != b.d) return a.d < b.d;
        if (a.depth != b.depth) return a.depth < b.depth;
        return a.c < b.c;
    });
    std::vector<std::string> out;
    for (auto& c : cs) {
        if (std::find(out.begin(), out.end(), c.c) != out.end()) continue;
        out.push_back(c.c);
        if (out.size() >= max) break;
    }
    return out;
}

std::string member_hint(const std::string& name, const std::vector<std::string>& members, const std::vector<std::string>& declaring,
                        const std::vector<std::string>& nested) {
    std::vector<std::string> parts;
    if (!declaring.empty())
        parts.push_back("'" + name + "' is a member of " + join(declaring, ", ") + ": filter to those first, e.g. where $ is " + declaring[0] +
                        " (or where has(" + name + "))");
    auto close = suggest_members(name, members, nested, 3);
    if (!close.empty()) {
        parts.push_back("did you mean " + join(close, " / ") + "?");
    } else if (declaring.empty() && !members.empty()) {
        parts.push_back(member_list_hint(by_similarity(name, members)));
    }
    return join(parts, "; ");
}

std::vector<std::string> by_similarity(const std::string& name, const std::vector<std::string>& names) {
    const std::string lower = ascii_lower(name);
    std::vector<std::pair<size_t, std::string>> ranked;
    for (auto& n : names) ranked.emplace_back(osa_distance(lower, ascii_lower(n)), n);
    std::sort(ranked.begin(), ranked.end());
    ranked.erase(std::unique(ranked.begin(), ranked.end()), ranked.end());
    std::vector<std::string> out;
    for (auto& r : ranked) out.push_back(std::move(r.second));
    return out;
}

namespace {

/// type-mismatch for a non-boolean condition (spec §1.2), with a boolean test to write instead.
RqlException not_a_condition(const Expr& e, const Value& v) {
    const std::string text = path_text(e);
    std::string what, example;
    if (v.is_string()) {
        what = "the string \"" + v.as_string() + "\"";
        example = (text.empty() ? "Name" : text) + " != \"\"";
    } else if (v.is_number()) {
        what = "the number " + to_text(v);
        example = (text.empty() ? "Count" : text) + " != 0";
    } else if (v.is_enum()) {
        what = "the " + v.type()->name + " value " + to_text(v);
        example = (text.empty() ? "Kind" : text) + " == \"" + to_text(v) + "\"";
    } else if (v.is_list() || v.is_map()) {
        what = v.is_list() ? "a list" : "a map";
        example = "len(" + (text.empty() ? std::string("Items") : text) + ") > 0";
    } else {
        what = "a " + type_name_of(v);
        example = (text.empty() ? "Target" : text) + " != null";
    }
    const std::string exists = !text.empty() && !v.is_string() && !v.is_number() && !v.is_enum() ? ", or has(" + text + ") to test that the member exists" : "";
    return RqlException("type-mismatch", "the condition " + (text.empty() ? std::string() : "'" + text + "' ") + "is " + what + ", not a boolean",
                        "a condition must be a boolean, e.g. " + example + exists);
}

}  // namespace

std::optional<bool> truth(const Value& v) {
    if (v.is_bool()) return v.as_bool();
    if (v.is_null()) return false;
    return std::nullopt;
}

Value tri(std::optional<bool> b) { return b ? Value::boolean(*b) : Value::unknown(); }

// ------------------------------------------------------------------ equality / ordering

namespace {

std::optional<bool> enum_equal(const Value& e, const Value& other) {
    if (other.is_string()) {
        // The entry the string resolves to (spec §8 enum name resolution); unresolved or ambiguous is not equal.
        int64_t v = 0;
        return e.type()->enum_parse(other.as_string(), v) && v == e.enum_value();
    }
    if (other.is_number()) return static_cast<double>(e.enum_value()) == other.as_number();
    if (other.is_enum()) return e.type() == other.type() && e.enum_value() == other.enum_value();
    return std::nullopt;
}

bool same_identity(const Value& a, const Value& b) {
    const void *a1, *a2, *b1, *b2;
    if (!a.identity(a1, a2) || !b.identity(b1, b2)) return false;
    return a1 == b1 && a2 == b2;
}

}  // namespace

namespace {

/// Nesting of lists compared by value (spec §4.1).
constexpr int kMaxCompareDepth = 32;

struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
};

}  // namespace

std::optional<bool> rql_equal(Exec& x, const Value& a, const Value& b) {
    // Destroyed objects arrive here as null already: values are normalized where they enter expressions.
    if (a.is_absent() || b.is_absent()) return std::nullopt;
    if (a.is_null() || b.is_null()) return a.is_null() && b.is_null();
    if (x.has_capability(Capability::Equals)) {
        // Adapters of the left operand first, then of the right operand (spec §13).
        if (auto r = first_answer(x.adapters_for(a, Capability::Equals), [&](const TypeAdapter& ad) { return ad.equals(a, b); })) return r;
        if (auto r = first_answer(x.adapters_for(b, Capability::Equals), [&](const TypeAdapter& ad) { return ad.equals(b, a); })) return r;
    }
    if (a.is_number()) {
        if (b.is_number()) {
            if (a.is_single() || b.is_single())
                return static_cast<float>(a.as_number()) == static_cast<float>(b.as_number());
            return a.as_number() == b.as_number();
        }
        if (b.is_enum()) return static_cast<double>(b.enum_value()) == a.as_number();
        return std::nullopt;
    }
    if (a.is_enum()) return enum_equal(a, b);
    if (b.is_enum()) return enum_equal(b, a);
    if (a.is_string()) return b.is_string() ? std::optional<bool>(a.as_string() == b.as_string()) : std::nullopt;
    if (a.is_bool()) return b.is_bool() ? std::optional<bool>(a.as_bool() == b.as_bool()) : std::nullopt;
    // Two structs of one type compare member by member, recursively (spec §4.1): the members their encoding shows, without
    // members of the struct's own type (derived values); false as soon as one differs, unknown when none differs but one is.
    if (a.is_struct() && b.is_struct() && a.type() && a.type() == b.type()) {
        const auto view = x.view(a.type());
        // Struct nesting is capped at 8 so derived members of other struct types cannot branch without end.
        if (x.compare_depth >= kMaxCompareDepth || x.struct_eq_depth >= 8) return std::nullopt;
        DepthGuard guard(x.compare_depth);
        DepthGuard struct_guard(x.struct_eq_depth);
        bool unknown = false;
        size_t compared = 0;
        for (auto& m : view.implicit_members()) {
            if (m.type == a.type()) continue;
            ++compared;
            Value ma, mb;
            try {
                ma = x.live(m.getter(a.ptr()));
                mb = x.live(m.getter(b.ptr()));
            } catch (const std::exception&) {
                return std::nullopt;
            }
            auto r = rql_equal(x, ma, mb);
            if (r == std::optional<bool>(false)) return false;
            if (!r) unknown = true;
        }
        if (compared == 0) return std::nullopt;  // nothing visible to compare
        return unknown ? std::nullopt : std::optional<bool>(true);
    }
    // structs of different types and mixed scalar kinds are not comparable
    if (a.is_struct() || b.is_struct() || b.is_number() || b.is_bool() || b.is_string()) return std::nullopt;
    if (a.is_list() && b.is_list()) {
        // Lists compare by value (spec §4.1): the same length and pairwise equal elements; false as soon as a pair differs,
        // unknown when no pair differs but one is unknown. Self-containing or very deep lists are not compared.
        if (same_identity(a, b)) return true;
        if (x.compare_depth >= kMaxCompareDepth) return std::nullopt;
        DepthGuard guard(x.compare_depth);
        const size_t n = a.list().size();
        if (n != b.list().size()) return false;
        bool unknown = false;
        for (size_t i = 0; i < n; ++i) {
            auto r = rql_equal(x, x.live(a.list().at(i)), x.live(b.list().at(i)));
            if (r == std::optional<bool>(false)) return false;
            if (!r) unknown = true;
        }
        return unknown ? std::nullopt : std::optional<bool>(true);
    }
    if (a.is_list() || b.is_list() || a.is_map() || b.is_map()) return std::nullopt;
    return same_identity(a, b);
}

std::optional<int> rql_compare(Exec& x, const Value& a, const Value& b) {
    if (a.is_null() || b.is_null() || a.is_absent() || b.is_absent()) return std::nullopt;
    if (x.has_capability(Capability::Compare)) {
        // Adapters of the left operand first, then of the right operand with the result mirrored (spec §13).
        auto sign = [](int c) { return c < 0 ? -1 : (c > 0 ? 1 : 0); };
        if (auto r = first_answer(x.adapters_for(a, Capability::Compare), [&](const TypeAdapter& ad) { return ad.compare(a, b); }))
            return sign(*r);
        if (auto r = first_answer(x.adapters_for(b, Capability::Compare), [&](const TypeAdapter& ad) { return ad.compare(b, a); }))
            return -sign(*r);
    }
    if (a.is_number()) {
        if (b.is_number()) {
            if (a.is_single() || b.is_single())
                return cmp_double(static_cast<float>(a.as_number()), static_cast<float>(b.as_number()));
            return cmp_double(a.as_number(), b.as_number());
        }
        if (b.is_enum()) return cmp_double(a.as_number(), static_cast<double>(b.enum_value()));
        return std::nullopt;
    }
    if (a.is_enum()) {
        double va = static_cast<double>(a.enum_value());
        if (b.is_enum() && b.type() == a.type()) return cmp_double(va, static_cast<double>(b.enum_value()));
        if (b.is_number()) return cmp_double(va, b.as_number());
        int64_t ev = 0;
        if (b.is_string() && a.type()->enum_parse(b.as_string(), ev)) return cmp_double(va, static_cast<double>(ev));
        return std::nullopt;
    }
    if (a.is_string() && b.is_string()) {
        int c = a.as_string().compare(b.as_string());
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (a.is_bool() && b.is_bool()) return a.as_bool() == b.as_bool() ? 0 : (a.as_bool() ? 1 : -1);
    return std::nullopt;
}

/// A struct member number as numeric tuples display it (spec §5, §13): single precision to 6 significant digits, others
/// to 12, tiny magnitudes (below 1e-6 / 1e-9) and -0 as 0. Only compared within one process, so text is a fine key.
static std::string rounded_key(const Value& v) {
    const double d = v.as_number();
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    const bool single = v.is_single();
    if (std::fabs(d) < (single ? 1e-6 : 1e-9)) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*e", single ? 5 : 11, single ? static_cast<double>(to_single(d)) : d);
    return buf;
}

/// Struct levels a distinct / group-by key looks into: the struct and two nested levels (spec §5 "Struct keys").
constexpr int kMaxStructKeyDepth = 3;

bool distinct_equal(Exec& x, const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return a.is_null() && b.is_null();
    if (a.is_missing() || b.is_missing()) return a.is_missing() && b.is_missing();  // singletons in .NET
    if (a.is_unknown() || b.is_unknown()) return a.is_unknown() && b.is_unknown();
    if (a.is_number() && b.is_number())  // NaN groups with NaN
        return a.as_number() == b.as_number() || (std::isnan(a.as_number()) && std::isnan(b.as_number()));
    if (a.is_enum() && b.is_enum()) return a.type() == b.type() && a.enum_value() == b.enum_value();
    if (a.is_string() && b.is_string()) return a.as_string() == b.as_string();
    if (a.is_bool() && b.is_bool()) return a.as_bool() == b.as_bool();
    if (a.is_struct() && b.is_struct()) {
        if (a.type() != b.type()) return false;
        // The struct and two nested levels count (spec §5 "Struct keys"); deeper levels are not compared.
        if (x.struct_key_depth >= kMaxStructKeyDepth) return true;
        DepthGuard guard(x.struct_key_depth);
        const auto view = x.view(a.type());
        for (auto& m : view.implicit_members()) {  // never hidden / explicit-only members (spec §13)
            if (m.type == a.type()) continue;      // a member of the struct's own type is a derived value (normalized)
            Value ma, mb;
            bool threw_a = false, threw_b = false;
            try {
                ma = x.live(m.getter(a.ptr()));
            } catch (const std::exception&) {
                threw_a = true;
            }
            try {
                mb = x.live(m.getter(b.ptr()));
            } catch (const std::exception&) {
                threw_b = true;
            }
            if (threw_a || threw_b) {  // as .NET: members whose getter throws on both sides are equal
                if (threw_a && threw_b) continue;
                return false;
            }
            // Numbers as numeric tuples print them (spec §5 "Struct keys"): values that print alike group together.
            if (ma.is_number() && mb.is_number()) {
                if (rounded_key(ma) != rounded_key(mb)) return false;
            } else if (!distinct_equal(x, ma, mb)) {
                return false;
            }
        }
        return true;
    }
    if (a.is_row() && b.is_row()) {  // projected rows by their column values (spec §5)
        const auto& ra = a.row().values;
        const auto& rb = b.row().values;
        if (ra.size() != rb.size()) return false;
        for (size_t i = 0; i < ra.size(); ++i)
            if (!distinct_equal(x, ra[i], rb[i])) return false;
        return true;
    }
    if (a.is_list() && b.is_list()) {  // lists by their elements (spec §5): group-by Tags merges equal tag lists
        if (same_identity(a, b)) return true;
        if (x.compare_depth >= kMaxCompareDepth) return false;
        DepthGuard guard(x.compare_depth);
        const size_t n = a.list().size();
        if (n != b.list().size()) return false;
        for (size_t i = 0; i < n; ++i)
            if (!distinct_equal(x, x.live(a.list().at(i)), x.live(b.list().at(i)))) return false;
        return true;
    }
    return same_identity(a, b);
}

size_t distinct_hash(Exec& x, const Value& v) {
    switch (v.kind()) {
        case Value::Kind::Null: return 0;
        case Value::Kind::Number: {
            double d = v.as_number();
            if (d == 0) d = 0;  // -0 == 0
            if (std::isnan(d)) return 0x7ff8;
            return std::hash<double>()(d);
        }
        case Value::Kind::String: return std::hash<std::string>()(v.as_string());
        case Value::Kind::Bool: return v.as_bool() ? 1 : 2;
        case Value::Kind::Enum: return std::hash<int64_t>()(v.enum_value()) ^ std::hash<const void*>()(v.type());
        case Value::Kind::Struct: {
            size_t h = std::hash<const void*>()(v.type());
            if (x.struct_key_depth >= kMaxStructKeyDepth) return h;
            DepthGuard guard(x.struct_key_depth);
            const auto view = x.view(v.type());
            for (auto& m : view.implicit_members()) {
                if (m.type == v.type()) continue;
                try {
                    Value mv = x.live(m.getter(v.ptr()));
                    h = h * 31 + (mv.is_number() ? std::hash<std::string>()(rounded_key(mv)) : distinct_hash(x, mv));
                } catch (const std::exception&) {
                    h = h * 31 + 1;
                }
            }
            return h;
        }
        case Value::Kind::Row: {
            const auto& values = v.row().values;
            size_t h = values.size();
            for (auto& el : values) h = h * 31 + distinct_hash(x, el);
            return h;
        }
        case Value::Kind::List: {
            const size_t n = v.list().size();
            size_t h = 17 + n;
            if (x.compare_depth >= kMaxCompareDepth) return h;
            DepthGuard guard(x.compare_depth);
            for (size_t i = 0; i < n; ++i) h = h * 31 + distinct_hash(x, x.live(v.list().at(i)));
            return h;
        }
        default: {
            const void *a, *b;
            if (v.identity(a, b)) return std::hash<const void*>()(a) * 31 + std::hash<const void*>()(b);
            return 7;
        }
    }
}

Value reduce(Exec& x, const std::vector<Value>& values, const std::string& kind) {
    double sum = 0;
    int64_t n = 0;
    bool minmax = kind == "min" || kind == "max";
    Value best;
    bool has_best = false;
    int64_t ignored = 0;
    std::string first_ignored;
    for (auto& val : values) {
        // null / missing are skipped silently; any other skipped value (error / unknown included) is counted (spec §4.3).
        if (val.is_null() || val.is_missing()) continue;
        bool used;
        if (minmax) {
            // Numbers, strings, enums — and any value whose type has an ordering adapter (spec §13).
            used = !val.is_absent() &&
                   (val.is_number() || val.is_string() || val.is_enum() || !x.adapters_for(val, Capability::Compare).empty());
            if (used && !has_best) {
                best = val;
                has_best = true;
            } else if (used) {
                auto c = rql_compare(x, val, best);
                if (!c) used = false;
                else if ((kind == "min" && *c < 0) || (kind == "max" && *c > 0)) best = val;
            }
        } else {
            double d;
            used = try_number(val, d);
            if (used) {
                sum += d;
                ++n;
            }
        }
        if (!used && ignored++ == 0) first_ignored = describe_value(val);
    }
    if (ignored > 0) {
        const char* what = minmax ? "value(s) not comparable with the others" : "non-numeric value(s)";
        x.warn_once("type-mismatch", kind + " ignored " + std::to_string(ignored) + " " + what + " (first: " + first_ignored + ")",
                    "aggregate:" + kind);
    }
    if (kind == "sum") return Value::number(sum);
    if (kind == "avg") return n == 0 ? Value::null() : Value::number(sum / static_cast<double>(n));
    return has_best ? best : Value::null();
}

// ------------------------------------------------------------------ member access

namespace {

void note_miss(MemberSite& site, const TypeDescriptor* t) {
    ++site.misses;
    if (t && site.miss_types.size() < 8 && std::find(site.miss_types.begin(), site.miss_types.end(), t) == site.miss_types.end())
        site.miss_types.push_back(t);
}

Value map_lookup_raw(Exec& x, const Value& map, const Value& key, MemberSite* site);
/// Map value read (destroyed objects read as null).
Value map_lookup(Exec& x, const Value& map, const Value& key, MemberSite* site) { return x.live(map_lookup_raw(x, map, key, site)); }

Value map_lookup_raw(Exec& x, const Value& map, const Value& key, MemberSite* site) {
    Value out;
    MapLookup r = map.map().find(key, out);
    if (r == MapLookup::BadKey) {
        if (site) ++site->misses;
        return Value::missing();
    }
    ++x.members_read;
    if (r == MapLookup::Found) {
        if (site) ++site->hits;
        return out;
    }
    if (key.is_string()) {
        bool found = false;
        map.map().for_each([&](const Value& k, const Value& v) {
            if (k.is_string() && iequals(k.as_string(), key.as_string())) {
                out = v;
                found = true;
                return false;
            }
            return true;
        });
        if (found) {
            if (site) ++site->hits;
            return out;
        }
    }
    // An absent key reads as null (spec §3); inside has() it is missing, so has(M["k"]) tests the key.
    return x.key_probe > 0 ? Value::missing() : Value::null();
}

Value index_at(const Value& v, int64_t idx) {
    if (v.is_null()) return Value::missing();
    if (v.is_absent()) return v;
    if (!v.is_list()) return Value::missing();
    auto& l = v.list();
    int64_t n = static_cast<int64_t>(l.size());
    int64_t i = idx < 0 ? n + idx : idx;
    return i >= 0 && i < n ? l.at(static_cast<size_t>(i)) : Value::missing();
}

Value key_at(Exec& x, const Value& v, const Value& key) {
    if (v.is_null()) return Value::missing();
    if (v.is_absent()) return v;
    if (v.is_map()) return map_lookup(x, v, key, nullptr);
    if (key.is_string() && !v.is_list()) {
        MemberSite tmp(key.as_string());
        return member_of(x, v, tmp);
    }
    if (key.is_number() && std::floor(key.as_number()) == key.as_number())
        return x.live(index_at(v, static_cast<int64_t>(key.as_number())));
    return Value::missing();
}

const TypeDescriptor* scalar_type_of(const Value& v) {
    switch (v.kind()) {
        case Value::Kind::String: return builtin::string_type();
        case Value::Kind::Bool: return builtin::bool_type();
        case Value::Kind::Number: return v.type() ? v.type() : builtin::double_type();
        default: return v.type();
    }
}

}  // namespace

Value member_of(Exec& x, const Value& v, MemberSite& site) {
    if (v.is_null()) return Value::missing();
    if (v.is_absent()) return v;
    switch (v.kind()) {
        case Value::Kind::List: {
            if (iequals(site.name, "Count") || iequals(site.name, "Length")) {
                ++site.hits;
                return Value::number(static_cast<double>(v.list().size()));
            }
            // Implicit map over elements, flattening one level (spec §3).
            std::vector<Value> out;
            auto& l = v.list();
            size_t n = l.size();
            for (size_t i = 0; i < n; ++i) {
                x.visit();
                Value r = member_of(x, x.live(l.at(i)), site);
                if (r.is_missing()) continue;
                if (r.is_list()) {
                    auto& inner = r.list();
                    size_t m = inner.size();
                    for (size_t j = 0; j < m; ++j) out.push_back(inner.at(j));
                } else {
                    out.push_back(std::move(r));
                }
            }
            return Value::list_of(std::move(out));
        }
        case Value::Kind::Map:
            if (iequals(site.name, "Count")) {
                ++site.hits;
                return Value::number(static_cast<double>(v.map().size()));
            }
            return map_lookup(x, v, Value::string(site.name), &site);
        case Value::Kind::Row: {
            const RowData& row = v.row();
            int i = row.index_of(site.name);
            if (i >= 0) {
                ++site.hits;
                return row.values[static_cast<size_t>(i)];
            }
            // A group row's items (spec §5): a column of that name takes precedence.
            if (row.items.is_list() && iequals(site.name, "items")) {
                ++site.hits;
                return row.items;
            }
            // A single-column row stands for its value.
            if (row.values.size() == 1) return member_of(x, row.values[0], site);
            ++site.misses;
            if (site.row_columns.empty()) site.row_columns = *row.columns;
            if (row.group) site.group_rows = true;
            return Value::missing();
        }
        case Value::Kind::Object:
        case Value::Kind::Struct:
        case Value::Kind::Entry: {
            const TypeDescriptor* t = v.type();
            const MemberDescriptor* m;
            if (t == site.cached_type) {
                m = site.cached_member;
            } else {
                m = x.view(t).find(site.name);
                if (!m && v.is_entry()) return member_of(x, v.entry().value, site);  // spec §4.4 delegation
                if (!m) {
                    note_miss(site, t);
                    return Value::missing();
                }
                site.cached_type = t;
                site.cached_member = m;
            }
            ++site.hits;
            ++x.members_read;
            try {
                return x.live(m->getter(v.ptr()));  // destroyed objects read as null (spec §13)
            } catch (const std::exception& ex) {
                x.getter_threw(m->name, ex.what());
                return Value::error("getter-threw", ex.what());
            }
        }
        default:
            note_miss(site, scalar_type_of(v));
            return Value::missing();
    }
}

// ------------------------------------------------------------------ compile

Eval Compiler::compile(const Expr& e) {
    switch (e.kind) {
        case ExprKind::Lit: {
            Value v;
            switch (e.lit.type) {
                case Literal::Type::Null: v = Value::null(); break;
                case Literal::Type::Bool: v = Value::boolean(e.lit.b); break;
                case Literal::Type::Number: v = Value::number(e.lit.n); break;
                case Literal::Type::String: v = Value::string(e.lit.s); break;
            }
            return [v](Exec&) { return v; };
        }
        case ExprKind::List: {
            std::vector<Eval> items;
            for (auto& i : e.args) items.push_back(compile(*i));
            return [items](Exec& x) {
                std::vector<Value> out;
                out.reserve(items.size());
                for (auto& i : items) out.push_back(i(x));
                return Value::list_of(std::move(out));
            };
        }
        case ExprKind::Object: {
            // An ordered string-keyed map (spec §4.5); coerced into maps / constructed objects on writes.
            std::vector<std::string> keys = e.keys;
            std::vector<Eval> values;
            for (auto& v : e.args) values.push_back(compile(*v));
            return [keys, values](Exec& x) {
                std::vector<std::pair<Value, Value>> entries;
                entries.reserve(keys.size());
                for (size_t i = 0; i < keys.size(); ++i) entries.emplace_back(Value::string(keys[i]), values[i](x));
                return Value::map(std::make_shared<OwnedMap>(nullptr, std::move(entries), true));
            };
        }
        case ExprKind::Path: return compile_path(e);
        case ExprKind::Op: return compile_op(e);
        case ExprKind::Call: return compile_call(e);
    }
    throw RqlException("invalid-plan", "unknown expression node");
}

Eval Compiler::compile_path(const Expr& p) {
    // The current item enters the expression here: a destroyed object reads as null (no getter runs on it).
    Eval cur = p.on ? compile(*p.on) : Eval([](Exec& x) { return x.live(x.current); });
    bool relative = !p.on;
    for (size_t i = 0; i < p.segments.size(); ++i) {
        const Segment& seg = p.segments[i];
        Eval prev = cur;
        bool first = i == 0 && relative;
        switch (seg.kind) {
            case SegmentKind::Member: {
                x_.sites.push_back(std::make_unique<MemberSite>(seg.name));
                MemberSite* site = x_.sites.back().get();
                site->bare = first;
                site->guarded = guarded_ > 0;
                if (first) {
                    // Lexical scope chain: inside collection-method arguments a bare name missing on the
                    // element resolves against the enclosing item(s), innermost first (spec §3).
                    cur = [site](Exec& x) {
                        if (x.outer.empty()) return member_of(x, x.live(x.current), *site);
                        // An absent map key counts as missing here, so the name can still resolve on an enclosing item
                        // (spec §3); when no item has it, the element's own answer stands (null for an absent key).
                        {
                            struct Probe {
                                int& depth;
                                explicit Probe(int& d) : depth(d) { ++depth; }
                                ~Probe() { --depth; }
                            } probe(x.key_probe);
                            Value r = member_of(x, x.live(x.current), *site);
                            if (!r.is_missing()) return r;
                            for (size_t k = x.outer.size(); k-- > 0;) {
                                Value o = member_of(x, x.live(x.outer[k]), *site);
                                if (!o.is_missing()) return o;
                            }
                        }
                        return x.key_probe > 0 ? Value::missing() : member_of(x, x.live(x.current), *site);
                    };
                } else {
                    cur = [prev, site](Exec& x) { return member_of(x, prev(x), *site); };
                }
                break;
            }
            case SegmentKind::Index: {
                int64_t idx = seg.index;
                Value key = Value::number(static_cast<double>(idx));
                cur = [prev, idx, key](Exec& x) {
                    Value v = prev(x);
                    if (v.is_map()) return map_lookup(x, v, key, nullptr);
                    return x.live(index_at(v, idx));
                };
                break;
            }
            case SegmentKind::Key: {
                Value key = seg.key.type == Literal::Type::String ? Value::string(seg.key.s) : Value::number(seg.key.n);
                cur = [prev, key](Exec& x) { return key_at(x, prev(x), key); };
                break;
            }
            case SegmentKind::Meta:
                cur = compile_meta(seg.name, prev, first);
                break;
        }
    }
    if (relative && p.segments.size() > 1 &&
        std::all_of(p.segments.begin(), p.segments.end(), [](const Segment& s) { return s.kind == SegmentKind::Member; })) {
        // After `select Owner.Label`, the path Owner.Label names that column (spec §3), not Owner on the row.
        std::vector<std::string> names;
        for (auto& s : p.segments) names.push_back(s.name);
        std::string column = join(names, ".");
        Eval navigate = cur;
        return [column, navigate](Exec& x) {
            if (x.current.is_row()) {
                int i = x.current.row().index_of(column);
                if (i >= 0) return x.current.row().values[static_cast<size_t>(i)];
            }
            return navigate(x);
        };
    }
    return cur;
}

Eval Compiler::compile_meta(const std::string& name, Eval prev, bool of_current_item) {
    Eval inner = compile_meta_core(name, prev, of_current_item);
    if (!of_current_item) return inner;
    // After `select`, a column literally named "$name" (e.g. select $path) shadows the meta member.
    std::string column = "$" + name;
    return [inner, column](Exec& x) {
        if (x.current.is_row()) {
            int i = x.current.row().index_of(column);
            if (i >= 0) return x.current.row().values[static_cast<size_t>(i)];
        }
        return inner(x);
    };
}

Eval Compiler::compile_meta_core(const std::string& name, Eval prev, bool of_current_item) {
    // Meta members applied to a navigated list map over its elements (spec §3), skipping missing.
    auto map_meta = [](const Value& list, const std::function<Value(const Value&)>& f) {
        std::vector<Value> out;
        auto& l = list.list();
        size_t n = l.size();
        for (size_t i = 0; i < n; ++i) {
            Value r = f(l.at(i));
            if (!r.is_missing()) out.push_back(std::move(r));
        }
        return Value::list_of(std::move(out));
    };
    if (name == "type") {
        return [prev, of_current_item, map_meta](Exec& x) {
            Value v = prev(x);
            if (v.is_null()) return Value::null();
            if (v.is_absent()) return v;
            if (!of_current_item && v.is_list())
                return map_meta(v, [](const Value& e) { return e.is_null() ? Value::null() : Value::string(type_name_of(e)); });
            return Value::string(type_name_of(v));
        };
    }
    if (name == "ref") {
        return [prev, of_current_item, map_meta](Exec& x) {
            Value v = prev(x);
            if (v.is_null()) return Value::null();
            if (v.is_absent()) return v;
            if (!of_current_item && v.is_list())
                return map_meta(v, [&x](const Value& e) { return e.is_object() ? Value::string(x.ref_of(e)) : Value::missing(); });
            if (v.is_row()) {
                const RowData& row = v.row();
                return row.values.size() == 1 && row.values[0].is_object() ? Value::string(x.ref_of(row.values[0]))
                                                                            : Value::missing();
            }
            return v.is_object() ? Value::string(x.ref_of(v)) : Value::missing();
        };
    }
    if (name == "path") {
        x_.track_paths = true;
        if (!of_current_item) return [](Exec&) { return Value::missing(); };
        return [](Exec& x) { return x.current_path ? Value::string(x.current_path->str()) : Value::missing(); };
    }
    if (name == "index") {
        if (!of_current_item) return [](Exec&) { return Value::missing(); };
        return [](Exec& x) { return Value::number(static_cast<double>(x.index)); };
    }
    if (name == "count") {
        if (!of_current_item) return [](Exec&) { return Value::missing(); };
        if (!mutation_values)
            x_.diagnostics.push_back(Diagnostic::warning("unknown-meta", "'$count' is only defined in a set value or call argument",
                                                         "it is the number of items entering the set / call stage, so $index / $count runs from 0 towards 1 there"));
        return [](Exec& x) { return x.count >= 0 ? Value::number(static_cast<double>(x.count)) : Value::missing(); };
    }
    if (name == "depth") {
        if (!of_current_item) return [](Exec&) { return Value::missing(); };
        return [](Exec& x) { return x.current_depth >= 0 ? Value::number(x.current_depth) : Value::missing(); };
    }
    x_.diagnostics.push_back(Diagnostic::warning("unknown-meta", "unknown meta member '$" + name + "'",
                                                 "meta members: $type $ref $path $index $count $depth"));
    return [](Exec&) { return Value::missing(); };
}

// ------------------------------------------------------------------ operators

namespace {

std::optional<bool> contains_op(Exec& x, const Value& a, const Value& b) {
    if (a.is_absent() || b.is_absent() || a.is_null()) return std::nullopt;
    if (a.is_string()) {
        if (!b.is_string()) return std::nullopt;
        return icontains(a.as_string(), b.as_string());
    }
    if (a.is_list()) {
        auto& l = a.list();
        size_t n = l.size();
        for (size_t i = 0; i < n; ++i)
            if (rql_equal(x, x.live(l.at(i)), b) == std::optional<bool>(true)) return true;
        return false;
    }
    if (a.is_map()) {
        bool found = false;
        a.map().for_each([&](const Value& k, const Value&) {
            if (rql_equal(x, k, b) == std::optional<bool>(true)) { found = true; return false; }
            return true;
        });
        return found;
    }
    return std::nullopt;
}

std::optional<bool> string_test(const Value& a, const Value& b, bool starts) {
    if (!a.is_string() || !b.is_string()) return std::nullopt;
    std::string s = ascii_lower(a.as_string()), t = ascii_lower(b.as_string());
    if (t.size() > s.size()) return false;
    return starts ? s.compare(0, t.size(), t) == 0 : s.compare(s.size() - t.size(), t.size(), t) == 0;
}

Exec::CachedRegex compiled_regex(Exec& x, const std::string& pattern) {
    auto it = x.regex_cache.find(pattern);
    if (it != x.regex_cache.end()) return it->second;
    Exec::CachedRegex made;
    try {
        made.re = std::make_shared<std::regex>(pattern, std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error& ex) {
        made.error = ex.what();
        if (made.error.empty()) made.error = "the pattern does not compile";
    }
    if (x.regex_cache.size() < 256) x.regex_cache[pattern] = made;
    return made;
}

std::string invalid_regex_message(const std::string& pattern, const std::string& error) {
    return "invalid regex \"" + pattern + "\": " + error;
}

/// A literal pattern that does not compile fails the query before it runs (spec §4.1).
void check_literal_regex(Exec& x, const Expr* pattern) {
    if (!pattern || pattern->kind != ExprKind::Lit || pattern->lit.type != Literal::Type::String) return;
    const Exec::CachedRegex cached = compiled_regex(x, pattern->lit.s);
    if (!cached.re) throw RqlException("invalid-regex", invalid_regex_message(pattern->lit.s, cached.error));
}

std::optional<bool> matches_op(Exec& x, const Value& a, const Value& b) {
    if (!a.is_string() || !b.is_string()) return std::nullopt;
    const std::string& pattern = b.as_string();
    const Exec::CachedRegex cached = compiled_regex(x, pattern);
    const std::shared_ptr<std::regex>& re = cached.re;
    if (!re) {
        x.warn_once("invalid-regex", invalid_regex_message(pattern, cached.error) + "; such values count as unknown", "invalid-regex");
        return std::nullopt;
    }
    try {
        return std::regex_search(a.as_string(), *re);
    } catch (const std::regex_error& ex) {
        // std::regex has no time limit: complexity / stack exhaustion stands in for the .NET match timeout.
        x.warn_once("timeout", "regex \"" + pattern + "\" failed on a value (" + ex.what() +
                                   "); such values count as unknown (simplify the pattern)", "regex-timeout");
        return std::nullopt;
    }
}

std::optional<bool> in_op(Exec& x, const Value& a, const Value& b) {
    if (a.is_absent() || b.is_absent() || b.is_null()) return std::nullopt;
    if (!b.is_list()) return rql_equal(x, a, b);
    auto& l = b.list();
    size_t n = l.size();
    for (size_t i = 0; i < n; ++i)
        if (rql_equal(x, a, x.live(l.at(i))) == std::optional<bool>(true)) return true;
    return false;
}

std::optional<bool> is_type(const Value& a, const Value& b) {
    if (a.is_absent()) return std::nullopt;
    if (a.is_null()) return false;
    if (!b.is_string()) return std::nullopt;
    const TypeDescriptor* t = a.type();
    if (t && (a.is_object() || a.is_struct() || a.is_enum() || a.is_entry() || a.is_row() || a.is_number() || a.is_list() || a.is_map()))
        return t->is_a(b.as_string()) || iequals(type_name_of(a), b.as_string());
    return iequals(type_name_of(a), b.as_string());
}

/// The arithmetic components of a struct whose type adapter declares them (spec §4.2, §13), with that adapter.
bool components_of(Exec& x, const Value& v, std::vector<double>& parts, const TypeAdapter*& adapter) {
    if (!v.is_struct()) return false;
    for (const TypeAdapter* ad : x.adapters_for(v, Capability::Components)) {
        if (auto c = ad->components(v)) {
            parts = std::move(*c);
            adapter = ad;
            return true;
        }
    }
    return false;
}

Value arith_mismatch(Exec& x, const std::string& op, const Value& a, const Value& b, std::string why = {}) {
    std::string message = "cannot apply '" + op + "' to " + describe_value(a) + " and " + describe_value(b);
    if (why.empty()) {
        // A struct without declared components: say why (spec §4.2), so `Scale * 1.5` is not a mystery.
        std::vector<double> parts;
        const TypeAdapter* ad = nullptr;
        if (a.is_struct() && !components_of(x, a, parts, ad)) why = describe_value(a) + " has no component-wise arithmetic";
        else if (op != "neg" && b.is_struct() && !components_of(x, b, parts, ad)) why = describe_value(b) + " has no component-wise arithmetic";
    }
    return Value::error("type-mismatch", why.empty() ? message : message + " (" + why + ")");
}

/// The numbers of a list whose elements are all numbers (spec §4.2 numeric lists).
bool numeric_list(const Value& v, std::vector<double>& out) {
    if (!v.is_list()) return false;
    auto& l = v.list();
    out.clear();
    const size_t n = l.size();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        double d;
        if (!try_number(l.at(i), d)) return false;
        out.push_back(d);
    }
    return true;
}

/// A list of exactly `width` numbers read as components (spec §4.2).
bool list_parts(const Value& v, size_t width, std::vector<double>& out) { return numeric_list(v, out) && out.size() == width; }

/// A value of `like`'s struct type built from components through its adapter, or nullopt (spec §4.2, §13).
std::optional<Value> build_from_components(const TypeAdapter& adapter, const Value& like, const std::vector<double>& parts) {
    try {
        if (adapter.from_components) return adapter.from_components(*like.type(), parts);
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

/// Component-wise arithmetic on structs whose adapter declares components (spec §4.2): v * n, n * v, v / n, v ± w, v * w,
/// v / w (w of the same type, or a list of as many numbers), -v. nullopt when neither operand has components.
std::optional<Value> struct_arith(Exec& x, std::string op, const Value& a, const Value& b) {
    std::vector<double> pa, pb;
    const TypeAdapter* ada = nullptr;
    const TypeAdapter* adb = nullptr;
    const bool ha = components_of(x, a, pa, ada);
    const bool hb = op != "neg" && components_of(x, b, pb, adb);
    if (!ha && !hb) return std::nullopt;
    if (op == "%") return arith_mismatch(x, op, a, b, "'%' is not component-wise");
    const bool vector_left = ha;
    const TypeAdapter* adapter = vector_left ? ada : adb;
    const Value& vector = vector_left ? a : b;
    std::vector<double> parts = vector_left ? std::move(pa) : std::move(pb);
    std::vector<double> other;
    if (op == "neg") {
        other = parts;
        parts.assign(other.size(), 0.0);  // -v = 0 - v
        op = "-";
    } else if (ha && hb) {
        if (a.type() != b.type() || parts.size() != pb.size()) return arith_mismatch(x, op, a, b, "different types");
        other = std::move(pb);
    } else {
        const Value& operand = vector_left ? b : a;
        if (operand.is_number()) {
            // v * n, n * v and v / n scale; a number added to a vector, or divided by one, is not defined.
            if (!(op == "*" || (op == "/" && vector_left)))
                return arith_mismatch(x, op, a, b,
                                      op == "/" ? "a number cannot be divided by a vector"
                                                : "add a value of the same type or a list of " + std::to_string(parts.size()) +
                                                      " numbers; multiply or divide by a number");
            other.assign(parts.size(), operand.as_number());
        } else if (!list_parts(operand, parts.size(), other)) {
            return arith_mismatch(x, op, a, b,
                                  operand.is_list() ? "the list must hold exactly " + std::to_string(parts.size()) + " numbers, one per component"
                                                    : std::string());
        }
        if (!vector_left) std::swap(parts, other);
    }
    std::vector<double> result(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        const double l = parts[i], r = other[i];
        if (op == "+") result[i] = l + r;
        else if (op == "-") result[i] = l - r;
        else if (op == "*") result[i] = l * r;
        else {
            if (r == 0) return Value::error("div-by-zero");
            result[i] = l / r;
        }
    }
    std::optional<Value> built = build_from_components(*adapter, vector, result);
    if (!built) return arith_mismatch(x, op, a, b, "cannot build " + describe_value(vector) + " from components");
    return built;
}

Value number_list(const std::vector<double>& parts) {
    std::vector<Value> items;
    items.reserve(parts.size());
    for (double d : parts) items.push_back(Value::number(d));
    return Value::list_of(std::move(items));
}

/// Arithmetic on numeric lists without a struct operand (spec §4.2): L * n, n * L, L / n, L ± M (same length), -L; a new
/// list of numbers. nullopt when neither operand is a list; a type-mismatch with the reason for any other list operation.
std::optional<Value> list_arith(const std::string& op, const Value& a, const Value& b) {
    const bool la = a.is_list(), lb = op != "neg" && b.is_list();
    if (!la && !lb) return std::nullopt;
    auto mismatch = [&](const std::string& why) {
        return Value::error("type-mismatch", "cannot apply '" + std::string(op == "neg" ? "-" : op) + "' to " + describe_value(a) +
                                                 (op == "neg" ? std::string() : " and " + describe_value(b)) + " (" + why + ")");
    };
    std::vector<double> pa, pb;
    if ((la && !numeric_list(a, pa)) || (lb && !numeric_list(b, pb))) return mismatch("every list element must be a number");
    if (op == "%") return mismatch("'%' is not a vector operation");
    std::vector<Value> result;
    if (op == "neg") {
        result.reserve(pa.size());
        for (double d : pa) result.push_back(Value::number(-d));
    } else if (la && lb) {
        if (op == "*" || op == "/") return mismatch("an element-wise product is not a vector operation; use dot(a, b)");
        if (pa.size() != pb.size())
            return mismatch("the lists have " + std::to_string(pa.size()) + " and " + std::to_string(pb.size()) + " numbers");
        result.reserve(pa.size());
        for (size_t i = 0; i < pa.size(); ++i) result.push_back(Value::number(op == "+" ? pa[i] + pb[i] : pa[i] - pb[i]));
    } else {
        const std::vector<double>& list = la ? pa : pb;
        const Value& operand = la ? b : a;
        double n;
        if (!try_number(operand, n))
            return mismatch(operand.is_null() ? std::string("null is not a number")
                                              : "a list combines with a number or a list of " + std::to_string(list.size()) + " numbers");
        if (!(op == "*" || (op == "/" && la)))
            return mismatch(op == "/" ? std::string("a number cannot be divided by a list")
                                      : "add a list of " + std::to_string(list.size()) + " numbers; multiply or divide by a number");
        if (op == "/" && n == 0) return Value::error("div-by-zero");
        result.reserve(list.size());
        for (double d : list) result.push_back(Value::number(op == "*" ? d * n : d / n));
    }
    return Value::list_of(std::move(result));
}

/// A vector (spec §4.3): a numeric list or a struct whose adapter declares components.
struct Vec {
    std::vector<double> parts;
    Value value;
    const TypeAdapter* adapter = nullptr;
};

bool vector_of(Exec& x, const Value& v, Vec& out) {
    out.value = v;
    out.adapter = nullptr;
    if (components_of(x, v, out.parts, out.adapter)) return true;
    return numeric_list(v, out.parts);
}

/// Σ pᵢ·qᵢ in order; every product and sum rounded separately (no fused multiply-add, spec §4.3).
double vector_dot(const std::vector<double>& p, const std::vector<double>& q) {
    volatile double sum = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        volatile double prod = p[i] * q[i];
        sum = sum + prod;
    }
    return sum;
}

/// a + (b - a) × t, each operation rounded separately.
double lerp1(double a, double b, double t) {
    volatile double span = b - a;
    volatile double step = span * t;
    return a + step;
}

/// A value of the first vector's kind: its struct type through the adapter, else a list of numbers.
Value vector_build(const std::string& name, const Vec& like, const std::vector<double>& parts) {
    if (!like.adapter) return number_list(parts);
    std::optional<Value> built = build_from_components(*like.adapter, like.value, parts);
    if (!built) return Value::error("type-mismatch", name + "(): cannot build " + describe_value(like.value) + " from components");
    return *built;
}

Value not_vector(const std::string& name, const Value& v) {
    const bool sized = v.is_string() || v.is_list() || v.is_map();
    return Value::error("type-mismatch", name + "() takes a vector: a list of numbers or a struct with component-wise arithmetic, got " + describe_value(v) +
                                             (sized ? "; for the size of a string, list or map use len(â¦)" : ""));
}

Value bad_t(const Value& t) { return Value::error("type-mismatch", "lerp() expects a number t, got " + describe_value(t)); }

/// Vector functions (spec §4.3): length / magnitude, normalize, distance, dot, lerp.
Value vector_function(Exec& x, const std::string& name, const std::vector<Value>& vals) {
    const bool lerp = name == "lerp";
    double t = 0, na, nb;
    const bool t_ok = lerp && try_number(vals[2], t);
    if (lerp && try_number(vals[0], na) && try_number(vals[1], nb)) return t_ok ? Value::number(lerp1(na, nb, t)) : bad_t(vals[2]);
    Vec a, b;
    if (!vector_of(x, vals[0], a)) return not_vector(name, vals[0]);
    if (vals.size() > 1) {
        if (!vector_of(x, vals[1], b)) return not_vector(name, vals[1]);
        if (a.adapter && b.adapter && a.value.type() != b.value.type())
            return Value::error("type-mismatch", name + "() needs vectors of one type, got " + describe_value(a.value) + " and " + describe_value(b.value));
        if (a.parts.size() != b.parts.size())
            return Value::error("type-mismatch", name + "() needs vectors of the same width, got " + std::to_string(a.parts.size()) + " and " +
                                                     std::to_string(b.parts.size()) + " components");
    }
    if (name == "length" || name == "magnitude") return Value::number(std::sqrt(vector_dot(a.parts, a.parts)));
    if (name == "dot") return Value::number(vector_dot(a.parts, b.parts));
    if (name == "distance") {
        volatile double sum = 0;
        for (size_t i = 0; i < a.parts.size(); ++i) {
            volatile double d = a.parts[i] - b.parts[i];
            volatile double sq = d * d;
            sum = sum + sq;
        }
        return Value::number(std::sqrt(static_cast<double>(sum)));
    }
    if (name == "normalize") {
        const double len = std::sqrt(vector_dot(a.parts, a.parts));
        if (len == 0) return vector_build(name, a, a.parts);  // a zero vector stays zero (a new value)
        std::vector<double> r(a.parts.size());
        for (size_t i = 0; i < r.size(); ++i) r[i] = a.parts[i] / len;
        return vector_build(name, a, r);
    }
    if (!t_ok) return bad_t(vals[2]);  // lerp
    std::vector<double> r(a.parts.size());
    for (size_t i = 0; i < r.size(); ++i) r[i] = lerp1(a.parts[i], b.parts[i], t);
    return vector_build(name, a, r);
}

bool is_vector_function(const std::string& name) {
    return name == "length" || name == "magnitude" || name == "normalize" || name == "distance" || name == "dot" || name == "lerp";
}

Value arith(Exec& x, const std::string& op, const Value& a, const Value& b) {
    if (a.is_missing() || b.is_missing()) return Value::missing();
    if (a.is_error()) return a;
    if (b.is_error()) return b;
    if (op == "+" && (a.is_string() || b.is_string())) return Value::string(to_text(a) + to_text(b));
    double l, r;
    if (!try_number(a, l) || !try_number(b, r)) {
        if (x.has_capability(Capability::Components))
            if (auto v = struct_arith(x, op, a, b)) return *v;
        if (!a.is_struct() && !b.is_struct())
            if (auto v = list_arith(op, a, b)) return *v;
        return arith_mismatch(x, op, a, b);
    }
    if (op == "neg") return Value::number(-l);
    if (op == "+") return Value::number(l + r);
    if (op == "-") return Value::number(l - r);
    if (op == "*") return Value::number(l * r);
    if (op == "/") return r == 0 ? Value::error("div-by-zero") : Value::number(l / r);
    if (op == "%") return r == 0 ? Value::error("div-by-zero") : Value::number(std::fmod(l, r));
    return Value::error("type-mismatch");
}

}  // namespace

Eval Compiler::compile_op(const Expr& o) { return compile_op_args(o.name, o.args[0], o.args.size() > 1 ? o.args[1] : nullptr); }

Eval Compiler::compile_condition(const Expr& e) {
    Eval ev = compile(e);
    const Expr* ep = &e;
    return [ev, ep](Exec& x) {
        Value v = ev(x);
        if (v.is_bool()) return v;
        if (v.is_row() && v.row().values.size() == 1) v = v.row().values[0];  // a one-column row stands for its value (spec §3)
        if (v.is_null() || v.is_bool() || v.is_absent()) return v;
        throw not_a_condition(*ep, v);
    };
}

Eval Compiler::compile_op_args(const std::string& op, const ExprPtr& ae, const ExprPtr& be) {
    const bool logical = op == "and" || op == "or" || op == "not";
    auto operand = [&](const Expr& e) { return logical ? compile_condition(e) : compile(e); };
    Eval a = operand(*ae);
    Eval b = be ? operand(*be) : Eval();
    if (op == "and")
        return [a, b](Exec& x) {
            auto l = truth(a(x));
            if (l == std::optional<bool>(false)) return Value::boolean(false);
            auto r = truth(b(x));
            if (r == std::optional<bool>(false)) return Value::boolean(false);
            return l == std::optional<bool>(true) && r == std::optional<bool>(true) ? Value::boolean(true) : Value::unknown();
        };
    if (op == "or")
        return [a, b](Exec& x) {
            auto l = truth(a(x));
            if (l == std::optional<bool>(true)) return Value::boolean(true);
            auto r = truth(b(x));
            if (r == std::optional<bool>(true)) return Value::boolean(true);
            return l == std::optional<bool>(false) && r == std::optional<bool>(false) ? Value::boolean(false) : Value::unknown();
        };
    if (op == "not")
        return [a](Exec& x) {
            auto t = truth(a(x));
            return t ? Value::boolean(!*t) : Value::unknown();
        };
    if (op == "==") return [a, b](Exec& x) { Value l = a(x); return tri(rql_equal(x, l, b(x))); };
    if (op == "!=")
        return [a, b](Exec& x) {
            Value l = a(x);
            auto t = rql_equal(x, l, b(x));
            return t ? Value::boolean(!*t) : Value::unknown();
        };
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        int kind = op == "<" ? 0 : op == "<=" ? 1 : op == ">" ? 2 : 3;
        return [a, b, kind](Exec& x) {
            Value l = a(x);
            auto c = rql_compare(x, l, b(x));
            if (!c) return Value::unknown();
            bool r = kind == 0 ? *c < 0 : kind == 1 ? *c <= 0 : kind == 2 ? *c > 0 : *c >= 0;
            return Value::boolean(r);
        };
    }
    if (op == "contains") return [a, b](Exec& x) { Value l = a(x); return tri(contains_op(x, l, b(x))); };
    if (op == "starts-with") return [a, b](Exec& x) { Value l = a(x); return tri(string_test(l, b(x), true)); };
    if (op == "ends-with") return [a, b](Exec& x) { Value l = a(x); return tri(string_test(l, b(x), false)); };
    if (op == "matches") {
        check_literal_regex(x_, be.get());
        return [a, b](Exec& x) {
            Value l = a(x);
            Value r = tri(matches_op(x, l, b(x)));
            x.check_time();  // a per-match bound alone does not bound the whole query
            return r;
        };
    }
    if (op == "in") return [a, b](Exec& x) { Value l = a(x); return tri(in_op(x, l, b(x))); };
    if (op == "is") {
        IsSite* site = nullptr;
        if (be && be->kind == ExprKind::Lit && be->lit.type == Literal::Type::String) {
            x_.is_sites.push_back(std::make_unique<IsSite>());
            site = x_.is_sites.back().get();
            site->type_name = be->lit.s;
        }
        return [a, b, site](Exec& x) {
            Value v = a(x);
            auto r = is_type(v, b(x));
            if (site && r) {
                ++site->evaluated;
                if (*r) ++site->matched;
                else if (!v.is_null() && site->seen.size() < 6) {
                    std::string n = type_name_of(v);
                    if (std::find(site->seen.begin(), site->seen.end(), n) == site->seen.end()) site->seen.push_back(n);
                }
            }
            return tri(r);
        };
    }
    if (op == "neg") return [a](Exec& x) { return arith(x, "neg", a(x), Value::number(0)); };
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
        return [a, b, op](Exec& x) {
            Value l = a(x);
            return arith(x, op, l, b(x));
        };
    if (op == "index")
        return [a, b](Exec& x) {
            Value v = a(x);
            Value k = b(x);
            if (k.is_absent()) return k;
            if (v.is_null()) return Value::missing();
            if (v.is_absent()) return v;
            if (v.is_map()) return map_lookup(x, v, (k.is_number() || k.is_string()) ? k : Value::string(to_text(k)), nullptr);
            double d;
            if (try_number(k, d) && std::floor(d) == d) return x.live(index_at(v, static_cast<int64_t>(d)));
            if (k.is_string()) {
                MemberSite tmp(k.as_string());
                return member_of(x, v, tmp);
            }
            return Value::missing();
        };
    throw RqlException("invalid-plan", "unknown operator '" + op + "'");
}

// ------------------------------------------------------------------ calls

namespace {

const char* kCoreFunctions[] = {"has", "is-null", "len", "lower", "upper", "abs", "round", "floor", "ceil", "sqrt", "pow", "min", "max",
                                "coalesce", "if", "ref",
                                "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pi", "deg", "rad",
                                "append", "insert", "remove-at", "without", "range",
                                "replace", "replace-regex", "substring", "split", "trim", "pad",
                                "contains", "starts-with", "ends-with", "matches",
                                "length", "magnitude", "normalize", "distance", "dot", "lerp"};
const char* kCoreMethods[] = {"any", "all", "count", "sum", "min", "max", "avg", "first", "keys", "values",
                              "contains", "starts-with", "ends-with", "matches", "lower", "upper", "len",
                              "replace", "replace-regex", "substring", "split", "trim", "pad",
                              "length", "magnitude", "normalize", "distance", "dot", "lerp"};

// ------------------------------------------------------------------ string functions (spec §4.3)

bool is_string_function(const std::string& name) {
    return name == "replace" || name == "replace-regex" || name == "substring" || name == "split" || name == "trim" || name == "pad";
}

/// The widest result pad builds.
constexpr int64_t kMaxPadWidth = 1024;

/// An integral number a double holds exactly (up to 2^53).
bool integral_arg(const Value& v, int64_t& n) {
    double d;
    if (!try_number(v, d) || std::isnan(d) || std::floor(d) != d || std::fabs(d) > 9007199254740992.0) return false;  // up to 2^53; positions beyond the text are clamped
    n = static_cast<int64_t>(d);
    return true;
}

/// Byte offset of the UTF-16 position `units` in UTF-8 text (positions count UTF-16 code units, like len).
/// Counting exactly like utf16_length (continuation bytes add nothing, a lead byte from 0xF0 adds 2); a position inside
/// a surrogate pair rounds up to the end of the character.
size_t utf8_offset(const std::string& s, int64_t units) {
    int64_t u = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if ((ch & 0xC0) == 0x80) continue;  // continuation byte
        if (u >= units) return i;
        u += ch >= 0xF0 ? 2 : 1;
    }
    return s.size();
}

/// Whether position `at` of UTF-8 text `s` falls inside a character (on a continuation byte).
bool splits_character(const std::string& s, std::string::const_iterator at) {
    return at != s.cend() && (static_cast<unsigned char>(*at) & 0xC0) == 0x80;
}

/// Start offsets of every non-overlapping occurrence of `needle` in `hay`, left to right, ASCII case-insensitive
/// (like contains; folding ASCII keeps byte offsets).
std::vector<size_t> ifind_all(const std::string& hay, const std::string& needle) {
    const std::string h = ascii_lower(hay), n = ascii_lower(needle);
    std::vector<size_t> hits;
    for (size_t at = h.find(n); at != std::string::npos; at = h.find(n, at + n.size())) hits.push_back(at);
    return hits;
}

/// Portable replacement text: $0-$9 insert a group (empty when absent), $$ a dollar, any other $ is literal.
std::string expand_replacement(const std::smatch& m, const std::string& repl) {
    std::string out;
    for (size_t i = 0; i < repl.size(); ++i) {
        char ch = repl[i];
        if (ch == '$' && i + 1 < repl.size()) {
            char next = repl[i + 1];
            if (next == '$') {
                out += '$';
                ++i;
                continue;
            }
            if (next >= '0' && next <= '9') {
                size_t g = static_cast<size_t>(next - '0');
                if (g < m.size() && m[g].matched) out += m[g].str();
                ++i;
                continue;
            }
        }
        out += ch;
    }
    return out;
}

Value string_function(Exec& x, const std::string& name, const std::vector<Value>& vals) {
    if (name == "pad") {
        // A number pads its text form: pad(3, 2) -> "03".
        const Value& v = vals[0];
        if (!v.is_string() && !v.is_number()) return Value::error("type-mismatch", "pad() needs a string or a number, got " + describe_value(v));
        std::string text = v.is_string() ? v.as_string() : to_text(v);
        int64_t width = 0;
        if (!integral_arg(vals[1], width) || width < 0 || width > kMaxPadWidth)
            return Value::error("type-mismatch", "pad() width must be an integer from 0 to " + std::to_string(kMaxPadWidth));
        std::string fill = "0";
        if (vals.size() > 2) {
            if (!vals[2].is_string() || utf16_length(vals[2].as_string()) != 1)
                return Value::error("type-mismatch", "pad() fill must be a one-character string");
            fill = vals[2].as_string();
        }
        std::string prefix;
        for (int64_t have = static_cast<int64_t>(utf16_length(text)); have < width; ++have) prefix += fill;
        return Value::string(prefix + text);
    }
    if (!vals[0].is_string()) return Value::error("type-mismatch", name + "() needs a string, got " + describe_value(vals[0]));
    const std::string& s = vals[0].as_string();
    if (name == "trim") {
        static const char* ws = " \t\n\r\v\f";
        size_t a = s.find_first_not_of(ws);
        if (a == std::string::npos) return Value::string("");
        size_t b = s.find_last_not_of(ws);
        return Value::string(s.substr(a, b - a + 1));
    }
    if (name == "replace" || name == "split") {
        if (!vals[1].is_string() || (name == "replace" && !vals[2].is_string())) return Value::error("type-mismatch", name + "() needs string arguments");
        const std::string& from = vals[1].as_string();
        if (name == "split") {
            if (from.empty()) return Value::error("type-mismatch", "split() needs a non-empty separator");
            std::vector<Value> parts;
            size_t start = 0;
            for (size_t at : ifind_all(s, from)) {
                parts.push_back(Value::string(s.substr(start, at - start)));
                start = at + from.size();
            }
            parts.push_back(Value::string(s.substr(start)));
            return Value::list_of(std::move(parts));
        }
        if (from.empty()) return vals[0];
        const std::string& to = vals[2].as_string();
        std::string out;
        size_t pos = 0;
        for (size_t hit : ifind_all(s, from)) {
            out.append(s, pos, hit - pos);
            out += to;
            pos = hit + from.size();
        }
        out.append(s, pos, std::string::npos);
        return Value::string(out);
    }
    if (name == "substring") {
        int64_t start = 0, want = 0;
        if (!integral_arg(vals[1], start) || (vals.size() > 2 && !integral_arg(vals[2], want)))
            return Value::error("type-mismatch", "substring() needs integral start and length");
        if (vals.size() > 2 && want < 0) return Value::error("type-mismatch", "substring() length must not be negative");
        const int64_t n = static_cast<int64_t>(utf16_length(s));
        if (start < 0) start += n;
        start = std::max<int64_t>(0, std::min(n, start));
        int64_t count = n - start;
        if (vals.size() > 2) count = std::min(count, want);
        size_t a = utf8_offset(s, start);
        size_t b = utf8_offset(s, start + count);
        return Value::string(s.substr(a, b - a));
    }
    // replace-regex
    if (!vals[1].is_string() || !vals[2].is_string()) return Value::error("type-mismatch", "replace-regex() needs string arguments");
    const std::string& pattern = vals[1].as_string();
    const Exec::CachedRegex cached = compiled_regex(x, pattern);
    if (!cached.re) {
        std::string detail = invalid_regex_message(pattern, cached.error);
        x.warn_once("invalid-regex", detail, "invalid-regex");
        return Value::error("invalid-regex", detail);
    }
    const std::string& repl = vals[2].as_string();
    try {
        std::string out;
        auto last = s.cbegin();
        for (std::sregex_iterator it(s.cbegin(), s.cend(), *cached.re), end; it != end; ++it) {
            const std::smatch& m = *it;
            // std::regex works on bytes: a match that would split a UTF-8 character is skipped (spec §12).
            if (splits_character(s, m[0].first) || splits_character(s, m[0].second)) continue;
            out.append(last, m[0].first);
            out += expand_replacement(m, repl);
            last = m[0].second;
        }
        out.append(last, s.cend());
        return Value::string(out);
    } catch (const std::regex_error& ex) {
        std::string detail = "regex \"" + pattern + "\" failed on a value (" + ex.what() + "); simplify the pattern";
        x.warn_once("timeout", detail, "regex-timeout");
        return Value::error("timeout", detail);
    }
}

/// The ref() hint: malformed text is usually a name, not a $ref (spec §7).
std::string ref_hint(const Engine& e, const std::string& reference) {
    std::string t = reference;
    size_t a = t.find_first_not_of(" \t\r\n");
    size_t b = t.find_last_not_of(" \t\r\n");
    t = a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    size_t i = t.find_first_of("#@");
    std::string find = !e.find_hint.empty() ? "look it up with " + e.find_hint : "look it up by name: \xE2\x80\xA6 | where Name == \"\xE2\x80\xA6\"";
    return i == std::string::npos || i == 0 || i == t.size() - 1
               ? "ref() takes a $ref from a query result (Type#id), not a name; " + find
               : "the object may be gone or its $ref stale; re-query it (" + find + ") to get a fresh $ref";
}

Value len_of(const Value& v) {
    if (v.is_null()) return Value::missing();
    if (v.is_absent()) return v;
    if (v.is_string() || v.is_list() || v.is_map()) return Value::number(static_cast<double>(collection_size(v)));
    return Value::missing();
}

Value case_of(const Value& v, bool upper) {
    if (v.is_null() || v.is_absent()) return v;
    if (!v.is_string()) return Value::error("type-mismatch");
    return Value::string(upper ? ascii_upper(v.as_string()) : ascii_lower(v.as_string()));
}

/// Trigonometric results are rounded to 12 decimal places (spec §4.3), so runtimes whose math libraries differ in the last
/// bit return the same digits (and sin(pi()) is 0, never -0).
double round_away(double v, int digits);
double trig(double d) { return round_away(d, 12) + 0.0; }  // -0 + 0 is +0
/// Inverse trigonometric results (radians) are rounded to 14 places, so deg() of them, rounded to 12, comes out clean:
/// deg(atan2(1, 1)) is 45 (spec §4.3).
double trig_inverse(double d) { return round_away(d, 14) + 0.0; }
constexpr double kPi = 3.14159265358979323846;

Value num_of(const Value& v, double (*f)(double)) {
    if (v.is_null() || v.is_absent()) return v;
    double d;
    if (!try_number(v, d)) return Value::error("type-mismatch");
    return Value::number(f(d));
}

/// Like .NET Math.Min / Math.Max: NaN when either argument is NaN; -0 orders below +0.
double math_min(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    if (a == b) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}

double math_max(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    if (a == b) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}

double round_away(double v, int digits) {
    if (digits == 0) return std::round(v);
    if (std::fabs(v) >= 1e16) return v;
    double p = std::pow(10.0, digits);
    return std::round(v * p) / p;
}

/// Collection elements enter expressions here: destroyed objects read as null.
void live_all(Exec& x, std::vector<Value>& els) {
    if (!x.liveness_enabled()) return;
    for (auto& el : els) el = x.live(std::move(el));
}

Value quantify(Exec& x, const Value& v, const Eval* pred, const std::string& kind) {
    if (v.is_missing() || v.is_error()) return (kind == "count" || kind == "first") ? Value::missing() : Value::unknown();
    std::vector<Value> els;
    if (!elements(v, els)) {
        if (v.is_null()) {
            if (kind == "count") return Value::number(0);
            if (kind == "first") return Value::missing();
            return Value::boolean(kind == "all");
        }
        els = {v};  // a single object behaves as a one-element collection
    }
    live_all(x, els);
    ScopeGuard guard(x);
    double count = 0;
    bool saw_unknown = false;
    int64_t i = 0;
    for (auto& el : els) {
        x.visit();  // collection-method elements count toward maxVisited/timeout (spec §10)
        std::optional<bool> t = true;
        if (pred) {
            x.current = el;
            x.current_path = nullptr;
            x.index = i;
            t = truth((*pred)(x));
        }
        ++i;
        if (kind == "any") {
            if (t == std::optional<bool>(true)) return Value::boolean(true);
            if (!t) saw_unknown = true;
        } else if (kind == "all") {
            if (t == std::optional<bool>(false)) return Value::boolean(false);
            if (!t) saw_unknown = true;
        } else if (kind == "first") {
            if (t == std::optional<bool>(true)) return el;
        } else if (t == std::optional<bool>(true)) {
            ++count;
        }
    }
    if (kind == "any") return saw_unknown ? Value::unknown() : Value::boolean(false);
    if (kind == "all") return saw_unknown ? Value::unknown() : Value::boolean(true);
    if (kind == "first") return Value::missing();
    return Value::number(count);
}

Value aggregate(Exec& x, const Value& v, const Eval* sel, const std::string& kind) {
    if (v.is_missing() || v.is_error()) return v;
    std::vector<Value> els;
    if (!elements(v, els)) return v.is_null() ? Value::null() : Value::missing();
    live_all(x, els);
    ScopeGuard guard(x);
    std::vector<Value> values;
    values.reserve(els.size());
    for (auto& el : els) {
        x.visit();
        if (sel) {
            x.current = el;
            x.current_path = nullptr;
            values.push_back((*sel)(x));
        } else {
            values.push_back(el);
        }
    }
    return reduce(x, values, kind);
}

}  // namespace

double range_at(double start, double step, int64_t k) {
    // Two separately rounded operations, never a fused multiply-add (arm64 compilers contract a + b * c), so every runtime
    // produces the same numbers and the same count.
    volatile double offset = static_cast<double>(k) * step;
    return start + offset;
}

bool range_of(const Value& a, const Value& b, const Value& c, double& start, double& step, int64_t& count, Value& error) {
    double nums[3] = {0, 0, 1};
    const Value* vals[3] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        const Value& v = *vals[i];
        if (v.is_error()) {
            error = v;
            return false;
        }
        if (!try_number(v, nums[i])) {
            error = Value::error("type-mismatch", "range() expects numbers, got " + describe_value(v));
            return false;
        }
        if (!std::isfinite(nums[i])) {
            error = Value::error("type-mismatch", "range() expects finite numbers, got " + format_number_text(nums[i]));
            return false;
        }
    }
    start = nums[0];
    step = nums[2];
    if (step == 0) {
        error = Value::error("type-mismatch", "range() step must not be 0");
        return false;
    }
    const double n = std::ceil((nums[1] - start) / step);
    count = n <= 0 ? 0 : n >= 9.2e18 ? INT64_MAX : static_cast<int64_t>(n);
    // end is excluded: rounding in (end - start) / step can count one number too many (range(0, 2.1, 0.3)).
    while (count > 0 && count < INT64_MAX &&
           (step > 0 ? range_at(start, step, count - 1) >= nums[1] : range_at(start, step, count - 1) <= nums[1]))
        --count;
    return true;
}

void Compiler::arity(const Expr& c, size_t min, size_t max) {
    if (c.args.size() < min || c.args.size() > max) {
        std::string expect = min == max ? std::to_string(min) : std::to_string(min) + "-" + std::to_string(max);
        throw RqlException("arity", "'" + c.name + "' takes " + expect + " argument(s), got " + std::to_string(c.args.size()));
    }
}

const FunctionHandler* extension_function(Exec& x, const Expr& c, std::string& name) {
    if (c.kind != ExprKind::Call) return nullptr;
    const auto& fns = x.engine.functions();
    if (!c.on) {
        if (c.name.find('.') == std::string::npos) return nullptr;
        auto it = fns.find(c.name);
        if (it == fns.end()) return nullptr;
        name = it->first;
        return &it->second;
    }
    // ns.fn(x) parses as method fn on path ns: rewrite when a namespaced function is registered (spec §9).
    if (c.on->kind != ExprKind::Path || c.on->on || c.on->segments.empty()) return nullptr;
    std::vector<std::string> parts;
    for (auto& s : c.on->segments) {
        if (s.kind != SegmentKind::Member) return nullptr;
        parts.push_back(s.name);
    }
    auto it = fns.find(join(parts, ".") + "." + c.name);
    if (it == fns.end()) return nullptr;
    name = it->first;
    return &it->second;
}

Eval Compiler::compile_call(const Expr& c) {
    std::string ext;
    if (const FunctionHandler* fn = extension_function(x_, c, ext)) return compile_extension(ext, *fn, c.args);
    if (!c.on) return compile_function(c);
    return compile_method(c);
}

Eval Compiler::compile_extension(const std::string& name, const FunctionHandler& fn, const std::vector<ExprPtr>& argExprs) {
    std::vector<Eval> args;
    for (auto& a : argExprs) args.push_back(compile(*a));
    return [name, fn, args](Exec& x) {
        std::vector<Value> vals;
        vals.reserve(args.size());
        for (auto& a : args) vals.push_back(a(x));
        std::string what;
        try {
            return fn(vals);
        } catch (const std::exception& ex) {
            what = ex.what();
        } catch (...) {
            what = "non-standard exception";
        }
        // spec §9: an error value for the call plus one warning per function.
        std::string detail = name + "() threw: " + what;
        x.warn_once("function-threw", detail, "function-threw:" + name);
        return Value::error("function-threw", detail);
    };
}

Eval Compiler::compile_function(const Expr& c) {
    std::string name = ascii_lower(c.name);
    if (name == "has") {
        arity(c, 1, 1);
        struct Guard {
            int& depth;
            explicit Guard(int& d) : depth(d) { ++depth; }
            ~Guard() { --depth; }
        } guard(guarded_);
        Eval a = compile(*c.args[0]);
        return [a](Exec& x) {
            struct Probe {
                int& depth;
                explicit Probe(int& d) : depth(d) { ++depth; }
                ~Probe() { --depth; }
            } probe(x.key_probe);
            return Value::boolean(!a(x).is_missing());
        };
    }
    if (name == "is-null") {
        arity(c, 1, 1);
        Eval a = compile(*c.args[0]);
        return [a](Exec& x) { return tri(rql_equal(x, a(x), Value::null())); };
    }
    if (name == "len") {
        arity(c, 1, 1);
        Eval a = compile(*c.args[0]);
        return [a](Exec& x) { return len_of(a(x)); };
    }
    if (name == "lower" || name == "upper") {
        arity(c, 1, 1);
        Eval a = compile(*c.args[0]);
        bool up = name == "upper";
        return [a, up](Exec& x) { return case_of(a(x), up); };
    }
    if (name == "pi") {
        arity(c, 0, 0);
        return [](Exec&) { return Value::number(kPi); };
    }
    if (name == "abs" || name == "floor" || name == "ceil" || name == "sqrt" || name == "sin" || name == "cos" || name == "tan" ||
        name == "asin" || name == "acos" || name == "atan" || name == "deg" || name == "rad") {
        arity(c, 1, 1);
        Eval a = compile(*c.args[0]);
        using Unary = double (*)(double);
        // deg / rad multiply, then divide, so every runtime returns the same digits; degrees are rounded like trig results
        // (60, not 59.99999999999999), radians are not (rad(180) is pi()).
        Unary f = name == "abs"    ? static_cast<Unary>(std::fabs)
                  : name == "floor" ? static_cast<Unary>(std::floor)
                  : name == "ceil"  ? static_cast<Unary>(std::ceil)
                  : name == "sqrt"  ? static_cast<Unary>(std::sqrt)  // IEEE: sqrt(-1) is NaN
                  : name == "sin"   ? +[](double v) { return trig(std::sin(v)); }
                  : name == "cos"   ? +[](double v) { return trig(std::cos(v)); }
                  : name == "tan"   ? +[](double v) { return trig(std::tan(v)); }
                  : name == "asin"  ? +[](double v) { return trig_inverse(std::asin(v)); }
                  : name == "acos"  ? +[](double v) { return trig_inverse(std::acos(v)); }
                  : name == "atan"  ? +[](double v) { return trig_inverse(std::atan(v)); }
                  : name == "deg"   ? +[](double v) { return trig(v * 180 / kPi); }
                                    : +[](double v) { return v * kPi / 180; };
        return [a, f](Exec& x) { return num_of(a(x), f); };
    }
    if (name == "pow" || name == "atan2" || name == "min" || name == "max") {
        if (name == "pow" || name == "atan2") arity(c, 2, 2);
        else arity(c, 2, 64);
        std::vector<Eval> args;
        for (auto& e : c.args) args.push_back(compile(*e));
        double (*f)(double, double) = name == "pow"     ? static_cast<double (*)(double, double)>(std::pow)
                                      : name == "atan2" ? +[](double y, double xv) { return trig_inverse(std::atan2(y, xv)); }
                                      : name == "min"   ? math_min
                                                        : math_max;
        return [args, f, name](Exec& x) -> Value {
            double acc = 0;
            for (size_t i = 0; i < args.size(); ++i) {
                Value v = args[i](x);
                if (v.is_null() || v.is_absent()) return v;  // the first null / missing / error / unknown argument
                double d;
                if (!try_number(v, d)) return Value::error("type-mismatch", name + "() expects numbers, got " + describe_value(v));
                acc = i == 0 ? d : f(acc, d);
            }
            return Value::number(acc);
        };
    }
    if (name == "round") {
        arity(c, 1, 2);
        Eval a = compile(*c.args[0]);
        Eval digits = c.args.size() > 1 ? compile(*c.args[1]) : Eval();
        return [a, digits](Exec& x) {
            int dg = 0;
            double dd;
            if (digits && try_number(digits(x), dd)) dd = std::max(0.0, std::min(15.0, dd)), dg = static_cast<int>(dd);
            Value v = a(x);
            if (v.is_null() || v.is_absent()) return v;
            double d;
            if (!try_number(v, d)) return Value::error("type-mismatch");
            return Value::number(round_away(d, dg));
        };
    }
    // List editing (spec §4.3): pure functions returning a new list; assign the result with `set`.
    if (name == "range") {
        arity(c, 2, 3);
        std::vector<Eval> args;
        for (auto& e : c.args) args.push_back(compile(*e));
        return [args](Exec& x) -> Value {
            double start = 0, step = 1;
            int64_t count = 0;
            Value error;
            // Left to right, like every other function (C++ argument evaluation order is unspecified).
            const Value from = args[0](x);
            const Value to = args[1](x);
            const Value by = args.size() > 2 ? args[2](x) : Value::number(1);
            if (!range_of(from, to, by, start, step, count, error)) return error;
            std::vector<Value> list;
            for (int64_t k = 0; k < count; ++k) {
                x.visit();  // a range is bounded by maxVisited like any stream (spec §2)
                list.push_back(Value::number(range_at(start, step, k)));
            }
            return Value::list_of(std::move(list));
        };
    }
    if (name == "append" || name == "insert" || name == "remove-at" || name == "without") {
        enum class ListFn { Append, Insert, RemoveAt, Without };
        const ListFn fn = name == "append" ? ListFn::Append : name == "insert" ? ListFn::Insert : name == "remove-at" ? ListFn::RemoveAt : ListFn::Without;
        if (fn == ListFn::Append) arity(c, 2, 64);
        else if (fn == ListFn::Insert) arity(c, 3, 3);
        else arity(c, 2, 2);
        std::vector<Eval> args;
        for (auto& e : c.args) args.push_back(compile(*e));
        return [args, name, fn](Exec& x) -> Value {
            Value first = args[0](x);
            if (!first.is_list()) return Value::error("type-mismatch", name + "() expects a list first");
            std::vector<Value> list;
            elements(first, list);  // a fresh copy: the argument is never modified
            live_all(x, list);
            auto index = [&](const Value& iv, bool inserting, int64_t& out) {
                double d;
                if (!try_number(iv, d) || !std::isfinite(d) || std::floor(d) != d) return false;
                int64_t n = static_cast<int64_t>(list.size());
                if (std::fabs(d) > static_cast<double>(n) + 1) return false;  // out of range: reject before casting (no UB)
                int64_t i = static_cast<int64_t>(d);
                if (i < 0) i += inserting ? n + 1 : n;
                if (i < 0 || i > n || (!inserting && i == n)) return false;
                out = i;
                return true;
            };
            if (fn == ListFn::Append) {
                for (size_t i = 1; i < args.size(); ++i) list.push_back(args[i](x));
            } else if (fn == ListFn::Insert) {
                int64_t i = 0;
                if (!index(args[1](x), true, i)) return Value::error("type-mismatch", "insert() index must be an integer in range");
                list.insert(list.begin() + i, args[2](x));
            } else if (fn == ListFn::RemoveAt) {
                int64_t i = 0;
                if (!index(args[1](x), false, i)) return Value::error("type-mismatch", "remove-at() index must be an integer in range");
                list.erase(list.begin() + i);
            } else {
                Value drop = args[1](x);
                list.erase(std::remove_if(list.begin(), list.end(),
                                          [&](const Value& e) { return rql_equal(x, e, drop) == std::optional<bool>(true); }),
                           list.end());
            }
            return Value::list_of(std::move(list));
        };
    }
    if (name == "coalesce") {
        arity(c, 1, 16);
        std::vector<Eval> args;
        for (auto& e : c.args) args.push_back(compile(*e));
        return [args](Exec& x) {
            for (auto& a : args) {
                Value v = a(x);
                if (!v.is_null() && !v.is_absent()) return v;
            }
            return Value::null();
        };
    }
    if (name == "if") {
        // Lazy: only the taken branch runs; a condition that is not true (false or unknown) takes the else branch.
        arity(c, 3, 3);
        Eval cond = compile_condition(*c.args[0]), then = compile(*c.args[1]), otherwise = compile(*c.args[2]);
        return [cond, then, otherwise](Exec& x) { return truth(cond(x)) == std::optional<bool>(true) ? then(x) : otherwise(x); };
    }
    if (name == "ref") {
        arity(c, 1, 64);
        std::vector<Eval> args;
        for (auto& e : c.args) args.push_back(compile(*e));
        return [args](Exec& x) {
            std::vector<Value> found;
            for (auto& a : args) {
                Value v = a(x);
                std::vector<Value> refs;
                if (v.is_list()) elements(v, refs);
                else refs.push_back(v);
                for (auto& r : refs) {
                    if (!r.is_string()) return Value::error("type-mismatch", "ref() expects reference strings");
                    std::string err;
                    Value o = x.resolve_ref(r.as_string(), err);
                    if (o.is_missing()) {
                        x.diagnostics.push_back(Diagnostic::error("stale-ref", err, ref_hint(x.engine, r.as_string())));
                        return Value::error("stale-ref", err);
                    }
                    found.push_back(o);
                }
            }
            return found.size() == 1 ? found[0] : Value::list_of(std::move(found));
        };
    }
    if (name == "contains" || name == "starts-with" || name == "ends-with" || name == "matches") {
        arity(c, 2, 2);
        return compile_op_args(name, c.args[0], c.args[1]);
    }
    if (is_string_function(name)) {
        std::vector<const Expr*> args;
        for (auto& a : c.args) args.push_back(a.get());
        return compile_string_function(c, name, args, 0);
    }
    if (is_vector_function(name)) {
        std::vector<const Expr*> args;
        for (auto& a : c.args) args.push_back(a.get());
        return compile_vector_function(c, name, args, 0);
    }
    std::vector<std::string> known;
    for (auto* f : kCoreFunctions) known.push_back(f);
    for (auto& kv : x_.engine.functions()) known.push_back(kv.first);
    throw RqlException("unknown-function", "unknown function '" + c.name + "'",
                       "functions: " + join(known, " ") + "; collection methods: list.any(pred) list.count() ...");
}

Eval Compiler::compile_method(const Expr& c) {
    Eval on = compile(*c.on);
    std::string name = ascii_lower(c.name);
    if (name == "any" || name == "all" || name == "count" || name == "first") {
        arity(c, name == "all" ? 1 : 0, 1);
        auto pred = c.args.empty() ? nullptr : std::make_shared<Eval>(compile_condition(*c.args[0]));
        return [on, pred, name](Exec& x) { return quantify(x, on(x), pred.get(), name); };
    }
    if (name == "sum" || name == "min" || name == "max" || name == "avg") {
        arity(c, 0, 1);
        auto sel = c.args.empty() ? nullptr : std::make_shared<Eval>(compile(*c.args[0]));
        return [on, sel, name](Exec& x) { return aggregate(x, on(x), sel.get(), name); };
    }
    if (name == "keys" || name == "values") {
        arity(c, 0, 0);
        bool keys = name == "keys";
        return [on, keys](Exec& x) {
            Value v = on(x);
            if (!v.is_map()) return Value::missing();
            std::vector<Value> out;
            v.map().for_each([&](const Value& k, const Value& val) {
                out.push_back(keys ? k : x.live(val));
                return true;
            });
            return Value::list_of(std::move(out));
        };
    }
    if (name == "contains" || name == "starts-with" || name == "ends-with" || name == "matches") {
        arity(c, 1, 1);
        return compile_op_args(name, c.on, c.args[0]);
    }
    if (name == "lower" || name == "upper") {
        arity(c, 0, 0);
        bool up = name == "upper";
        return [on, up](Exec& x) { return case_of(on(x), up); };
    }
    if (name == "len") {
        arity(c, 0, 0);
        return [on](Exec& x) { return len_of(on(x)); };
    }
    if (is_string_function(name)) {
        std::vector<const Expr*> args{c.on.get()};
        for (auto& a : c.args) args.push_back(a.get());
        return compile_string_function(c, name, args, 1);
    }
    if (is_vector_function(name)) {
        std::vector<const Expr*> args{c.on.get()};
        for (auto& a : c.args) args.push_back(a.get());
        return compile_vector_function(c, name, args, 1);
    }
    std::vector<std::string> known;
    for (auto* m : kCoreMethods) known.push_back(m);
    throw RqlException("unknown-function", "unknown method '" + c.name + "'", "methods: " + join(known, " "));
}

Eval Compiler::compile_vector_function(const Expr& /*c*/, const std::string& name, const std::vector<const Expr*>& argExprs, size_t receiver) {
    const size_t n = name == "lerp" ? 3 : name == "distance" || name == "dot" ? 2 : 1;
    if (argExprs.size() != n) {
        const std::string rest = n == 3 ? "w, 0.5" : n == 2 ? "w" : "";
        const std::string example = receiver == 1 ? "v." + name + "(" + rest + ")" : name + "(v" + (rest.empty() ? std::string() : ", " + rest) + ")";
        throw RqlException("arity", "'" + name + "' takes " + std::to_string(n - receiver) + " argument(s), got " + std::to_string(argExprs.size() - receiver),
                           "example: " + example);
    }
    std::vector<Eval> args;
    args.reserve(argExprs.size());
    for (auto* e : argExprs) args.push_back(compile(*e));
    return [args, name](Exec& x) {
        std::vector<Value> vals;
        vals.reserve(args.size());
        for (auto& a : args) {
            Value v = a(x);
            if (v.is_null() || v.is_absent()) return v;  // null / missing / error propagate (first one)
            vals.push_back(std::move(v));
        }
        return vector_function(x, name, vals);
    };
}

Eval Compiler::compile_string_function(const Expr& c, const std::string& name, const std::vector<const Expr*>& argExprs, size_t receiver) {
    size_t min = 1, max = 1;
    if (name == "replace" || name == "replace-regex") min = max = 3;
    else if (name == "substring" || name == "pad") min = 2, max = 3;
    else if (name == "split") min = max = 2;
    arity(c, min - receiver, max - receiver);
    if (name == "replace-regex") check_literal_regex(x_, argExprs[1]);
    std::vector<Eval> args;
    for (auto* e : argExprs) args.push_back(compile(*e));
    bool regex = name == "replace-regex";
    return [args, name, regex](Exec& x) {
        std::vector<Value> vals;
        for (auto& a : args) {
            Value v = a(x);
            if (v.is_null() || v.is_absent()) return v;  // null / missing / error propagate (first one)
            vals.push_back(std::move(v));
        }
        Value r = string_function(x, name, vals);
        if (regex) x.check_time();
        return r;
    };
}

}  // namespace detail
}  // namespace rql
