// QueryResult / Diagnostic / Budgets (spec/query-result.schema.json, semantics.md §6, §10).
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "rql/json.hpp"

namespace rql {

/// Thrown for malformed plans and fatal execution errors. code() is a spec diagnostic code.
class RqlException : public std::runtime_error {
public:
    RqlException(std::string code, const std::string& message, std::string hint = {})
        : std::runtime_error(message), code_(std::move(code)), hint_(std::move(hint)) {}
    const std::string& code() const { return code_; }
    const std::string& hint() const { return hint_; }

private:
    std::string code_;
    std::string hint_;
};

enum class Severity : uint8_t { Error, Warning };

struct TextSpan { int start = 0; int end = 0; };

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string code;
    std::string message;
    std::string hint;              // empty = none
    std::optional<TextSpan> at;

    static Diagnostic error(std::string code, std::string message, std::string hint = {}) {
        return {Severity::Error, std::move(code), std::move(message), std::move(hint), std::nullopt};
    }
    static Diagnostic warning(std::string code, std::string message, std::string hint = {}) {
        return {Severity::Warning, std::move(code), std::move(message), std::move(hint), std::nullopt};
    }
    static Diagnostic from(const RqlException& e) { return error(e.code(), e.what(), e.hint()); }
    Json to_json() const;
};

/// Request-level resource limits. Unset = runtime default.
struct Budgets {
    std::optional<int> max_visited;
    std::optional<int> max_rows;
    std::optional<int> max_list_items;
    std::optional<int> max_depth;
    std::optional<int> timeout_ms;
    std::optional<int> max_output_bytes;
    std::optional<int> max_mutation_targets;

    /// Parses {"maxVisited":..,...}; throws RqlException(invalid-plan) on unknown keys / bad values.
    static Budgets from_json(const Json& j);
    static bool try_from_json(const Json& j, Budgets& out, Diagnostic& error) noexcept;
};

struct QueryOptions {
    bool dry_run = false;
    bool allow_mutation = false;
    static QueryOptions from_json(const Json& j);
    static bool try_from_json(const Json& j, QueryOptions& out, Diagnostic& error) noexcept;
};

enum class ResultKind : uint8_t { Rows, Items, Scalar, Schema };

struct Stats {
    int64_t visited = 0;
    int64_t members_read = 0;
    double elapsed_ms = 0;
};

/// Execution result. Values are already encoded to JSON (spec §6).
struct QueryResult {
    bool ok = false;
    std::optional<ResultKind> kind;
    std::optional<std::vector<std::string>> columns;
    std::optional<std::vector<std::vector<Json>>> rows;
    std::optional<std::vector<Json>> items;
    bool has_value = false;
    Json value;
    std::optional<Json> types;  // array of type descriptors (describe)
    std::optional<int64_t> total;
    bool truncated = false;
    bool dry_run = false;
    /// A set receipt: how many assignments left the value as it was (old == new), omitted from the rows.
    int64_t unchanged = 0;
    std::vector<Diagnostic> diagnostics;
    Stats stats;

    static QueryResult fail(std::vector<Diagnostic> diags) {
        QueryResult r;
        r.diagnostics = std::move(diags);
        return r;
    }

    Json to_json(bool include_stats = true) const;
    std::string dump(bool include_stats = true, int indent = -1) const { return to_json(include_stats).dump(indent); }
};

const char* result_kind_name(ResultKind k);

}  // namespace rql
