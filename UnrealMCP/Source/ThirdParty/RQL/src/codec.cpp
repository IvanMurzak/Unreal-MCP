#include <string_view>
#include "rql/codec.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace rql {

// ------------------------------------------------------------------ plan model helpers

namespace {
struct CoreStage { const char* name; StageKind kind; };
const CoreStage kCoreStages[] = {
    {"where", StageKind::Where}, {"select", StageKind::Select}, {"sort", StageKind::Sort},
    {"first", StageKind::First}, {"skip", StageKind::Skip}, {"count", StageKind::Count},
    {"describe", StageKind::Describe}, {"get", StageKind::Get}, {"sum", StageKind::Sum},
    {"min", StageKind::Min}, {"max", StageKind::Max}, {"avg", StageKind::Avg},
    {"group-by", StageKind::GroupBy}, {"distinct", StageKind::Distinct}, {"recurse", StageKind::Recurse},
    {"set", StageKind::Set}, {"call", StageKind::Call},
};
const char* kExprOps[] = {"or", "and", "not", "==", "!=", "<", "<=", ">", ">=", "contains", "starts-with",
                          "ends-with", "matches", "in", "is", "neg", "+", "-", "*", "/", "%", "index"};
}  // namespace

bool is_core_stage(const std::string& op, StageKind* kind) {
    for (auto& s : kCoreStages) {
        if (op == s.name) {
            if (kind) *kind = s.kind;
            return true;
        }
    }
    return false;
}

std::string group_by_problem(const std::vector<SelectField>& keys) {
    auto upper = [](std::string s) {
        for (auto& ch : s)
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
        return s;
    };
    std::vector<std::string> seen{"COUNT"};
    for (auto& k : keys) {
        std::string u = upper(k.name);
        if (std::find(seen.begin(), seen.end(), u) != seen.end())
            return u == "COUNT" ? "group-by key column 'count' clashes with the count column; name the key: group-by n=count"
                                : "group-by key column '" + k.name + "' appears twice; name the keys: group-by a=..., b=...";
        seen.push_back(u);
    }
    return "";
}

bool is_expression_op(const std::string& op) {
    for (auto* o : kExprOps)
        if (op == o) return true;
    return false;
}

bool Stage::is_terminal() const {
    switch (kind) {
        case StageKind::Count:
        case StageKind::Describe:
        case StageKind::Sum:
        case StageKind::Min:
        case StageKind::Max:
        case StageKind::Avg:
        case StageKind::Set:
        case StageKind::Call:
            return true;
        default:
            return false;
    }
}

std::string format_key_literal(const Literal& key) {
    if (key.type == Literal::Type::String) {
        std::string s;
        json_escape_to(s, key.s);
        return s;
    }
    if (key.type == Literal::Type::Number) return format_number_text(key.n);
    return "null";
}

std::string Segment::to_string() const {
    switch (kind) {
        case SegmentKind::Member: return name;
        case SegmentKind::Meta: return "$" + name;
        case SegmentKind::Index: return "[" + std::to_string(index) + "]";
        default: return "[" + format_key_literal(key) + "]";
    }
}

namespace plan {
ExprPtr lit(Literal v) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Lit;
    e->lit = std::move(v);
    return e;
}
ExprPtr list(std::vector<ExprPtr> items) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::List;
    e->args = std::move(items);
    return e;
}
ExprPtr object(std::vector<std::pair<std::string, ExprPtr>> entries) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Object;
    for (auto& kv : entries) {
        e->keys.push_back(std::move(kv.first));
        e->args.push_back(std::move(kv.second));
    }
    return e;
}
ExprPtr path(std::vector<Segment> segs, ExprPtr on) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Path;
    e->segments = std::move(segs);
    e->on = std::move(on);
    return e;
}
ExprPtr path(const std::string& dotted) {
    std::vector<Segment> segs;
    size_t start = 0;
    while (start <= dotted.size() && !dotted.empty()) {
        size_t dot = dotted.find('.', start);
        std::string part = dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!part.empty()) segs.push_back(part[0] == '$' ? Segment::meta(part.substr(1)) : Segment::member(part));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return path(std::move(segs));
}
ExprPtr op(std::string name, std::vector<ExprPtr> args) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Op;
    e->name = std::move(name);
    e->args = std::move(args);
    return e;
}
ExprPtr call(std::string name, std::vector<ExprPtr> args, ExprPtr on) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Call;
    e->name = std::move(name);
    e->args = std::move(args);
    e->on = std::move(on);
    return e;
}
}  // namespace plan

