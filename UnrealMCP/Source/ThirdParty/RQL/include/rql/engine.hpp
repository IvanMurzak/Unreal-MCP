// Host-facing RQL runtime over live C++ objects (mirrors dotnet/Rql.Execution/RqlEngine.cs).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rql/adapters.hpp"
#include "rql/codec.hpp"
#include "rql/plan.hpp"
#include "rql/reflection.hpp"
#include "rql/result.hpp"
#include "rql/value.hpp"

namespace rql {

namespace detail {
class Exec;
struct AdaptedType;
class TypeView;
}

struct SourceRegistration {
    std::string name;
    std::function<Value()> getter;
    std::string description;
};

/// Per-item evaluator handed to extension stages: evaluates an argument expression with $ = item.
using ArgEvaluator = std::function<Value(const Value& item)>;
/// Extension stage: receives the (materialized) input values, returns the output values.
using StageHandler = std::function<std::vector<Value>(const std::vector<Value>& input, const std::vector<ArgEvaluator>& args,
                                                      const std::map<std::string, ArgEvaluator>& named)>;
using FunctionHandler = std::function<Value(const std::vector<Value>& args)>;

/// One change made (or about to be made) by `set` or `call`, as seen by host write hooks (spec §13).
struct MutationRecord {
    Value item;    // the mutation target (a stream item)
    Value owner;   // the object whose state changes: the last reference object on the written path
    std::string member;  // the written path (set) or the method signature (call)
    Value old_value;     // set only
    Value new_value;     // set only: planned value (before_write), read-back value (committed)
    const MethodDescriptor* method = nullptr;  // call only
    std::vector<Value> args;                   // call only: the coerced arguments (defaults filled in)
};
using MutationHook = std::function<void(const std::vector<MutationRecord>& records)>;
/// Batch validation: a refusal reason, or "" to accept.
using BatchValidator = std::function<std::string(const std::vector<MutationRecord>& records)>;

struct MutationPolicy {
    /// Host-level switch. Requests must additionally set QueryOptions::allow_mutation.
    bool enabled = false;
    /// Optional extra allow-check per (declaring object type, member) → permission-denied when false.
    std::function<bool(const TypeDescriptor& owner, const MemberDescriptor& member)> can_write;
    int max_targets = 20;
    /// Host ceiling no request's maxMutationTargets can exceed.
    int hard_max_targets = 1000;
    /// Batch validation (spec §8): every planned change of a set / call stage at once (targets, members or methods,
    /// coerced values or arguments), after each target passed its own checks and before anything is written or
    /// called — on dry runs too. A non-empty reason (or a throw) refuses the whole stage → invalid-value, nothing changed.
    BatchValidator validate_batch;
    /// Replaces the mutation-limit hint (spec §10): (stage "set"/"call", target limit) → hint; "" keeps the built-in one.
    std::function<std::string(const std::string& op, int limit)> limit_hint;
    /// Host text coercion (spec §8): converts a string written to a struct / object type that no type adapter coerces
    /// (e.g. Unreal's ExportText form "(Minimum=(X=0.5,Y=0),Maximum=(X=0.5,Y=0))"). Return the value; nullopt when the
    /// type is not handled (the built-in rules then report the failure); set `reason` (or throw) to refuse.
    std::function<std::optional<Value>(const TypeDescriptor& type, const std::string& text, std::string& reason)> coerce_text;
    /// After planning, before any write (not on dry-run): e.g. record undo for the owners. Throwing (any
    /// exception) vetoes the whole mutation → permission-denied, nothing changed.
    MutationHook before_write;
    /// After every write succeeded (not on dry-run or rollback; for a failing call, the calls that completed):
    /// e.g. mark the owners dirty. Records carry the value read back after the write (what the setter stored).
    /// Throwing → hook-threw warning; the writes stay applied.
    MutationHook committed;
};

struct EngineDefaults {
    int max_visited = 1000000;
    int max_rows = 50;
    int max_list_items = 20;
    int max_depth = 16;
    int timeout_ms = 2000;
    int max_output_bytes = 65536;
    int default_recurse_depth = 8;
    int hard_max_visited = 5000000;
    int hard_max_rows = 10000;
    int hard_timeout_ms = 30000;
    /// Whether the host's callers can pass request budgets (budgets.*). Hosts whose tools take no budgets argument
    /// set it to false, so error hints never advise raising a budget the caller cannot reach (spec §10).
    bool budgets_adjustable = true;
};

class Engine {
public:
    explicit Engine(TypeRegistry& types);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    TypeRegistry& types() const { return *types_; }
    EngineDefaults& defaults() { return defaults_; }
    const EngineDefaults& defaults() const { return defaults_; }
    MutationPolicy& mutations() { return mutations_; }
    const MutationPolicy& mutations() const { return mutations_; }
    std::string runtime_name = "cpp";

