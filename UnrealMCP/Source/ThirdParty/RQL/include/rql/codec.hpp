// Canonical QueryPlan JSON codec (mirrors dotnet/Rql.Contracts/PlanCodec.cs):
// fixed key order, optional fields at their default omitted; decoding validates structure and
// throws RqlException with the same codes (invalid-plan, version-unsupported, arity, unknown-stage).
#pragma once

#include <string>
#include <string_view>

#include "rql/json.hpp"
#include "rql/plan.hpp"
#include "rql/result.hpp"

namespace rql {

Json encode_plan(const QueryPlan& plan);
Json encode_expr(const Expr& e);
std::string serialize_plan(const QueryPlan& plan, int indent = -1);

QueryPlan decode_plan(const Json& json);
/// Parses JSON text first; malformed JSON → invalid-plan.
QueryPlan decode_plan(std::string_view json_text);
ExprPtr decode_expr(const Json& json);

/// Non-throwing variants (use these from code built without exception handling at call sites).
bool try_decode_plan(const Json& json, QueryPlan& out, Diagnostic& error) noexcept;
bool try_decode_plan(std::string_view json_text, QueryPlan& out, Diagnostic& error) noexcept;

/// Structural validation shared by codec and host-built plans (terminal placement).
void validate_plan_structure(const QueryPlan& plan);

/// ["key"] / [1.5] display form of a key literal.
std::string format_key_literal(const Literal& key);

}  // namespace rql