// ------------------------------------------------------------------ serialize

namespace {

Json number_json(double d) {
    // Integral values without fraction (except -0, which keeps its sign as a double).
    if (std::fabs(d) < 9007199254740992.0 && std::floor(d) == d && !(d == 0 && std::signbit(d)))
        return Json::number(d, true);
    return Json::number(d, false);
}

Json literal_json(const Literal& l) {
    switch (l.type) {
        case Literal::Type::Null: return Json();
        case Literal::Type::Bool: return Json(l.b);
        case Literal::Type::Number: return number_json(l.n);
        default: return Json(l.s);
    }
}

Json expr_array(const std::vector<ExprPtr>& items) {
    Json a = Json::array();
    for (auto& i : items) a.push(encode_expr(*i));
    return a;
}

Json segment_json(const Segment& s) {
    Json o = Json::object();
    switch (s.kind) {
        case SegmentKind::Member: return Json(s.name);
        case SegmentKind::Index: o.set("index", static_cast<int64_t>(s.index)); break;
        case SegmentKind::Meta: o.set("meta", s.name); break;
        case SegmentKind::Key: o.set("key", literal_json(s.key)); break;
    }
    return o;
}

Json fields_json(const std::vector<SelectField>& fields) {
    Json a = Json::array();
    for (auto& f : fields) {
        Json fo = Json::object();
        fo.set("name", f.name);
        fo.set("expr", encode_expr(*f.expr));
        a.push(std::move(fo));
    }
    return a;
}

Json stage_json(const Stage& s) {
    Json o = Json::object();
    o.set("op", s.op);
    switch (s.kind) {
        case StageKind::Where: o.set("pred", encode_expr(*s.expr)); break;
        case StageKind::Select: o.set("fields", fields_json(s.fields)); break;
        case StageKind::Sort: {
            Json a = Json::array();
            for (auto& k : s.keys) {
                Json ko = Json::object();
                ko.set("expr", encode_expr(*k.expr));
                if (k.desc) ko.set("desc", true);
                a.push(std::move(ko));
            }
            o.set("keys", std::move(a));
            break;
        }
        case StageKind::First:
        case StageKind::Skip: o.set("n", s.n); break;
        case StageKind::Count:
        case StageKind::Describe: break;
        case StageKind::Get:
        case StageKind::Sum:
        case StageKind::Min:
        case StageKind::Max:
        case StageKind::Avg:
        case StageKind::Distinct:
            if (s.expr) o.set("expr", encode_expr(*s.expr));
            break;
        case StageKind::GroupBy:
            if (s.expr) o.set("expr", encode_expr(*s.expr));
            else o.set("fields", fields_json(s.fields));  // several / named keys (spec §5)
            break;
        case StageKind::Recurse:
            if (!s.via.empty()) o.set("via", expr_array(s.via));
            if (s.max_depth) o.set("maxDepth", *s.max_depth);
            if (!s.include_self) o.set("includeSelf", false);
            break;
        case StageKind::Set: {
            Json a = Json::array();
            for (auto& as : s.assign) {
                Json ao = Json::object();
                ao.set("target", encode_expr(*as.target));
                ao.set("value", encode_expr(*as.value));
                a.push(std::move(ao));
            }
            o.set("assign", std::move(a));
            break;
        }
        case StageKind::Call:
            o.set("method", s.method);
            if (s.on) o.set("on", encode_expr(*s.on));
            o.set("args", expr_array(s.args));
            break;
        case StageKind::Generic: {
            if (!s.args.empty()) o.set("args", expr_array(s.args));
            if (!s.named.empty()) {
                auto named = s.named;
                std::stable_sort(named.begin(), named.end(), [](auto& a, auto& b) { return a.first < b.first; });
                Json no = Json::object();
                for (auto& kv : named) no.set(kv.first, encode_expr(*kv.second));
                o.set("named", std::move(no));
            }
            break;
        }
    }
    return o;
}

}  // namespace