    /// Liveness check for explicitly destroyed objects (Unreal/Unity style). Unset = trust pointers
    /// resolved by search; session handles ("Type@n") are then re-validated by reachability.
    std::function<bool(const Value& object)> is_alive;
    /// Resolve "Type#id" (type short name, id text). Return missing/null when not found.
    /// Unset: bounded search over owning members of all sources.
    std::function<Value(const std::string& type, const std::string& id)> resolve_stable_ref;
    /// When false, $path is tracked only for plans that reference $path.
    bool always_track_paths = false;
    /// How callers find an object by name on this host, e.g. unreal.find("Floor"); ref() hints name it (spec §7).
    /// Empty: the hints suggest a generic query.
    std::string find_hint;

    // Every Engine member function is noexcept: exceptions used internally never cross this API.
    // Registration with an invalid name is ignored and reported through last_error().

    Engine& add_source(const std::string& name, std::function<Value()> getter, std::string description = {}) noexcept;
    /// Convenience: a registered object exposed as a source.
    template <class T>
    Engine& add_object_source(const std::string& name, T* object, std::string description = {}) noexcept {
        TypeRegistry* r = types_;
        return add_source(name, [r, object]() { return r->wrap(object); }, std::move(description));
    }
    /// Namespaced extension function "ns.fn".
    Engine& add_function(const std::string& name, FunctionHandler fn) noexcept;
    /// Namespaced extension stage "ns.stage".
    Engine& add_stage(const std::string& name, StageHandler handler) noexcept;
    /// Register a type adapter (spec §13). Register adapters before running queries (not concurrently
    /// with them). An adapter without a target or interface name is rejected via last_error().
    Engine& add_adapter(TypeAdapter adapter) noexcept;
    /// Ordered adapters applying to a type, most specific first: "name (target T, rank r, priority p)".
    std::vector<std::string> explain(const TypeDescriptor& type) const noexcept;
    bool has_adapters() const noexcept { return !adapters_.empty(); }
    /// Never expose members whose type (or element / key type) matches (spec §13 Exclusion). Configure
    /// before queries run; several predicates combine with "or".
    Engine& exclude_types(TypeExclusion excluded) noexcept;

    /// Last registration error (empty when the last registration succeeded).
    const std::string& last_error() const noexcept { return last_error_; }

    const std::vector<SourceRegistration>& sources() const noexcept { return sources_; }
    const SourceRegistration* find_source(const std::string& name) const noexcept;  // exact, then unique case-insensitive
    const std::map<std::string, FunctionHandler>& functions() const noexcept { return functions_; }
    const std::map<std::string, StageHandler>& stages() const noexcept { return stages_; }

    /// Execute a plan. Any failure (including host callbacks throwing) → ok=false with a diagnostic.
    QueryResult execute(const QueryPlan& plan, const Budgets& budgets = {}, const QueryOptions& options = {}) noexcept;
    /// Decode + execute a plan JSON text; codec errors become an error result.
    QueryResult execute_json(const std::string& plan_json, const Budgets& budgets = {}, const QueryOptions& options = {}) noexcept;
    /// Transport envelope {"rql","plan","budgets","options"} as JSON text or value (text requests → unsupported).
    QueryResult execute_request(const Json& request) noexcept;
    QueryResult execute_request(const std::string& request_json) noexcept;

    /// Stable reference string: "Type#id" (id member) or session handle "Type@n".
    std::string ref_of(const Value& v) noexcept;
    /// Resolve a reference string (copied display forms tolerated). On failure returns missing and sets error.
    Value resolve_ref(const std::string& reference, std::string& error) noexcept;
    void clear_session_handles() noexcept;

    /// Objects this engine built from object literals (spec §4.5) and keeps alive. The engine never frees
    /// them automatically: any raw pointer in the graph or in host state (e.g. an undo stack holding hook
    /// records) may still reach them. Types with a host factory (ClassBuilder::factory) are host-owned and
    /// never retained.
    size_t retained_literal_objects() noexcept;
    /// Free the retained literal-built objects. Call it ONLY when no graph or host pointer can still reach
    /// them (e.g. after the scene they were linked into was torn down); otherwise pointers dangle.
    void release_literal_objects() noexcept;

    /// Capability manifest (spec/capability-manifest.schema.json). Null Json on internal failure.
    Json capabilities() noexcept;
    /// Type descriptor JSON (capability-manifest typeDescriptor).
    Json describe_type(const TypeDescriptor& t) const noexcept;

private:
    friend class detail::Exec;

