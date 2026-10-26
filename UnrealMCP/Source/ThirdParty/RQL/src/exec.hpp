// Internal execution state shared by compiler / pipeline / encoder / mutator.
#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rql/engine.hpp"

namespace rql {
namespace detail {

struct PathNode {
    std::shared_ptr<const PathNode> parent;
    std::string text;
    std::string str() const;
};
using PathPtr = std::shared_ptr<const PathNode>;
PathPtr path_text(PathPtr parent, std::string text);
PathPtr path_member(const PathPtr& parent, const std::string& name);
PathPtr path_at(const PathPtr& parent, int64_t i);
PathPtr path_key(const PathPtr& parent, const Value& key);

// ---------------------------------------------------------------- adapted types (spec §13)

enum class Capability : uint8_t { Encode, Coerce, Equals, Compare, StableId, IsAlive };
constexpr size_t kCapabilities = 6;

/// What adapters change for one type in one engine. Only built when some adapter applies; members are
/// copied only when an adapter configures them.
struct AdaptedType {
    std::vector<const TypeAdapter*> adapters;                               // most specific first
    std::array<std::vector<const TypeAdapter*>, kCapabilities> by_cap;      // adapters implementing each capability
    std::string description;                                                // effective type description
    bool owns_members = false;                                              // members configured by an adapter
    std::vector<MemberDescriptor> members;                                  // valid when owns_members
    const MemberDescriptor* id = nullptr;                                   // valid when owns_members
    const MemberDescriptor* display = nullptr;                              // valid when owns_members
    std::vector<MethodDescriptor> methods;                                  // valid when owns_members (host + adapter methods)
    bool has_explicit_only = false;
    std::vector<MemberDescriptor> implicit;                                 // members minus explicit-only (when any)
    bool writes_through = false;                                            // handle struct: no write-back (spec §13)
    std::optional<Json> literal_schema;                                     // from the most specific adapter providing one
};

/// Cheap handle: the descriptor plus its adaptation, if any. Without adapters every accessor reads the
/// descriptor directly.
class TypeView {
public:
    TypeView(const TypeDescriptor* type, const AdaptedType* adapted) noexcept : t_(type), a_(adapted) {}
    const TypeDescriptor* type() const { return t_; }
    const AdaptedType* adapted() const { return a_; }
    const std::vector<MemberDescriptor>& members() const { return a_ && a_->owns_members ? a_->members : t_->members; }
    /// Members read implicitly (generic recurse, inline/struct encoding, ref search, distinct): no explicit-only.
    const std::vector<MemberDescriptor>& implicit_members() const { return a_ && a_->has_explicit_only ? a_->implicit : members(); }
    const MemberDescriptor* find(const std::string& name) const { return find_member_in(members(), name); }
    const MemberDescriptor* id() const { return a_ && a_->owns_members ? a_->id : t_->id_member(); }
    const MemberDescriptor* display() const { return a_ && a_->owns_members ? a_->display : t_->display_member(); }
    bool member_writable(const MemberDescriptor& m) const { return rql::member_writable(m, id()); }
    /// Callable methods: host-registered plus adapter methods (spec §13), unsorted.
    const std::vector<MethodDescriptor>& methods() const { return a_ && a_->owns_members ? a_->methods : t_->methods; }
    const std::string& description() const { return a_ ? a_->description : t_->description; }
    const std::vector<const TypeAdapter*>& adapters() const { return a_ ? a_->adapters : empty(); }
    bool writes_through() const { return a_ && a_->writes_through; }
    const Json* literal_schema() const { return a_ && a_->literal_schema ? &*a_->literal_schema : nullptr; }
    const std::vector<const TypeAdapter*>& with(Capability c) const { return a_ ? a_->by_cap[static_cast<size_t>(c)] : empty(); }

private:
    static const std::vector<const TypeAdapter*>& empty() {
        static const std::vector<const TypeAdapter*> none;
        return none;
    }
    const TypeDescriptor* t_;
    const AdaptedType* a_;
};

/// A member type is excluded when it, its element type or its key type (recursively) matches.
bool type_excluded(const TypeDescriptor* t, const TypeExclusion& excluded, int depth = 0);
/// Some member of the type is excluded.
bool excludes_members(const TypeDescriptor& type, const TypeExclusion& excluded);

std::vector<const TypeAdapter*> applicable_adapters(const std::vector<std::unique_ptr<TypeAdapter>>& all, const TypeDescriptor& type);
/// nullptr when no adapter applies (the descriptor is then used directly).
std::unique_ptr<AdaptedType> build_adapted(TypeRegistry& registry, const TypeDescriptor& type,
                                           const std::vector<std::unique_ptr<TypeAdapter>>& adapters, const TypeExclusion& excluded);

/// First non-empty answer of `f(adapter)` over an adapter list.
template <class F>
auto first_answer(const std::vector<const TypeAdapter*>& list, F f) -> decltype(f(*list.front())) {
    for (const TypeAdapter* a : list)
        if (auto r = f(*a)) return r;
    return std::nullopt;
}

struct Item {
    Value value;
    PathPtr path;
    int depth = -1;  // depth within the most recent recurse stage; -1 outside
};

/// Per-member-path-segment statistics (unknown-member warnings) + monomorphic inline cache.
struct MemberSite {
    std::string name;
    int64_t hits = 0;
    int64_t misses = 0;
    std::vector<const TypeDescriptor*> miss_types;
    std::vector<std::string> row_columns;  // columns of a multi-column row that lacked the member
    bool bare = false;                     // first segment of a relative path (no `on`)
    const TypeDescriptor* cached_type = nullptr;
    const MemberDescriptor* cached_member = nullptr;
    explicit MemberSite(std::string n) : name(std::move(n)) {}
};

/// Statistics of one `x is T` test (no-match warning).
struct IsSite {
    std::string type_name;
    int64_t evaluated = 0;
    int64_t matched = 0;
    std::vector<std::string> seen;
};

class Exec {
public:
    Exec(Engine& engine, const Budgets& budgets, const QueryOptions& options, const QueryPlan& plan);
    ~Exec();
    Exec(const Exec&) = delete;
    Exec& operator=(const Exec&) = delete;