Json encode_expr(const Expr& e) {
    Json o = Json::object();
    switch (e.kind) {
        case ExprKind::Lit: o.set("lit", literal_json(e.lit)); break;
        case ExprKind::List: o.set("list", expr_array(e.args)); break;
        case ExprKind::Object: {
            Json entries = Json::array();
            for (size_t i = 0; i < e.keys.size(); ++i) {
                Json entry = Json::object();
                entry.set("key", e.keys[i]);
                entry.set("value", encode_expr(*e.args[i]));
                entries.push(std::move(entry));
            }
            o.set("object", std::move(entries));
            break;
        }
        case ExprKind::Path: {
            Json a = Json::array();
            for (auto& s : e.segments) a.push(segment_json(s));
            o.set("path", std::move(a));
            if (e.on) o.set("on", encode_expr(*e.on));
            break;
        }
        case ExprKind::Op:
            o.set("op", e.name);
            o.set("args", expr_array(e.args));
            break;
        case ExprKind::Call:
            o.set("call", e.name);
            if (e.on) o.set("on", encode_expr(*e.on));
            o.set("args", expr_array(e.args));
            break;
    }
    return o;
}

Json encode_plan(const QueryPlan& plan) {
    Json o = Json::object();
    o.set("rql", plan.version);
    o.set("source", encode_expr(*plan.source));
    Json st = Json::array();
    for (auto& s : plan.stages) st.push(stage_json(s));
    o.set("stages", std::move(st));
    return o;
}

std::string serialize_plan(const QueryPlan& plan, int indent) { return encode_plan(plan).dump(indent); }

// ------------------------------------------------------------------ deserialize