    struct HandleKey {
        const void* ptr;
        const void* type;
        bool operator==(const HandleKey& o) const { return ptr == o.ptr && type == o.type; }
    };
    struct HandleHash {
        size_t operator()(const HandleKey& k) const {
            return std::hash<const void*>()(k.ptr) * 31u ^ std::hash<const void*>()(k.type);
        }
    };
    struct HandleTarget {
        Value value;
    };

    TypeRegistry* types_;
    EngineDefaults defaults_;
    MutationPolicy mutations_;
    std::vector<SourceRegistration> sources_;
    std::map<std::string, FunctionHandler> functions_;
    std::map<std::string, StageHandler> stages_;
    std::mutex handle_mutex_;
    std::unordered_map<HandleKey, int64_t, HandleHash> handles_;
    std::unordered_map<int64_t, HandleTarget> handle_targets_;
    int64_t next_handle_ = 0;
    std::string last_error_;
    std::vector<std::unique_ptr<TypeAdapter>> adapters_;
    std::array<bool, 6> caps_{};  // some adapter implements the capability (detail::Capability order)
    TypeExclusion excluded_;
    // Objects the engine constructed from object literals (spec §4.5) for non-dry-run queries. They may be
    // linked from anywhere (raw pointers, host undo stacks holding hook records), so the engine never frees
    // them automatically: they live until release_literal_objects() or the engine's destruction.
    std::mutex retained_mutex_;
    std::vector<std::shared_ptr<void>> retained_;
    void retain(std::vector<std::shared_ptr<void>>& objects) noexcept;
    // Object-literal "$type" lookups (spec §4.5): per target type, name → registered subtypes of its kind.
    mutable std::mutex subtypes_mutex_;
    mutable std::unordered_map<const TypeDescriptor*, std::unordered_map<std::string, std::vector<const TypeDescriptor*>>> subtypes_;
    mutable size_t subtypes_registered_ = 0;
    std::vector<const TypeDescriptor*> subtypes_named(const TypeDescriptor* target, const std::string& name) const;
    /// Some adapter, or a type exclusion that actually removes a member of a registered type: only then
    /// may types differ from their descriptors (adapter-free engines read descriptors directly).
    bool adapts() const;
    // Cached answer of "does the exclusion remove a member of some registered type": a lock-free check on
    // the hot path, recomputed (under the mutex) only when types were registered since the last scan.
    mutable std::mutex exclusion_mutex_;
    mutable std::atomic<size_t> exclusion_scanned_{0};  // registered-type count of the last scan (0 = none)
    mutable std::atomic<bool> exclusion_hits_{false};

    mutable std::mutex adapted_mutex_;
    mutable std::unordered_map<const TypeDescriptor*, std::unique_ptr<detail::AdaptedType>> adapted_;

    // Internals (may throw; only called inside execution or from noexcept wrappers). `x` is the running
    // query when there is one: its per-query cache is used and visits are counted.
    /// Adaptation of a type (cached, locked); nullptr when no adapter applies. Throws std::logic_error
    /// for adapter configuration bugs (the running query then fails with internal-error).
    const detail::AdaptedType* adapted(const TypeDescriptor* type) const;
    detail::TypeView view_of(const TypeDescriptor* type, detail::Exec* x) const;
    /// THE liveness definition (spec §13): the first adapter verdict, else the host is_alive hook;
    /// nullopt = undecided (treated as alive).
    std::optional<bool> liveness(const Value& object, detail::Exec* x);
    bool alive(const Value& object, detail::Exec* x) { return liveness(object, x).value_or(true); }
    /// "#id" part of a stable reference: adapter stable_id first, then the id member; nullopt = none.
    std::optional<std::string> stable_id_of(const Value& object, detail::Exec* x);
    Value resolve_ref(const std::string& reference, std::string& error, detail::Exec* x);
    /// `fallback`: the trimmed display form, tried when the exact text does not resolve (stable refs of the
    /// same type are matched in ONE source-search pass, exact id preferred).
    Value resolve_ref_core(const std::string& reference, std::string& error, detail::Exec* x, const std::string* fallback);
    Value check_resolved(const Value& found, const std::string& reference, const std::string& type, std::string& error, detail::Exec* x);
    /// Source search for a stable id (`alt_id`: a second id accepted when the first is absent); cached per query.
    Value find_stable(const std::string& type, const std::string& id, const std::string* alt_id, detail::Exec* x);

    int64_t handle_of(const Value& v);
    Value search_sources(const std::string& type, const std::string& id, const std::string* alt_id, const Value* identity, detail::Exec* x);
    Json describe_type_impl(const TypeDescriptor& t) const;
    Json capabilities_impl();
    std::string ref_of_impl(const Value& v, detail::Exec* x);
};

}  // namespace rql
