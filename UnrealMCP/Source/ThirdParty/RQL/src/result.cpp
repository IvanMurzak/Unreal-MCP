#include "rql/result.hpp"

#include <cmath>

#include "rql/plan.hpp"

namespace rql {

const char* result_kind_name(ResultKind k) {
    switch (k) {
        case ResultKind::Rows: return "rows";
        case ResultKind::Items: return "items";
        case ResultKind::Scalar: return "scalar";
        default: return "schema";
    }
}

Json Diagnostic::to_json() const {
    Json o = Json::object();
    o.set("severity", severity == Severity::Error ? "error" : "warning");
    o.set("code", code);
    o.set("message", message);
    if (!hint.empty()) o.set("hint", hint);
    if (at) {
        Json a = Json::object();
        a.set("start", at->start);
        a.set("end", at->end);
        o.set("at", std::move(a));
    }
    return o;
}

Budgets Budgets::from_json(const Json& j) {
    Budgets b;
    if (!j.is_object()) throw RqlException("invalid-plan", "budgets must be an object");
    for (size_t i = 0; i < j.size(); ++i) {
        const std::string& k = j.key(i);
        const Json& v = j.at(i);
        if (!v.is_number() || std::floor(v.as_number()) != v.as_number() || v.as_number() < 0 || v.as_number() > 2147483647.0)
            throw RqlException("invalid-plan", "budget '" + k + "' must be a non-negative integer");
        int n = static_cast<int>(v.as_number());
        if (k == "maxVisited") b.max_visited = n;
        else if (k == "maxRows") b.max_rows = n;
        else if (k == "maxListItems") b.max_list_items = n;
        else if (k == "maxDepth") b.max_depth = n;
        else if (k == "timeoutMs") b.timeout_ms = n;
        else if (k == "maxOutputBytes") b.max_output_bytes = n;
        else if (k == "maxMutationTargets") b.max_mutation_targets = n;
        else throw RqlException("invalid-plan", "unknown budget '" + k + "'");
    }
    return b;
}

QueryOptions QueryOptions::from_json(const Json& j) {
    QueryOptions o;
    if (!j.is_object()) throw RqlException("invalid-plan", "options must be an object");
    for (size_t i = 0; i < j.size(); ++i) {
        const std::string& k = j.key(i);
        const Json& v = j.at(i);
        if (!v.is_bool()) throw RqlException("invalid-plan", "option '" + k + "' must be a boolean");
        if (k == "dryRun") o.dry_run = v.as_bool();
        else if (k == "allowMutation") o.allow_mutation = v.as_bool();
        else throw RqlException("invalid-plan", "unknown option '" + k + "'");
    }
    return o;
}

Json QueryResult::to_json(bool include_stats) const {
    Json o = Json::object();
    o.set("rql", kRqlVersion);
    o.set("ok", ok);
    if (kind) o.set("kind", result_kind_name(*kind));
    if (columns) {
        Json a = Json::array();
        for (auto& c : *columns) a.push(Json(c));
        o.set("columns", std::move(a));
    }
    if (rows) {
        Json a = Json::array();
        for (auto& row : *rows) {
            Json r = Json::array();
            for (auto& cell : row) r.push(cell);
            a.push(std::move(r));
        }
        o.set("rows", std::move(a));
    }
    if (items) {
        Json a = Json::array();
        for (auto& it : *items) a.push(it);
        o.set("items", std::move(a));
    }
    if (has_value) o.set("value", value);
    if (types) o.set("types", *types);
    if (total) o.set("total", static_cast<int64_t>(*total));
    if (truncated) o.set("truncated", true);
    if (dry_run) o.set("dryRun", true);
    if (!diagnostics.empty()) {
        Json a = Json::array();
        for (auto& d : diagnostics) a.push(d.to_json());
        o.set("diagnostics", std::move(a));
    }
    if (include_stats) {
        Json s = Json::object();
        s.set("visited", static_cast<int64_t>(stats.visited));
        s.set("membersRead", static_cast<int64_t>(stats.members_read));
        s.set("elapsedMs", std::round(stats.elapsed_ms * 1000.0) / 1000.0);
        o.set("stats", std::move(s));
    }
    return o;
}

namespace {
template <class T>
bool try_json(const Json& j, T& out, Diagnostic& error) noexcept {
    try {
        out = T::from_json(j);
        return true;
    } catch (const RqlException& ex) {
        try {
            error = Diagnostic::from(ex);
        } catch (...) {
        }
    } catch (...) {
    }
    return false;
}
}  // namespace

bool Budgets::try_from_json(const Json& j, Budgets& out, Diagnostic& error) noexcept { return try_json(j, out, error); }
bool QueryOptions::try_from_json(const Json& j, QueryOptions& out, Diagnostic& error) noexcept { return try_json(j, out, error); }

}  // namespace rql