namespace {

[[noreturn]] void invalid(const std::string& m) { throw RqlException("invalid-plan", m); }

// `ctx` is a string_view by value: a `const std::string&` would bind a temporary for every literal
// argument, which GCC 13+ reports as a possibly dangling reference in the returned `const Json&`.
const Json& required(const Json& obj, const char* name, std::string_view ctx) {
    const Json* v = obj.find(name);
    if (!v) invalid(std::string(ctx) + "." + name + " is required");
    return *v;
}

void allow_only(const Json& obj, const std::string& ctx, std::initializer_list<const char*> names) {
    if (!obj.is_object()) invalid(ctx + " must be an object");
    for (size_t i = 0; i < obj.size(); ++i) {
        const std::string& k = obj.key(i);
        bool ok = false;
        for (auto* n : names)
            if (k == n) ok = true;
        if (!ok) invalid("unexpected property '" + k + "' in " + ctx);
    }
}

Literal read_literal(const Json& v, const std::string& ctx) {
    switch (v.type()) {
        case Json::Type::Null: return Literal::null();
        case Json::Type::Bool: return Literal::boolean(v.as_bool());
        case Json::Type::Number:
            if (!std::isfinite(v.as_number())) invalid(ctx + " number is out of range");  // NaN / Infinity / overflow
            return Literal::number(v.as_number());
        case Json::Type::String: return Literal::string(v.as_string());
        default: invalid(ctx + " must be a scalar literal");
    }
}

/// Integers are JSON numbers with an integral value (1 and 1.0 are both accepted; 1.5 is not).
bool is_int_value(const Json& v, double lo, double hi) {
    if (!v.is_number()) return false;
    double d = v.as_number();
    if (std::floor(d) != d || (!v.integral_text() && std::fabs(d) > 9007199254740992.0)) return false;
    return d >= lo && d <= hi;
}

std::vector<ExprPtr> read_expr_array(const Json& arr, const std::string& ctx) {
    if (!arr.is_array()) invalid(ctx + " must be an array");
    std::vector<ExprPtr> list;
    for (auto& i : arr.items()) list.push_back(decode_expr(i));
    return list;
}

ExprPtr read_path(const Json& e, const Json& path) {
    if (!path.is_array()) invalid("path must be an array");
    std::vector<Segment> segs;
    for (auto& s : path.items()) {
        if (s.is_string()) {
            if (s.as_string().empty()) invalid("member segment must not be empty");
            segs.push_back(Segment::member(s.as_string()));
        } else if (s.is_object()) {
            if (const Json* ix = s.find("index")) {
                allow_only(s, "index segment", {"index"});
                if (!is_int_value(*ix, -9223372036854775808.0, 9223372036854775807.0)) invalid("index must be an integer");
                segs.push_back(Segment::at(static_cast<int64_t>(ix->as_number())));
            } else if (const Json* key = s.find("key")) {
                allow_only(s, "key segment", {"key"});
                Literal k = read_literal(*key, "key");
                if (k.type != Literal::Type::String && k.type != Literal::Type::Number) invalid("key must be a string or number");
                segs.push_back(Segment::for_key(std::move(k)));
            } else if (const Json* meta = s.find("meta")) {
                allow_only(s, "meta segment", {"meta"});
                if (!meta->is_string() || meta->as_string().empty()) invalid("meta must be a non-empty string");
                segs.push_back(Segment::meta(meta->as_string()));
            } else {
                invalid("segment object must have index, key or meta");
            }
        } else {
            invalid("segment must be a string or object");
        }
    }
    ExprPtr on;
    if (const Json* onEl = e.find("on")) on = decode_expr(*onEl);
    return plan::path(std::move(segs), std::move(on));
}

int read_int(const Json& obj, const char* name, const std::string& ctx, int min) {
    const Json& v = required(obj, name, ctx);
    if (!is_int_value(v, -2147483648.0, 2147483647.0) || v.as_number() < min)
        invalid(ctx + "." + name + " must be an integer >= " + std::to_string(min));
    return static_cast<int>(v.as_number());
}

bool read_true_flag(const Json& obj, const char* name, const std::string& ctx) {
    const Json* v = obj.find(name);
    if (!v) return false;
    if (v->is_bool()) return v->as_bool();
    invalid(ctx + "." + name + " must be a boolean");
}

/// select fields / group-by keys: a non-empty array of {name, expr}.
std::vector<SelectField> decode_fields(const Json& arr, const std::string& op) {
    if (!arr.is_array() || arr.size() == 0) invalid(op + ".fields must be a non-empty array");
    std::vector<SelectField> fields;
    for (auto& f : arr.items()) {
        if (!f.is_object()) invalid(op + " field must be an object");
        allow_only(f, op + " field", {"name", "expr"});
        const Json& name = required(f, "name", "field");
        if (!name.is_string()) invalid("field.name must be a string");
        fields.push_back({name.as_string(), decode_expr(required(f, "expr", "field"))});
    }
    return fields;
}

Stage read_stage(const Json& s) {
    if (!s.is_object()) invalid("stage must be an object");
    const Json& opEl = required(s, "op", "stage");
    if (!opEl.is_string()) invalid("stage.op must be a string");
    Stage st;
    st.op = opEl.as_string();
    const std::string& op = st.op;
    std::string ctx = "stage '" + op + "'";
    StageKind kind;
    if (!is_core_stage(op, &kind)) {
        if (op.find('.') == std::string::npos)
            throw RqlException("unknown-stage", "unknown stage '" + op + "' (extension stages must be namespaced, e.g. 'myapp." + op + "')");
        allow_only(s, ctx, {"op", "args", "named"});
        st.kind = StageKind::Generic;
        if (const Json* a = s.find("args")) st.args = read_expr_array(*a, "args");
        if (const Json* n = s.find("named")) {
            if (!n->is_object()) invalid("named must be an object");
            for (size_t i = 0; i < n->size(); ++i) {
                ExprPtr ex = decode_expr(n->at(i));
                bool replaced = false;
                for (auto& kv : st.named)
                    if (kv.first == n->key(i)) { kv.second = ex; replaced = true; }
                if (!replaced) st.named.emplace_back(n->key(i), ex);
            }
        }
        return st;
    }
    st.kind = kind;
    switch (kind) {
        case StageKind::Where:
            allow_only(s, ctx, {"op", "pred"});
            st.expr = decode_expr(required(s, "pred", ctx));
            break;
        case StageKind::Select:
            allow_only(s, ctx, {"op", "fields"});
            st.fields = decode_fields(required(s, "fields", ctx), op);
            break;
        case StageKind::Sort: {
            allow_only(s, ctx, {"op", "keys"});
            const Json& arr = required(s, "keys", ctx);
            if (!arr.is_array() || arr.size() == 0) invalid("sort.keys must be a non-empty array");
            for (auto& k : arr.items()) {
                if (!k.is_object()) invalid("sort key must be an object");
                allow_only(k, "sort key", {"expr", "desc"});
                SortKey sk;
                sk.expr = decode_expr(required(k, "expr", "sort key"));
                sk.desc = read_true_flag(k, "desc", "sort key");
                st.keys.push_back(std::move(sk));
            }
            break;
        }
        case StageKind::First:
        case StageKind::Skip:
            allow_only(s, ctx, {"op", "n"});
            st.n = read_int(s, "n", op, 0);
            break;
        case StageKind::Count:
        case StageKind::Describe:
            allow_only(s, ctx, {"op"});
            break;
        case StageKind::Get:
        case StageKind::Sum:
        case StageKind::Min:
        case StageKind::Max:
        case StageKind::Avg:
            allow_only(s, ctx, {"op", "expr"});
            st.expr = decode_expr(required(s, "expr", ctx));
            break;
        case StageKind::GroupBy: {
            allow_only(s, ctx, {"op", "expr", "fields"});
            const Json* gf = s.find("fields");
            const Json* ge = s.find("expr");
            if ((gf != nullptr) == (ge != nullptr)) invalid("group-by needs exactly one of 'expr' (one key) or 'fields' (named keys)");
            if (ge) {
                st.expr = decode_expr(*ge);
                break;
            }
            st.fields = decode_fields(*gf, op);
            std::string problem = group_by_problem(st.fields);
            if (!problem.empty()) invalid(problem);
            break;
        }
        case StageKind::Distinct:
            allow_only(s, ctx, {"op", "expr"});
            if (const Json* de = s.find("expr")) st.expr = decode_expr(*de);
            break;
        case StageKind::Recurse:
            allow_only(s, ctx, {"op", "via", "maxDepth", "includeSelf"});
            if (const Json* v = s.find("via")) {
                st.has_via = true;
                st.via = read_expr_array(*v, "via");
            }
            if (s.find("maxDepth")) st.max_depth = read_int(s, "maxDepth", ctx, 1);
            st.include_self = !s.find("includeSelf") || read_true_flag(s, "includeSelf", ctx);
            break;
        case StageKind::Set: {
            allow_only(s, ctx, {"op", "assign"});
            const Json& arr = required(s, "assign", ctx);
            if (!arr.is_array() || arr.size() == 0) invalid("set.assign must be a non-empty array");
            for (auto& a : arr.items()) {
                if (!a.is_object()) invalid("assignment must be an object");
                allow_only(a, "assignment", {"target", "value"});
                ExprPtr target = decode_expr(required(a, "target", "assignment"));
                bool meta = false;
                if (target->kind == ExprKind::Path)
                    for (auto& sg : target->segments)
                        if (sg.kind == SegmentKind::Meta) meta = true;
                if (target->kind != ExprKind::Path || target->segments.empty() || target->on || meta)
                    invalid("assignment.target must be a non-empty member path relative to the item (no 'on', no meta segments)");
                st.assign.push_back({target, decode_expr(required(a, "value", "assignment"))});
            }
            break;
        }
        case StageKind::Call: {
            allow_only(s, ctx, {"op", "method", "on", "args"});
            const Json& m = required(s, "method", ctx);
            if (!m.is_string() || m.as_string().empty()) invalid("call.method must be a non-empty string");
            st.method = m.as_string();
            if (const Json* on = s.find("on")) st.on = decode_expr(*on);
            st.args = read_expr_array(required(s, "args", ctx), "args");
            break;
        }
        case StageKind::Generic: break;
    }
    return st;
}

}  // namespace

