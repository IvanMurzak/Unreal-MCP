#include "rql/engine.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "exec.hpp"

namespace rql {

namespace {

bool ident_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

void validate_name(const std::string& name, bool require_namespace) {
    if (name.empty()) throw std::invalid_argument("name required");
    size_t start = 0;
    for (;;) {
        size_t dot = name.find('.', start);
        std::string part = name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty() || !ident_start(part[0])) throw std::invalid_argument("invalid name '" + name + "'");
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (require_namespace && name.find('.') == std::string::npos)
        throw std::invalid_argument("extension names must be namespaced (ns.name): '" + name + "'");
}

const char* kCoreStageNames[] = {"where", "select", "sort", "first", "skip", "count", "describe", "get", "sum",
                                 "min", "max", "avg", "group-by", "distinct", "recurse", "set", "call"};
// Keep in sync with kCoreFunctions / kCoreMethods in compiler.cpp (the manifest lists what the compiler binds).
const char* kFunctionNames[] = {"has", "is-null", "len", "lower", "upper", "abs", "round", "floor", "ceil", "sqrt", "pow", "min", "max",
                                "coalesce", "if", "ref",
                                "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pi", "deg", "rad",
                                "append", "insert", "remove-at", "without", "range",
                                "replace", "replace-regex", "substring", "split", "trim", "pad",
                                "contains", "starts-with", "ends-with", "matches",
                                "length", "magnitude", "normalize", "distance", "dot", "lerp"};
const char* kMethodNames[] = {"any", "all", "count", "sum", "min", "max", "avg", "first", "keys", "values",
                              "contains", "starts-with", "ends-with", "matches", "lower", "upper", "len",
                              "replace", "replace-regex", "substring", "split", "trim", "pad",
                              "length", "magnitude", "normalize", "distance", "dot", "lerp"};

}  // namespace

Engine& Engine::add_source(const std::string& name, std::function<Value()> getter, std::string description,
                           const TypeDescriptor* declared_type) noexcept {
    try {
        validate_name(name, false);
        last_error_.clear();
        for (auto& s : sources_) {
            if (s.name == name) {
                s.getter = std::move(getter);
                s.description = std::move(description);
                s.declared_type = declared_type;
                std::lock_guard<std::mutex> lock(source_types_mutex_);
                source_types_.erase(name);
                return *this;
            }
        }
        sources_.push_back({name, std::move(getter), std::move(description), declared_type});
        std::lock_guard<std::mutex> lock(source_types_mutex_);
        source_types_.erase(name);
    } catch (const std::exception& ex) {
        last_error_ = ex.what();
    } catch (...) {
        last_error_ = "add_source failed";
    }
    return *this;
}

Engine& Engine::add_function(const std::string& name, FunctionHandler fn) noexcept {
    try {
        validate_name(name, true);
        last_error_.clear();
        functions_[name] = std::move(fn);
    } catch (const std::exception& ex) {
        last_error_ = ex.what();
    } catch (...) {
        last_error_ = "add_function failed";
    }
    return *this;
}

Engine& Engine::add_stage(const std::string& name, StageHandler handler) noexcept {
    try {
        validate_name(name, true);
        last_error_.clear();
        stages_[name] = std::move(handler);
    } catch (const std::exception& ex) {
        last_error_ = ex.what();
    } catch (...) {
        last_error_ = "add_stage failed";
    }
    return *this;
}

const SourceRegistration* Engine::find_source(const std::string& name) const noexcept {
    for (auto& s : sources_)
        if (s.name == name) return &s;
    const SourceRegistration* hit = nullptr;
    for (auto& s : sources_) {
        if (iequals(s.name, name)) {
            if (hit) return nullptr;
            hit = &s;
        }
    }
    return hit;
}

// ------------------------------------------------------------------ adapters

Engine::Engine(TypeRegistry& types) : types_(&types) {}
Engine::~Engine() = default;

Engine& Engine::add_adapter(TypeAdapter adapter) noexcept {
    try {
        if (!adapter.target && adapter.interface_name.empty()) {
            last_error_ = "type adapter '" + adapter.name + "' needs a target type or an interface name";
            return *this;
        }
        if (adapter.name.empty()) adapter.name = adapter.target ? adapter.target->name + "Adapter" : adapter.interface_name + "Adapter";
        last_error_.clear();
        const bool caps[] = {bool(adapter.encode),  bool(adapter.coerce),    bool(adapter.equals),   bool(adapter.compare),
                             bool(adapter.stable_id), bool(adapter.is_alive), bool(adapter.components),
                             bool(adapter.literal_kind), bool(adapter.create_for_literal)};
        static_assert(sizeof(caps) / sizeof(caps[0]) == std::tuple_size<decltype(caps_)>::value, "one flag per adapter capability");
        static_assert(std::tuple_size<decltype(caps_)>::value == detail::kCapabilities, "caps_ covers every capability");
        for (size_t i = 0; i < caps_.size(); ++i) caps_[i] = caps_[i] || caps[i];
        adapters_.push_back(std::make_unique<TypeAdapter>(std::move(adapter)));
        // Registering adapters while queries run is not supported, so cached adaptations can just go.
        std::lock_guard<std::mutex> lock(adapted_mutex_);
        adapted_.clear();
    } catch (const std::exception& ex) {
        last_error_ = ex.what();
    } catch (...) {
        last_error_ = "add_adapter failed";
    }
    return *this;
}

std::vector<std::string> Engine::explain(const TypeDescriptor& type) const noexcept {
    std::vector<std::string> out;
    try {
        std::vector<const TypeAdapter*> ordered;
        try {
            if (const detail::AdaptedType* a = adapted(&type)) ordered = a->adapters;
        } catch (const std::logic_error&) {
            ordered = detail::applicable_adapters(adapters_, type);  // member configuration bug: still explain the order
        }
        for (const TypeAdapter* a : ordered) {
            std::string target = a->target ? a->target->name : "interface " + a->interface_name;
            out.push_back(a->name + " (target " + target + ", rank " + std::to_string(adapter_rank(*a, type)) + ", priority " +
                          std::to_string(a->priority) + ")");
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

Engine& Engine::exclude_types(TypeExclusion excluded) noexcept {
    try {
        if (!excluded) {
            last_error_ = "exclude_types needs a predicate";
            return *this;
        }
        last_error_.clear();
        if (excluded_) {
            TypeExclusion prev = std::move(excluded_);
            excluded_ = [prev, excluded](const TypeDescriptor& t) { return prev(t) || excluded(t); };
        } else {
            excluded_ = std::move(excluded);
        }
        {
            std::lock_guard<std::mutex> lock(exclusion_mutex_);
            exclusion_scanned_.store(static_cast<size_t>(-1), std::memory_order_release);  // force a rescan
        }
        std::lock_guard<std::mutex> lock(adapted_mutex_);
        adapted_.clear();
    } catch (...) {
        last_error_ = "exclude_types failed";
    }
    return *this;
}

bool Engine::adapts() const {
    if (!adapters_.empty()) return true;
    if (!excluded_) return false;
    const size_t count = types_->registered_count();
    if (count == exclusion_scanned_.load(std::memory_order_acquire)) return exclusion_hits_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(exclusion_mutex_);  // types were registered since the last scan
    if (count != exclusion_scanned_.load(std::memory_order_relaxed)) {
        const std::vector<const TypeDescriptor*> registered = types_->registered();
        exclusion_hits_.store(std::any_of(registered.begin(), registered.end(),
                                          [&](const TypeDescriptor* t) { return detail::excludes_members(*t, excluded_); }),
                              std::memory_order_release);
        exclusion_scanned_.store(count, std::memory_order_release);
    }
    return exclusion_hits_.load(std::memory_order_acquire);
}

std::vector<const TypeDescriptor*> Engine::subtypes_named(const TypeDescriptor* target, const std::string& name) const {
    std::lock_guard<std::mutex> lock(subtypes_mutex_);
    if (types_->registered_count() != subtypes_registered_) {  // types were registered since: rebuild lazily
        subtypes_.clear();
        subtypes_registered_ = types_->registered_count();
    }
    auto it = subtypes_.find(target);
    if (it == subtypes_.end()) {
        std::unordered_map<std::string, std::vector<const TypeDescriptor*>> by_name;
        const std::vector<const TypeDescriptor*> registered = types_->registered();
        for (const TypeDescriptor* t : registered)
            if (t->kind == target->kind && t->derives_from(target)) by_name[t->name].push_back(t);
        it = subtypes_.emplace(target, std::move(by_name)).first;
    }
    auto hit = it->second.find(name);
    return hit == it->second.end() ? std::vector<const TypeDescriptor*>{} : hit->second;
}

size_t Engine::retained_literal_objects() noexcept {
    std::lock_guard<std::mutex> lock(retained_mutex_);
    return retained_.size();
}

void Engine::release_literal_objects() noexcept {
    std::vector<std::shared_ptr<void>> released;
    {
        std::lock_guard<std::mutex> lock(retained_mutex_);
        released.swap(retained_);
    }
}  // freed here, outside the lock

void Engine::retain(std::vector<std::shared_ptr<void>>& objects) noexcept {
    if (objects.empty()) return;
    try {
        std::lock_guard<std::mutex> lock(retained_mutex_);
        retained_.reserve(retained_.size() + objects.size());
        for (auto& o : objects) retained_.push_back(std::move(o));
        objects.clear();
    } catch (...) {
        // Out of memory: keep the objects alive forever rather than risk dangling links.
        auto* leak = new (std::nothrow) std::vector<std::shared_ptr<void>>(std::move(objects));
        (void)leak;
    }
}

const detail::AdaptedType* Engine::adapted(const TypeDescriptor* type) const {
    std::lock_guard<std::mutex> lock(adapted_mutex_);
    auto it = adapted_.find(type);
    if (it != adapted_.end()) return it->second.get();
    auto a = detail::build_adapted(*types_, *type, adapters_, excluded_);  // throws on adapter configuration bugs (not cached)
    return adapted_.emplace(type, std::move(a)).first->second.get();
}

detail::TypeView Engine::view_of(const TypeDescriptor* type, detail::Exec* x) const {
    if (!adapts()) return detail::TypeView(type, nullptr);
    if (x) return x->view(type);
    return detail::TypeView(type, adapted(type));
}

std::optional<bool> Engine::liveness(const Value& object, detail::Exec* x) {
    if (caps_[static_cast<size_t>(detail::Capability::IsAlive)] && object.type()) {
        const detail::TypeView tv = view_of(object.type(), x);
        auto verdict = detail::first_answer(tv.with(detail::Capability::IsAlive), [&](const TypeAdapter& a) { return a.is_alive(object); });
        if (verdict) return verdict;
    }
    if (is_alive) return is_alive(object);
    return std::nullopt;
}

std::optional<std::string> Engine::stable_id_of(const Value& object, detail::Exec* x) {
    detail::TypeView tv = view_of(object.type(), x);
    if (caps_[static_cast<size_t>(detail::Capability::StableId)]) {
        auto sid = detail::first_answer(tv.with(detail::Capability::StableId), [&](const TypeAdapter& a) { return a.stable_id(object); });
        if (sid) return sid;
    }
    const MemberDescriptor* idm = tv.id();
    if (!idm) return std::nullopt;
    Value id;
    try {
        id = idm->getter(object.ptr());
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (id.is_null() || id.is_absent()) return std::nullopt;
    return to_text(id);
}

namespace detail {
std::string Exec::ref_of(const Value& v) { return engine.ref_of_impl(v, this); }
Value Exec::resolve_ref(const std::string& reference, std::string& error) { return engine.resolve_ref(reference, error, this); }
std::vector<const TypeDescriptor*> Exec::subtypes_named(const TypeDescriptor* target, const std::string& name) {
    return engine.subtypes_named(target, name);
}
Exec::~Exec() {
    // Literal-built objects of a non-dry-run query may now be linked from the graph or host state
    // (also after a failed rollback): the engine retains them (spec §4.5 ownership).
    if (!options.dry_run) engine.retain(constructed);
}
}  // namespace detail

// ------------------------------------------------------------------ execution

namespace {
QueryResult internal_error(const char* what) noexcept {
    try {
        return QueryResult::fail({Diagnostic::error("internal-error", std::string("internal error: ") + what,
                                                    "this is an RQL runtime bug or a throwing host callback; please report the query")});
    } catch (...) {
        return QueryResult();  // ok=false, no diagnostics (allocation failure)
    }
}
}  // namespace

QueryResult Engine::execute(const QueryPlan& plan, const Budgets& budgets, const QueryOptions& options) noexcept {
    try {
        if (!plan.source) return QueryResult::fail({Diagnostic::error("invalid-plan", "plan has no source")});
        detail::Exec x(*this, budgets, options, plan);
        QueryResult r;
        try {
            r = detail::run_pipeline(x);
        } catch (const std::exception& ex) {
            // Last line of defence: untrusted queries must never crash the host with a raw exception.
            r = internal_error(ex.what());
        } catch (...) {
            r = internal_error("unknown exception");
        }
        r.stats.visited = x.visited;
        r.stats.members_read = x.members_read;
        r.stats.elapsed_ms = x.elapsed_ms();
        return r;
    } catch (const std::exception& ex) {
        return internal_error(ex.what());
    } catch (...) {
        return internal_error("unknown exception");
    }
}

QueryResult Engine::execute_json(const std::string& plan_json, const Budgets& budgets, const QueryOptions& options) noexcept {
    QueryPlan plan;
    Diagnostic err;
    if (!try_decode_plan(std::string_view(plan_json), plan, err)) {
        try {
            return QueryResult::fail({err});
        } catch (...) {
            return QueryResult();
        }
    }
    return execute(plan, budgets, options);
}

QueryResult Engine::execute_request(const std::string& request_json) noexcept {
    Json req;
    std::string error;
    if (!Json::try_parse(request_json, req, error, 256)) {
        try {
            return QueryResult::fail({Diagnostic::error("invalid-plan", "request is not valid JSON: " + error)});
        } catch (...) {
            return QueryResult();
        }
    }
    return execute_request(req);
}

QueryResult Engine::execute_request(const Json& req) noexcept {
    try {
        if (!req.is_object()) throw RqlException("invalid-plan", "request must be an object");
        for (size_t i = 0; i < req.size(); ++i) {
            const std::string& k = req.key(i);
            if (k != "rql" && k != "plan" && k != "text" && k != "budgets" && k != "options")
                throw RqlException("invalid-plan", "unexpected property '" + k + "' in request");
        }
        if (const Json* v = req.find("rql")) {
            if (!v->is_string() || v->as_string() != kRqlVersion)
                throw RqlException("version-unsupported", "request version is not supported (supported: " + std::string(kRqlVersion) + ")");
        }
        const Json* plan = req.find("plan");
        const Json* text = req.find("text");
        if ((plan != nullptr) == (text != nullptr)) throw RqlException("invalid-plan", "request needs exactly one of 'plan' or 'text'");
        if (text)
            throw RqlException("unsupported", "the C++ runtime executes QueryPlans only; parse RQL text with the .NET/TypeScript parser sidecar");
        Budgets b;
        QueryOptions o;
        if (const Json* bj = req.find("budgets")) b = Budgets::from_json(*bj);
        if (const Json* oj = req.find("options")) o = QueryOptions::from_json(*oj);
        QueryPlan p = decode_plan(*plan);
        return execute(p, b, o);
    } catch (const RqlException& ex) {
        try {
            return QueryResult::fail({Diagnostic::from(ex)});
        } catch (...) {
            return QueryResult();
        }
    } catch (const std::exception& ex) {
        return internal_error(ex.what());
    } catch (...) {
        return internal_error("unknown exception");
    }
}

// ------------------------------------------------------------------ references

int64_t Engine::handle_of(const Value& v) {
    std::lock_guard<std::mutex> lock(handle_mutex_);
    const void *a, *b;
    if (!v.identity(a, b)) {
        int64_t id = ++next_handle_;
        return id;
    }
    HandleKey key{a, b};
    auto it = handles_.find(key);
    if (it != handles_.end()) return it->second;
    int64_t id = ++next_handle_;
    handles_.emplace(key, id);
    // Weak semantics: keep only the raw identity (no keep-alive), re-validated on resolution.
    handle_targets_.emplace(id, HandleTarget{v.is_object() ? Value::object(v.type(), v.ptr()) : Value::missing()});
    return id;
}

void Engine::clear_session_handles() noexcept {
    std::lock_guard<std::mutex> lock(handle_mutex_);
    handles_.clear();
    handle_targets_.clear();
}

std::string Engine::ref_of(const Value& v) noexcept {
    try {
        return ref_of_impl(v, nullptr);
    } catch (...) {
        return "?";
    }
}

std::string Engine::ref_of_impl(const Value& v, detail::Exec* x) {
    if (v.is_object()) {
        const TypeDescriptor* t = v.type();
        if (auto id = stable_id_of(v, x)) return t->name + "#" + *id;
        return t->name + "@" + std::to_string(handle_of(v));
    }
    return type_name_of(v) + "@" + std::to_string(handle_of(v));
}

Value Engine::search_sources(const std::string& type, const std::string& id, const std::string* alt_id, const Value* identity,
                             detail::Exec* x) {
    Value alt_hit;  // first object matching alt_id (returned only when no object matches id)
    struct IdKey {
        const void* a;
        const void* b;
        bool operator==(const IdKey& o) const { return a == o.a && b == o.b; }
    };
    struct IdHash {
        size_t operator()(const IdKey& k) const { return std::hash<const void*>()(k.a) * 31u ^ std::hash<const void*>()(k.b); }
    };
    const void *ta = nullptr, *tb = nullptr;
    if (identity) identity->identity(ta, tb);
    std::unordered_set<IdKey, IdHash> visited;
    std::vector<std::pair<Value, int>> stack;
    for (auto& s : sources_) {
        Value v;
        try {
            v = s.getter();
        } catch (const std::exception&) {
            continue;
        }
        if (!v.is_null() && !v.is_absent()) stack.emplace_back(v, 0);
    }
    int budget = defaults_.max_visited;
    while (!stack.empty() && budget-- > 0) {
        if (x) x->visit();
        auto [o, depth] = stack.back();
        stack.pop_back();
        const void *a, *b;
        if (!o.identity(a, b) || !visited.insert(IdKey{a, b}).second) continue;
        if (o.is_list() || o.is_map()) {
            std::vector<Value> els;
            detail::elements(o, els);
            for (auto& e : els) {
                Value ev = e.is_entry() ? e.entry().value : e;
                if (ev.is_object() || ev.is_list() || ev.is_map()) stack.emplace_back(ev, depth);
            }
            continue;
        }
        if (!o.is_object()) continue;
        const TypeDescriptor* t = o.type();
        if (identity) {
            if (a == ta && b == tb) return o;
        } else if (t->is_a(type)) {
            auto sid = stable_id_of(o, x);
            if (sid && *sid == id) return o;
            if (sid && alt_id && alt_hit.is_missing() && *sid == *alt_id) alt_hit = o;
        }
        if (depth >= 32) continue;
        const detail::TypeView tv = view_of(t, x);
        for (auto& m : tv.implicit_members()) {
            if (!m.owning || !m.type) continue;
            if (m.type->kind == TypeKind::Enum || m.type->kind == TypeKind::Struct) continue;
            if (m.type->kind == TypeKind::Scalar && m.type->scalar != ScalarKind::Any) continue;
            Value v;
            try {
                v = m.getter(o.ptr());
            } catch (const std::exception&) {
                continue;
            }
            if (v.is_object() || v.is_list() || v.is_map()) stack.emplace_back(v, depth + 1);
        }
    }
    return alt_hit;
}

Value Engine::resolve_ref(const std::string& reference, std::string& error) noexcept {
    try {
        return resolve_ref(reference, error, nullptr);
    } catch (const std::exception& ex) {
        try {
            error = ex.what();
        } catch (...) {
        }
        return Value::missing();
    } catch (...) {
        return Value::missing();
    }
}

Value Engine::resolve_ref(const std::string& reference_in, std::string& error, detail::Exec* x) {
    std::string reference = reference_in;
    while (!reference.empty() && (reference.front() == ' ' || reference.front() == '\t')) reference.erase(reference.begin());
    while (!reference.empty() && (reference.back() == ' ' || reference.back() == '\t')) reference.pop_back();
    // Ids may contain spaces / parentheses (asset paths): the exact text is preferred, then copied display
    // forms such as "Enemy#12(Goblin_0012)" or "Enemy#12 Goblin" are tolerated (spec §7).
    size_t cut = reference.find_first_of(" (\t");
    if (cut == std::string::npos || cut == 0) return resolve_ref_core(reference, error, x, nullptr);
    const std::string trimmed = reference.substr(0, cut);
    return resolve_ref_core(reference, error, x, &trimmed);
}

namespace {
bool split_ref(const std::string& reference, std::string& type, std::string& id, char& kind) {
    size_t i = reference.find_first_of("#@");
    if (i == std::string::npos || i == 0 || i == reference.size() - 1) return false;
    type = reference.substr(0, i);
    id = reference.substr(i + 1);
    kind = reference[i];
    return true;
}
}  // namespace

Value Engine::find_stable(const std::string& type, const std::string& id, const std::string* alt_id, detail::Exec* x) {
    if (!x) return search_sources(type, id, alt_id, nullptr, nullptr);
    std::string key = type + "#" + id;
    if (alt_id) key += std::string(1, '\0') + *alt_id;
    auto cached = x->ref_cache.find(key);
    if (cached != x->ref_cache.end()) return cached->second;
    Value found = search_sources(type, id, alt_id, nullptr, x);
    x->ref_cache.emplace(key, found);
    return found;
}

Value Engine::check_resolved(const Value& found, const std::string& reference, const std::string& type, std::string& error,
                             detail::Exec* x) {
    if (!found.is_object() || !found.type()->is_a(type)) {
        error = "reference '" + reference + "' does not resolve to a live object";
        return Value::missing();
    }
    if (!alive(found, x)) {
        error = "reference '" + reference + "' points to a destroyed object";
        return Value::missing();
    }
    return found;
}

Value Engine::resolve_ref_core(const std::string& reference, std::string& error, detail::Exec* x, const std::string* fallback) {
    std::string type, id;
    char kind = 0;
    if (!split_ref(reference, type, id, kind)) {
        if (fallback) return resolve_ref_core(*fallback, error, x, nullptr);
        error = "malformed reference '" + reference + "' (expected Type#id or Type@handle)";
        return Value::missing();
    }
    Value found;
    if (kind == '@') {
        double h = 0;
        Value candidate;
        if (parse_double(id, h) && std::floor(h) == h) {
            std::lock_guard<std::mutex> lock(handle_mutex_);
            auto it = handle_targets_.find(static_cast<int64_t>(h));
            if (it != handle_targets_.end()) candidate = it->second.value;
        }
        if (candidate.is_object()) {
            // The stored pointer may dangle. Trust it only when liveness is DECIDED for this handle's own
            // type (the type stored with the handle; deciding this never dereferences the object): the host
            // hook, or a liveness adapter of that type that answers. Otherwise re-validate by reachability
            // (identity comparison only).
            const std::optional<bool> verdict = liveness(candidate, x);
            if (!verdict) {
                found = search_sources(type, id, nullptr, &candidate, x);
            } else if (*verdict && candidate.type()->is_a(type)) {
                return candidate;  // liveness already decided above
            } else if (!*verdict) {
                error = "reference '" + reference + "' points to a destroyed object";
                return Value::missing();  // an exact match is final: no display-form fallback
            }
        }
    } else if (resolve_stable_ref) {
        found = resolve_stable_ref(type, id);
    } else {
        // One pass over the sources accepts the trimmed id too (when it names the same type); exact wins.
        std::string alt_type, alt_id;
        char alt_kind = 0;
        const bool one_pass = fallback && split_ref(*fallback, alt_type, alt_id, alt_kind) && alt_kind == '#' && alt_type == type;
        found = find_stable(type, id, one_pass ? &alt_id : nullptr, x);
        if (one_pass) {
            const auto sid = found.is_object() ? stable_id_of(found, x) : std::nullopt;
            if (found.is_object() && sid && *sid == alt_id && alt_id != id) return check_resolved(found, *fallback, type, error, x);
            fallback = nullptr;  // the trimmed form was searched in the same pass
        }
    }
    Value resolved = check_resolved(found, reference, type, error, x);
    // An exact match is final (a destroyed one is stale-ref); only an absent exact id tries the display form.
    if (resolved.is_missing() && fallback && !found.is_object()) return resolve_ref_core(*fallback, error, x, nullptr);
    return resolved;
}

// ------------------------------------------------------------------ schema

Json Engine::describe_type(const TypeDescriptor& t) const noexcept {
    try {
        return describe_type_impl(t);
    } catch (...) {
        return Json();
    }
}

Json Engine::describe_type_impl(const TypeDescriptor& t) const {
    const detail::TypeView tv = view_of(&t, nullptr);
    Json o = Json::object();
    o.set("name", t.name);
    if (t.base) o.set("base", t.base->name);
    o.set("kind", t.kind == TypeKind::Enum ? "enum" : t.kind == TypeKind::Struct ? "struct" : "object");
    if (!tv.description().empty()) o.set("description", tv.description());
    if (const Json* literal = tv.literal_schema()) o.set("literal", *literal);
    if (t.kind == TypeKind::Enum) {
        Json values = Json::array();
        for (auto& e : t.enum_values) values.push(Json(e.first));
        o.set("values", std::move(values));
        o.set("members", Json::array());
        return o;
    }
    Json members = Json::array();
    for (auto& m : tv.members()) {
        Json mo = Json::object();
        mo.set("name", m.name);
        std::string tn = m.type ? m.type->friendly_name() : "object";
        if (m.nullable && m.type && m.type->is_value_type() && m.type->scalar != ScalarKind::String) tn += "?";
        mo.set("type", tn);
        if (mutations_.enabled && tv.member_writable(m)) mo.set("writable", true);
        if (m.explicit_only) mo.set("explicitOnly", true);
        if (!m.description.empty()) mo.set("description", m.description);
        members.push(std::move(mo));
    }
    o.set("members", std::move(members));
    // Callable methods (host + adapter), sorted by name then arity: "name(type param, ...) — description".
    std::vector<const MethodDescriptor*> callable;
    for (auto& m : tv.methods())
        if (m.callable) callable.push_back(&m);
    std::stable_sort(callable.begin(), callable.end(), [](const MethodDescriptor* a, const MethodDescriptor* b) {
        return a->name != b->name ? a->name < b->name : a->params.size() < b->params.size();
    });
    Json methods = Json::array();
    for (auto* m : callable)
        methods.push(Json(m->description.empty() ? t.signature(*m) : t.signature(*m) + " \xE2\x80\x94 " + m->description));
    if (methods.size() > 0) o.set("methods", std::move(methods));
    return o;
}

Json Engine::capabilities() noexcept {
    try {
        return capabilities_impl();
    } catch (...) {
        return Json();
    }
}

Json Engine::capabilities_impl() {
    Json o = Json::object();
    o.set("rql", kRqlVersion);
    o.set("runtime", runtime_name);
    Json sources = Json::array();
    for (auto& s : sources_) {
        Json so = Json::object();
        so.set("name", s.name);
        std::string type = "object";
        try {
            Value v = s.getter();
            type = friendly_type_of(v);
        } catch (const std::exception&) {
        }
        so.set("type", type);
        if (!s.description.empty()) so.set("description", s.description);
        sources.push(std::move(so));
    }
    o.set("sources", std::move(sources));
    Json stages = Json::array();
    for (auto* s : kCoreStageNames) stages.push(Json(s));
    for (auto& kv : stages_) stages.push(Json(kv.first));
    o.set("stages", std::move(stages));
    Json functions = Json::array();
    for (auto* f : kFunctionNames) functions.push(Json(f));
    for (auto& kv : functions_) functions.push(Json(kv.first));
    o.set("functions", std::move(functions));
    Json methods = Json::array();
    for (auto* m : kMethodNames) methods.push(Json(m));
    o.set("methods", std::move(methods));
    Json mut = Json::object();
    mut.set("enabled", mutations_.enabled);
    mut.set("maxTargets", mutations_.max_targets);
    mut.set("hardMaxTargets", mutations_.hard_max_targets);
    Json callable = Json::array();
    for (auto* t : types_->registered()) {
        const detail::TypeView tv = view_of(t, nullptr);  // includes adapter methods
        for (auto& m : tv.methods())
            if (m.callable) callable.push(Json(t->name + "." + t->signature(m)));
    }
    mut.set("callable", std::move(callable));
    o.set("mutations", std::move(mut));
    Json limits = Json::object();
    limits.set("maxVisited", defaults_.hard_max_visited);
    limits.set("maxRows", defaults_.hard_max_rows);
    limits.set("maxListItems", defaults_.max_list_items);
    limits.set("maxDepth", defaults_.max_depth);
    limits.set("timeoutMs", defaults_.hard_timeout_ms);
    limits.set("maxOutputBytes", defaults_.max_output_bytes);
    limits.set("maxMutationTargets", mutations_.max_targets);
    o.set("limits", std::move(limits));
    Json types = Json::array();
    for (auto* t : types_->registered()) types.push(describe_type_impl(*t));
    o.set("types", std::move(types));
    return o;
}

}  // namespace rql