    Engine& engine;
    TypeRegistry& types;
    QueryOptions options;
    const QueryPlan& plan;
    int max_visited, max_rows, max_list_items, max_depth, timeout_ms, max_output_bytes, max_mutation_targets;
    bool track_paths = false;

    // evaluation scope
    Value current;
    PathPtr current_path;
    int current_depth = -1;
    int64_t index = 0;
    std::vector<Value> outer;  // enclosing items while evaluating collection-method arguments

    int64_t visited = 0;
    int64_t members_read = 0;
    int getter_errors = 0;
    std::string first_getter_error;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::unique_ptr<MemberSite>> sites;
    std::vector<std::unique_ptr<IsSite>> is_sites;
    /// A compiled pattern, or (regex null) the compile error of an invalid one.
    struct CachedRegex {
        std::shared_ptr<std::regex> re;
        std::string error;
    };
    std::unordered_map<std::string, CachedRegex> regex_cache;
    std::unordered_map<std::string, Value> ref_cache;  // fallback ref() search results

    /// View of a type for this query. Adapter-free engines: one flag check, the descriptor itself.
    TypeView view(const TypeDescriptor* t) {
        if (!has_adapters_) return TypeView(t, nullptr);
        auto it = adapted_.find(t);
        if (it == adapted_.end()) it = adapted_.emplace(t, engine.adapted(t)).first;  // engine cache (locked) on first use
        return TypeView(t, it->second);
    }
    /// Adapters of a value's type implementing a capability (empty when none / adapter-free engine).
    const std::vector<const TypeAdapter*>& adapters_for(const Value& v, Capability c) {
        static const std::vector<const TypeAdapter*> none;
        if (!caps_[static_cast<size_t>(c)] || !v.type()) return none;
        return view_with(v.type(), c);
    }
    const std::vector<const TypeAdapter*>& adapters_for(const TypeDescriptor* t, Capability c) {
        static const std::vector<const TypeAdapter*> none;
        if (!caps_[static_cast<size_t>(c)] || !t) return none;
        return view_with(t, c);
    }
    bool has_capability(Capability c) const { return caps_[static_cast<size_t>(c)]; }
    /// A destroyed object (Engine::liveness says not alive) reads as null everywhere (spec §13). The gate is
    /// one flag: some liveness adapter, or the host is_alive hook.
    bool is_dead(const Value& v) {
        if (!liveness_ || !v.is_object()) return false;
        auto alive = engine.liveness(v, this);
        return alive && !*alive;
    }
    /// Normalize a value entering an expression: destroyed objects become null.
    Value live(Value v) {
        if (liveness_ && is_dead(v)) return Value::null();
        return v;
    }
    bool liveness_enabled() const { return liveness_; }
    /// Objects constructed from object literals during this query (engine-owned; retained by the engine
    /// when the query ends unless it was a dry run).
    std::vector<std::shared_ptr<void>> constructed;
    /// Registered types named `name` deriving from `target` (cached per target).
    std::vector<const TypeDescriptor*> subtypes_named(const TypeDescriptor* target, const std::string& name);
    /// Stable reference string (adapter stable ids, id member, session handles).
    std::string ref_of(const Value& v);
    /// Resolve a reference; fallback-search nodes count as visits; results cached per query.
    Value resolve_ref(const std::string& reference, std::string& error);