namespace {
thread_local int g_expr_depth = 0;
struct DepthGuard {
    DepthGuard() {
        if (++g_expr_depth > 128) {
            --g_expr_depth;
            invalid("expression nesting exceeds 128 levels");
        }
    }
    ~DepthGuard() { --g_expr_depth; }
};
}  // namespace

ExprPtr decode_expr(const Json& e) {
    DepthGuard guard;
    if (!e.is_object()) invalid("expression must be an object");
    if (const Json* lit = e.find("lit")) {
        allow_only(e, "lit expression", {"lit"});
        return plan::lit(read_literal(*lit, "lit"));
    }
    if (const Json* list = e.find("list")) {
        allow_only(e, "list expression", {"list"});
        return plan::list(read_expr_array(*list, "list"));
    }
    if (const Json* obj = e.find("object")) {
        allow_only(e, "object expression", {"object"});
        if (!obj->is_array()) invalid("object must be an array of {key, value}");
        std::vector<std::pair<std::string, ExprPtr>> entries;
        for (auto& entry : obj->items()) {
            if (!entry.is_object()) invalid("object entries must be {key, value}");
            allow_only(entry, "object entry", {"key", "value"});
            const Json& key = required(entry, "key", "object entry");
            if (!key.is_string() || key.as_string().empty()) invalid("object entry key must be a non-empty string");
            for (auto& kv : entries)
                if (kv.first == key.as_string()) invalid("duplicate object key '" + key.as_string() + "'");
            entries.emplace_back(key.as_string(), decode_expr(required(entry, "value", "object entry")));
        }
        return plan::object(std::move(entries));
    }
    if (const Json* path = e.find("path")) {
        allow_only(e, "path expression", {"path", "on"});
        return read_path(e, *path);
    }
    if (const Json* op = e.find("op")) {
        allow_only(e, "op expression", {"op", "args"});
        if (!op->is_string()) invalid("op must be a string");
        const std::string& name = op->as_string();
        if (!is_expression_op(name)) invalid("unknown operator '" + name + "'");
        auto args = read_expr_array(required(e, "args", "op"), "args");
        size_t expected = (name == "not" || name == "neg") ? 1 : 2;
        if (args.size() != expected)
            throw RqlException("arity", "operator '" + name + "' takes " + std::to_string(expected) + " argument(s), got " +
                                            std::to_string(args.size()));
        return plan::op(name, std::move(args));
    }
    if (const Json* call = e.find("call")) {
        allow_only(e, "call expression", {"call", "on", "args"});
        if (!call->is_string() || call->as_string().empty()) invalid("call must be a non-empty string");
        auto args = read_expr_array(required(e, "args", "call"), "args");
        ExprPtr on;
        if (const Json* onEl = e.find("on")) on = decode_expr(*onEl);
        return plan::call(call->as_string(), std::move(args), std::move(on));
    }
    invalid("expression must have one of: lit, list, path, op, call");
}

