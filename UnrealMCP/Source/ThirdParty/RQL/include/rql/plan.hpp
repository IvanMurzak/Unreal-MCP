// QueryPlan model (spec/query-plan.schema.json). Runtime-neutral; produced by the .NET/TS parser
// (or built in code with the helpers at the bottom) and executed by rql::Engine.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rql {

constexpr const char* kRqlVersion = "0.1";

/// Literal value of a `lit` expression or a `key` segment: null, bool, number (double) or string.
struct Literal {
    enum class Type : uint8_t { Null, Bool, Number, String };
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;

    static Literal null() { return {}; }
    static Literal boolean(bool v) { Literal l; l.type = Type::Bool; l.b = v; return l; }
    static Literal number(double v) { Literal l; l.type = Type::Number; l.n = v; return l; }
    static Literal string(std::string v) { Literal l; l.type = Type::String; l.s = std::move(v); return l; }
};

enum class SegmentKind : uint8_t { Member, Index, Key, Meta };

struct Segment {
    SegmentKind kind = SegmentKind::Member;
    std::string name;  // member or meta name (meta without '$')
    int64_t index = 0;
    Literal key;       // string or number

    static Segment member(std::string n) { Segment s; s.kind = SegmentKind::Member; s.name = std::move(n); return s; }
    static Segment meta(std::string n) { Segment s; s.kind = SegmentKind::Meta; s.name = std::move(n); return s; }
    static Segment at(int64_t i) { Segment s; s.kind = SegmentKind::Index; s.index = i; return s; }
    static Segment for_key(Literal k) { Segment s; s.kind = SegmentKind::Key; s.key = std::move(k); return s; }

    /// Display form used in labels and $path: name, $meta, [i], ["key"].
    std::string to_string() const;
};

enum class ExprKind : uint8_t { Lit, List, Object, Path, Op, Call };

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

struct Expr {
    ExprKind kind = ExprKind::Lit;
    Literal lit;                    // Lit
    std::vector<ExprPtr> args;      // List items / Object values / Op args / Call args
    std::vector<std::string> keys;  // Object keys (parallel to args; unique, non-empty)
    std::vector<Segment> segments;  // Path
    ExprPtr on;                     // Path / Call receiver (optional)
    std::string name;               // Op operator / Call function or method name
};

enum class StageKind : uint8_t {
    Where, Select, Sort, First, Skip, Count, Describe, Get, Sum, Min, Max, Avg, GroupBy, Distinct, Recurse, Set, Call, Generic
};

struct SelectField { std::string name; ExprPtr expr; };
struct SortKey { ExprPtr expr; bool desc = false; };
struct Assignment { ExprPtr target; ExprPtr value; };  // target is a Path expr

struct Stage {
    StageKind kind = StageKind::Where;
    std::string op;
    ExprPtr expr;                       // where pred; get/sum/min/max/avg/group-by; distinct (optional)
    std::vector<SelectField> fields;    // select
    std::vector<SortKey> keys;          // sort
    int n = 0;                          // first / skip
    bool has_via = false;               // recurse: via present (even if empty)
    std::vector<ExprPtr> via;
    std::optional<int> max_depth;
    bool include_self = true;           // recurse: emit input items (depth 0); default true
    std::vector<Assignment> assign;     // set
    std::string method;                 // call
    std::vector<ExprPtr> args;          // call / generic positional args
    std::vector<std::pair<std::string, ExprPtr>> named;  // generic named args

    /// Terminal stages must be last: count, describe, sum/min/max/avg, set, call.
    bool is_terminal() const;
    bool is_mutation() const { return kind == StageKind::Set || kind == StageKind::Call; }
};

struct QueryPlan {
    std::string version = kRqlVersion;
    ExprPtr source;
    std::vector<Stage> stages;
};

/// Core stage names (everything else must be namespaced: ns.stage).
bool is_core_stage(const std::string& op, StageKind* kind = nullptr);
/// Expression operators of the plan schema.
bool is_expression_op(const std::string& op);

// ---------------------------------------------------------------- builders (host-side plans)

namespace plan {
ExprPtr lit(Literal v);
inline ExprPtr lit_null() { return lit(Literal::null()); }
inline ExprPtr lit(double v) { return lit(Literal::number(v)); }
inline ExprPtr lit(bool v) { return lit(Literal::boolean(v)); }
inline ExprPtr lit(const char* v) { return lit(Literal::string(v)); }
inline ExprPtr lit(std::string v) { return lit(Literal::string(std::move(v))); }
ExprPtr list(std::vector<ExprPtr> items);
/// Object literal { key: expr, ... } (spec §4.5).
ExprPtr object(std::vector<std::pair<std::string, ExprPtr>> entries);
ExprPtr path(std::vector<Segment> segs, ExprPtr on = nullptr);
/// Dotted member path, e.g. path("Pos.x").
ExprPtr path(const std::string& dotted);
ExprPtr op(std::string name, std::vector<ExprPtr> args);
ExprPtr call(std::string name, std::vector<ExprPtr> args, ExprPtr on = nullptr);
}  // namespace plan

}  // namespace rql