    void visit();
    void check_time() const;
    void getter_threw(const std::string& member, const std::string& what);
    /// Adds a warning once per `key` (default: code + message), however often it recurs.
    void warn_once(const std::string& code, const std::string& message, const std::string& key = {});
    double elapsed_ms() const;

private:
    std::chrono::steady_clock::time_point start_;
    std::unordered_set<std::string> warned_;
    bool has_adapters_ = false;
    bool liveness_ = false;
    std::array<bool, kCapabilities> caps_{};
    std::unordered_map<const TypeDescriptor*, const AdaptedType*> adapted_;
    const std::vector<const TypeAdapter*>& view_with(const TypeDescriptor* t, Capability c) {
        TypeView tv = view(t);
        static const std::vector<const TypeAdapter*> none;
        return tv.adapted() ? tv.adapted()->by_cap[static_cast<size_t>(c)] : none;
    }
};

using Eval = std::function<Value(Exec&)>;


// ---------------------------------------------------------------- value semantics (compiler.cpp)

/// Kleene truth: bool → value, null → false, otherwise unknown (nullopt).
std::optional<bool> truth(const Value& v);
Value tri(std::optional<bool> b);
/// Equality / ordering (spec §4.1) with adapter hooks of the running query (spec §13).
std::optional<bool> rql_equal(Exec& x, const Value& a, const Value& b);
std::optional<int> rql_compare(Exec& x, const Value& a, const Value& b);
bool try_number(const Value& v, double& d, bool& single);
inline bool try_number(const Value& v, double& d) { bool s; return try_number(v, d, s); }
Value reduce(Exec& x, const std::vector<Value>& values, const std::string& kind);
/// Value equality for distinct / group-by (structs member-wise over their implicit members).
bool distinct_equal(Exec& x, const Value& a, const Value& b);
size_t distinct_hash(Exec& x, const Value& v);
/// Elements of a list (values) or a map (entries); false when v is not a collection.
bool elements(const Value& v, std::vector<Value>& out);
size_t collection_size(const Value& v);
std::vector<std::string> suggest_closest(const std::string& name, const std::vector<std::string>& candidates, size_t max);
/// Optimal string alignment distance (Levenshtein plus adjacent transposition, each cost 1).
size_t osa_distance(const std::string& a, const std::string& b);
/// "members: a b c … (N more)" with at most 12 names (spec §10).
std::string member_list_hint(const std::vector<std::string>& names);
/// Registered extension function named by a call (`ns.fn(...)` parsed as a method on a dotted path, or a
/// dotted function name), or nullptr.
const FunctionHandler* extension_function(Exec& x, const Expr& c, std::string& name);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
Value member_of(Exec& x, const Value& v, MemberSite& site);

class Compiler {
public:
    explicit Compiler(Exec& x) : x_(x) {}
    Eval compile(const Expr& e);

private:
    Exec& x_;
    Eval compile_path(const Expr& p);
    Eval compile_meta(const std::string& name, Eval prev, bool of_current_item);
    Eval compile_meta_core(const std::string& name, Eval prev, bool of_current_item);
    Eval compile_op(const Expr& o);
    Eval compile_op_args(const std::string& op, const ExprPtr& a, const ExprPtr& b);
    Eval compile_call(const Expr& c);
    Eval compile_function(const Expr& c);
    Eval compile_method(const Expr& c);
    Eval compile_extension(const std::string& name, const FunctionHandler& fn, const std::vector<ExprPtr>& args);
    void arity(const Expr& c, size_t min, size_t max);
};

// ---------------------------------------------------------------- encoder (encoder.cpp)

class Encoder {
public:
    explicit Encoder(Exec& x) : x_(x) {}
    Json encode(const Value& v, int depth = 0);
    Json summary(const Value& obj);
    QueryResult rows(const std::vector<std::string>& columns, const std::vector<std::vector<Value>>& rows);
    QueryResult items(const std::vector<Value>& items);
    static Json number(double d, bool single);

private:
    Exec& x_;
    Json inline_object(const Value& obj, int depth);
};

// ---------------------------------------------------------------- streams (pipeline.cpp)

class Stream {
public:
    virtual ~Stream() = default;
    virtual bool next(Item& out) = 0;
};
using StreamPtr = std::unique_ptr<Stream>;

QueryResult run_pipeline(Exec& x);

// ---------------------------------------------------------------- mutations (mutator.cpp)

QueryResult run_set(Exec& x, Compiler& c, Encoder& enc, Stream& stream, const Stage& s);
QueryResult run_call(Exec& x, Compiler& c, Encoder& enc, Stream& stream, const Stage& s);
/// Coerce a value to a member/parameter type (spec §8); nullopt + why on failure.
std::optional<Value> coerce(Exec& x, const Value& v, const TypeDescriptor* target, bool nullable, std::string& why);

// ---------------------------------------------------------------- describe (encoder.cpp)
QueryResult run_describe(Exec& x, Stream& stream);

}  // namespace detail
}  // namespace rql