void validate_plan_structure(const QueryPlan& plan) {
    for (size_t i = 0; i + 1 < plan.stages.size(); ++i) {
        if (plan.stages[i].is_terminal())
            throw RqlException("invalid-plan", "stage '" + plan.stages[i].op + "' is terminal and must be the last stage");
    }
}

QueryPlan decode_plan(const Json& root) {
    if (!root.is_object()) invalid("plan must be an object");
    allow_only(root, "plan", {"rql", "source", "stages"});
    const Json* v = root.find("rql");
    if (!v || !v->is_string()) invalid("plan.rql (version string) is required");
    if (v->as_string() != kRqlVersion)
        throw RqlException("version-unsupported",
                           "plan version '" + v->as_string() + "' is not supported (supported: " + kRqlVersion + ")");
    QueryPlan plan;
    plan.version = v->as_string();
    plan.source = decode_expr(required(root, "source", "plan"));
    const Json& stages = required(root, "stages", "plan");
    if (!stages.is_array()) invalid("plan.stages must be an array");
    for (auto& s : stages.items()) plan.stages.push_back(read_stage(s));
    validate_plan_structure(plan);
    return plan;
}

QueryPlan decode_plan(std::string_view json_text) {
    Json j;
    std::string err;
    if (!Json::try_parse(json_text, j, err, 256)) invalid("plan is not valid JSON: " + err);
    return decode_plan(j);
}

namespace {
template <class F>
bool try_decode(F f, QueryPlan& out, Diagnostic& error) noexcept {
    try {
        out = f();
        return true;
    } catch (const RqlException& ex) {
        try {
            error = Diagnostic::from(ex);
        } catch (...) {
        }
    } catch (const std::exception& ex) {
        try {
            error = Diagnostic::error("invalid-plan", ex.what());
        } catch (...) {
        }
    } catch (...) {
    }
    return false;
}
}  // namespace

bool try_decode_plan(const Json& json, QueryPlan& out, Diagnostic& error) noexcept {
    return try_decode([&] { return decode_plan(json); }, out, error);
}

bool try_decode_plan(std::string_view json_text, QueryPlan& out, Diagnostic& error) noexcept {
    return try_decode([&] { return decode_plan(json_text); }, out, error);
}

}  // namespace rql
