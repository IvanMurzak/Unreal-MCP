// Live value → JSON (spec §6). Mirrors dotnet/Rql.Execution/Encoder.cs and Describe.cs.
#include <algorithm>
#include <cmath>

#include "exec.hpp"

namespace rql {
namespace detail {

namespace {
const char* kEllipsis = "\xE2\x80\xA6";  // U+2026
}

Json Encoder::number(double d, bool single) {
    if (single) {
        double back = 0;
        if (parse_double(format_float(static_cast<float>(d)), back)) d = back;
    }
    if (std::isnan(d) || std::isinf(d)) return Json(format_number_text(d));
    if (std::fabs(d) < 9007199254740992.0 && std::floor(d) == d) return Json::number(d == 0 ? 0.0 : d, true);
    return Json(d);
}

Json Encoder::summary(const Value& obj) {
    Json o = Json::object();
    o.set("$ref", x_.ref_of(obj));
    if (const MemberDescriptor* dm = x_.view(obj.type()).display()) {
        try {
            Value name = dm->getter(obj.ptr());
            if (name.is_string()) o.set("name", name.as_string());
        } catch (const std::exception&) {
            // summaries never fail
        }
    }
    return o;
}

Json Encoder::inline_object(const Value& obj, int depth) {
    Json o = Json::object();
    const TypeView view = x_.view(obj.type());
    for (auto& m : view.implicit_members()) {  // explicit-only members are never read implicitly
        Value mv;
        try {
            mv = m.getter(obj.ptr());
        } catch (const std::exception& ex) {
            mv = Value::error("getter-threw", ex.what());
        }
        if (mv.is_list() || mv.is_map()) {
            o.set(m.name, friendly_type_of(mv) + "[" + std::to_string(collection_size(mv)) + "]");
            continue;
        }
        if (mv.is_object()) {
            const TypeView tv = x_.view(mv.type());
            bool value_like = !tv.id() && !tv.display();
            o.set(m.name, value_like && depth < 1 ? inline_object(mv, depth + 1) : summary(mv));
            continue;
        }
        o.set(m.name, encode(mv, depth + 1));
    }
    return o;
}

// Like the comparison nesting limit (spec §4.1): deeper lists and maps encode as "…".
constexpr size_t kMaxEncodeNesting = 32;

/// Pops the collection entered last, also when encoding its elements throws.
struct Leave {
    std::vector<const void*>& open;
    ~Leave() { open.pop_back(); }
};

bool Encoder::enter(const void* collection) {
    if (open_.size() >= kMaxEncodeNesting || std::find(open_.begin(), open_.end(), collection) != open_.end()) return false;
    open_.push_back(collection);
    return true;
}

Json Encoder::encode(const Value& v, int depth) {
    if (x_.is_dead(v)) return Json();  // destroyed objects encode as null (spec §13 liveness)
    // Adapter encoding first (spec §13): the first adapter that encodes wins.
    if (x_.has_capability(Capability::Encode)) {
        const auto& encoders = x_.adapters_for(v, Capability::Encode);
        if (!encoders.empty()) {
            EncodeContext ctx(*this, depth);
            if (auto j = first_answer(encoders, [&](const TypeAdapter& a) { return a.encode(v, ctx); })) return std::move(*j);
        }
    }
    switch (v.kind()) {
        case Value::Kind::Missing:
        case Value::Kind::Null:
        case Value::Kind::Unknown: return Json();
        case Value::Kind::Error: {
            Json o = Json::object();
            o.set("$error", v.error_code());
            return o;
        }
        case Value::Kind::Bool: return Json(v.as_bool());
        case Value::Kind::String: return Json(v.as_string());
        case Value::Kind::Number: return number(v.as_number(), v.is_single());
        case Value::Kind::Enum: return Json(to_text(v));
        case Value::Kind::Row: {
            Json o = Json::object();
            const RowData& r = v.row();
            for (size_t i = 0; i < r.values.size(); ++i) o.set((*r.columns)[i], encode(r.values[i], depth + 1));
            return o;
        }
        case Value::Kind::Entry: {
            Json o = Json::object();
            o.set("key", encode(v.entry().key, depth + 1));
            o.set("value", encode(v.entry().value, depth + 1));
            return o;
        }
        case Value::Kind::List: {
            auto& l = v.list();
            if (!enter(l.identity())) return Json(kEllipsis);
            Leave leave{open_};
            Json arr = Json::array();
            size_t total = l.size();
            size_t limit = static_cast<size_t>(std::max(0, x_.max_list_items));
            size_t n = std::min(total, limit);
            for (size_t i = 0; i < n; ++i) arr.push(encode(l.at(i), depth + 1));
            if (total > n) arr.push(Json(std::string(kEllipsis) + "+" + std::to_string(total - n)));
            return arr;
        }
        case Value::Kind::Map: {
            if (!enter(v.map().identity())) return Json(kEllipsis);
            Leave leave{open_};
            Json o = Json::object();
            size_t n = 0, total = 0;
            size_t limit = static_cast<size_t>(std::max(0, x_.max_list_items));
            v.map().for_each([&](const Value& k, const Value& val) {
                ++total;
                if (n >= limit) return true;
                std::string key = to_text(k);
                if (o.contains(key)) return true;
                o.set(key, encode(val, depth + 1));
                ++n;
                return true;
            });
            if (total > n) o.set(kEllipsis, "+" + std::to_string(total - n));
            return o;
        }
        case Value::Kind::Struct: {
            if (depth > 2) return Json(kEllipsis);
            Json o = Json::object();
            const TypeView view = x_.view(v.type());
            for (auto& m : view.implicit_members()) {
                Value mv;
                try {
                    mv = m.getter(v.ptr());
                } catch (const std::exception& ex) {
                    mv = Value::error("getter-threw", ex.what());
                }
                o.set(m.name, encode(mv, depth + 1));
            }
            return o;
        }
        case Value::Kind::Object: {
            const TypeView tv = x_.view(v.type());
            if (!tv.id() && !tv.display() && depth <= 1) return inline_object(v, depth);
            return summary(v);
        }
    }
    return Json();
}

QueryResult Encoder::rows(const std::vector<std::string>& columns, const std::vector<std::vector<Value>>& rows, const char* receipt) {
    QueryResult r;
    r.ok = true;
    r.kind = ResultKind::Rows;
    r.columns = columns;
    r.rows.emplace();
    int64_t bytes = 0;
    for (auto& row : rows) {
        std::vector<Json> enc;
        enc.reserve(row.size());
        for (auto& v : row) {
            enc.push_back(encode(v));
            bytes += static_cast<int64_t>(enc.back().dump().size());
        }
        bytes += static_cast<int64_t>(columns.size());
        if (bytes > x_.max_output_bytes && !r.rows->empty()) {
            r.truncated = true;
            r.diagnostics.push_back(Diagnostic::warning(
                "output-truncated",
                "output limited to " + std::to_string(x_.max_output_bytes) + " bytes; " + std::to_string(r.rows->size()) + " of " +
                    std::to_string(rows.size()) + " rows returned",
                receipt ? "the receipt is shortened, not the change: all " + std::to_string(rows.size()) +
                              (x_.options.dry_run ? std::string(" planned ") + receipt + " were checked (dry run)" : std::string(" ") + receipt + " were made") +
                              "; do not repeat it"
                        : std::string("select fewer/smaller columns or page with skip/first")));
            break;
        }
        r.rows->push_back(std::move(enc));
    }
    r.total = static_cast<int64_t>(rows.size());
    return r;
}

QueryResult Encoder::items(const std::vector<Value>& items) {
    QueryResult r;
    r.ok = true;
    r.kind = ResultKind::Items;
    r.items.emplace();
    int64_t bytes = 0;
    for (auto& it : items) {
        Json n = encode(it);
        bytes += static_cast<int64_t>(n.dump().size());
        if (bytes > x_.max_output_bytes && !r.items->empty()) {
            r.truncated = true;
            r.diagnostics.push_back(Diagnostic::warning(
                "output-truncated",
                "output limited to " + std::to_string(x_.max_output_bytes) + " bytes; " + std::to_string(r.items->size()) + " of " +
                    std::to_string(items.size()) + " items returned",
                "use select to project fewer members"));
            break;
        }
        r.items->push_back(std::move(n));
    }
    return r;
}

// ------------------------------------------------------------------ describe

QueryResult run_describe(Exec& x, Stream& stream) {
    std::vector<const TypeDescriptor*> types;
    std::vector<std::vector<std::string>> row_columns;
    Item it;
    while (stream.next(it)) {
        const Value& v = it.value;
        if (v.is_null() || v.is_absent()) continue;
        const TypeDescriptor* t = v.type();
        if (!t) t = v.is_string() ? builtin::string_type() : v.is_bool() ? builtin::bool_type() : builtin::double_type();
        if (std::find(types.begin(), types.end(), t) == types.end()) {
            types.push_back(t);
            row_columns.push_back(v.is_row() ? *v.row().columns : std::vector<std::string>{});
        }
        if (types.size() >= 8) break;
    }
    Json arr = Json::array();
    for (size_t i = 0; i < types.size(); ++i) {
        Json d = x.engine.describe_type(*types[i]);
        if (types[i] == builtin::row_type()) {
            Json members = Json::array();
            for (auto& c : row_columns[i]) {
                Json m = Json::object();
                m.set("name", c);
                m.set("type", "object");
                members.push(std::move(m));
            }
            d.set("members", std::move(members));
        }
        arr.push(std::move(d));
    }
    QueryResult r;
    r.ok = true;
    r.kind = ResultKind::Schema;
    r.types = std::move(arr);
    return r;
}

}  // namespace detail
}  // namespace rql
