// Pull-based stage pipeline (spec §2, §5, §6). Mirrors dotnet/Rql.Execution/Pipeline.cs.
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "exec.hpp"

namespace rql {
namespace detail {

// ------------------------------------------------------------------ paths

std::string PathNode::str() const {
    std::vector<const PathNode*> parts;
    for (const PathNode* n = this; n; n = n->parent.get()) parts.push_back(n);
    std::string s;
    for (size_t i = parts.size(); i-- > 0;) s += parts[i]->text;
    return s;
}

PathPtr path_text(PathPtr parent, std::string text) {
    auto n = std::make_shared<PathNode>();
    n->parent = std::move(parent);
    n->text = std::move(text);
    return n;
}

PathPtr path_member(const PathPtr& parent, const std::string& name) { return path_text(parent, "." + name); }
PathPtr path_at(const PathPtr& parent, int64_t i) { return path_text(parent, "[" + std::to_string(i) + "]"); }
PathPtr path_key(const PathPtr& parent, const Value& key) {
    if (key.is_string()) {
        std::string s;
        for (char c : key.as_string()) {
            if (c == '"') s += "\\\"";
            else s.push_back(c);
        }
        return path_text(parent, "[\"" + s + "\"]");
    }
    return path_text(parent, "[" + to_text(key) + "]");
}

// ------------------------------------------------------------------ Exec

Exec::Exec(Engine& e, const Budgets& b, const QueryOptions& o, const QueryPlan& p)
    : engine(e), types(e.types()), options(o), plan(p), start_(std::chrono::steady_clock::now()) {
    const EngineDefaults& d = e.defaults();
    max_visited = std::min(b.max_visited.value_or(d.max_visited), d.hard_max_visited);
    max_rows = std::min(b.max_rows.value_or(d.max_rows), d.hard_max_rows);
    max_list_items = b.max_list_items.value_or(d.max_list_items);
    max_depth = b.max_depth.value_or(d.max_depth);
    timeout_ms = std::min(b.timeout_ms.value_or(d.timeout_ms), d.hard_timeout_ms);
    max_output_bytes = b.max_output_bytes.value_or(d.max_output_bytes);
    // Requests may change the per-request limit, but never beyond the host's hard ceiling.
    max_mutation_targets = std::min(b.max_mutation_targets.value_or(e.mutations().max_targets), e.mutations().hard_max_targets);
    has_adapters_ = e.adapts();
    caps_ = e.caps_;
    liveness_ = caps_[static_cast<size_t>(Capability::IsAlive)] || static_cast<bool>(e.is_alive);
}

void Exec::visit() {
    if (++visited > max_visited)
        throw RqlException("budget-exceeded", "visited more than " + std::to_string(max_visited) + " objects",
                           "narrow the source (e.g. a specific scene), filter earlier, lower recurse max-depth, or raise budgets.maxVisited");
    if ((visited & 1023) == 0) check_time();
}

double Exec::elapsed_ms() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
}

void Exec::check_time() const {
    if (elapsed_ms() > timeout_ms)
        throw RqlException("timeout", "query exceeded " + std::to_string(timeout_ms) + " ms", "narrow the query or raise budgets.timeoutMs");
}

void Exec::getter_threw(const std::string& member, const std::string& what) {
    if (getter_errors++ == 0) first_getter_error = "getter '" + member + "' threw: " + what;
}

namespace {

void set_scope(Exec& x, const Item& it, int64_t index) {
    x.current = it.value;
    x.current_path = it.path;
    x.current_depth = it.depth;
    x.index = index;
}

// ------------------------------------------------------------------ plan scan

bool uses_meta(const Expr& e, const std::string& meta) {
    switch (e.kind) {
        case ExprKind::Path:
            for (auto& s : e.segments)
                if (s.kind == SegmentKind::Meta && s.name == meta) return true;
            return e.on && uses_meta(*e.on, meta);
        case ExprKind::Call:
            if (e.on && uses_meta(*e.on, meta)) return true;
            [[fallthrough]];
        case ExprKind::Op:
        case ExprKind::List:
        case ExprKind::Object:
            for (auto& a : e.args)
                if (uses_meta(*a, meta)) return true;
            return false;
        default: return false;
    }
}

bool plan_uses_meta(const QueryPlan& p, const std::string& meta) {
    if (uses_meta(*p.source, meta)) return true;
    for (auto& st : p.stages) {
        std::vector<ExprPtr> all;
        if (st.expr) all.push_back(st.expr);
        for (auto& f : st.fields) all.push_back(f.expr);
        for (auto& k : st.keys) all.push_back(k.expr);
        for (auto& v : st.via) all.push_back(v);
        for (auto& a : st.assign) { all.push_back(a.target); all.push_back(a.value); }
        for (auto& a : st.args) all.push_back(a);
        for (auto& n : st.named) all.push_back(n.second);
        for (auto& e : all)
            if (uses_meta(*e, meta)) return true;
    }
    return false;
}

/// Path text appended by get/recurse-via when the expression is a plain member/index path.
std::optional<std::string> path_suffix(const Expr& e) {
    if (e.kind != ExprKind::Path || e.on) return std::nullopt;
    std::string s;
    for (auto& seg : e.segments) {
        if (seg.kind == SegmentKind::Meta) return std::nullopt;
        s += seg.kind == SegmentKind::Member ? "." + seg.name : seg.to_string();
    }
    return s;
}

// ------------------------------------------------------------------ streams

/// Lists → elements, maps → entries, one-column rows → their value, else the value; null/missing dropped.
class ExpandStream final : public Stream {
public:
    ExpandStream(Exec& x, Value v, PathPtr path, bool count) : x_(x), path_(std::move(path)), count_(count) {
        while (v.is_row() && v.row().values.size() == 1) {
            Value inner = v.row().values[0];
            v = inner;
        }
        v = x_.live(std::move(v));  // a destroyed value reads as null: skipped like a null item (spec §13)
        if (v.is_null() || v.is_absent()) mode_ = Done;
        else if (v.is_list()) mode_ = List;
        else if (v.is_map()) mode_ = Map;
        else mode_ = Single;
        v_ = std::move(v);
    }

    bool next(Item& out) override {
        switch (mode_) {
            case Done: return false;
            case Single:
                mode_ = Done;
                if (count_) x_.visit();
                out = Item{v_, path_, -1};
                return true;
            case List: {
                auto& l = v_.list();
                while (i_ < l.size()) {
                    if (count_) x_.visit();
                    PathPtr p = path_ ? path_at(path_, static_cast<int64_t>(i_)) : nullptr;
                    Value el = x_.live(l.at(i_++));
                    if (el.is_null()) continue;
                    out = Item{std::move(el), std::move(p), -1};
                    return true;
                }
                mode_ = Done;
                return false;
            }
            case Map: {
                if (!loaded_) {
                    loaded_ = true;
                    v_.map().for_each([&](const Value& k, const Value& val) {
                        entries_.emplace_back(k, val);
                        return true;
                    });
                }
                if (i_ >= entries_.size()) { mode_ = Done; return false; }
                if (count_) x_.visit();
                auto& e = entries_[i_++];
                out = Item{Value::entry(e.first, e.second), path_ ? path_key(path_, e.first) : nullptr, -1};
                return true;
            }
        }
        return false;
    }

private:
    enum Mode { Done, Single, List, Map };
    Exec& x_;
    Value v_;
    PathPtr path_;
    bool count_;
    Mode mode_ = Done;
    size_t i_ = 0;
    bool loaded_ = false;
    std::vector<std::pair<Value, Value>> entries_;
};

class SourceStream final : public Stream {
public:
    SourceStream(Exec& x, const SourceRegistration* reg, Eval nav, std::string text, bool is_ref, Eval ref_eval)
        : x_(x), reg_(reg), nav_(std::move(nav)), text_(std::move(text)), is_ref_(is_ref), ref_(std::move(ref_eval)) {}

    bool next(Item& out) override {
        if (!started_) {
            started_ = true;
            if (is_ref_) {
                x_.current = Value::null();
                Value v = ref_(x_);
                if (v.is_error()) return false;  // diagnostic already recorded
                inner_ = std::make_unique<ExpandStream>(x_, v, nullptr, true);
            } else {
                Value root = reg_->getter();
                Value value = root;
                if (nav_) {
                    x_.current = root;
                    x_.current_path = nullptr;
                    value = nav_(x_);
                }
                if (value.is_error())
                    throw RqlException(value.error_code(), "source '" + text_ + "' failed: " +
                                                               (value.error_detail().empty() ? value.error_code() : value.error_detail()));
                if (value.is_missing()) return false;
                inner_ = std::make_unique<ExpandStream>(x_, value, x_.track_paths ? path_text(nullptr, text_) : nullptr, true);
            }
        }
        return inner_ && inner_->next(out);
    }

private:
    Exec& x_;
    const SourceRegistration* reg_;
    Eval nav_;
    std::string text_;
    bool is_ref_;
    Eval ref_;
    bool started_ = false;
    StreamPtr inner_;
};

class WhereStream final : public Stream {
public:
    WhereStream(Exec& x, StreamPtr in, Eval pred) : x_(x), in_(std::move(in)), pred_(std::move(pred)) {}
    bool next(Item& out) override {
        Item it;
        while (in_->next(it)) {
            set_scope(x_, it, i_++);
            Value r = pred_(x_);
            if (r.is_bool() && r.as_bool()) {
                out = std::move(it);
                return true;
            }
        }
        return false;
    }

private:
    Exec& x_;
    StreamPtr in_;
    Eval pred_;
    int64_t i_ = 0;
};

class SelectStream final : public Stream {
public:
    SelectStream(Exec& x, StreamPtr in, std::shared_ptr<const std::vector<std::string>> cols, std::vector<Eval> evals)
        : x_(x), in_(std::move(in)), cols_(std::move(cols)), evals_(std::move(evals)) {}
    bool next(Item& out) override {
        Item it;
        if (!in_->next(it)) return false;
        set_scope(x_, it, i_++);
        std::vector<Value> vals;
        vals.reserve(evals_.size());
        for (auto& e : evals_) vals.push_back(e(x_));
        out = Item{Value::row(cols_, std::move(vals)), nullptr, -1};
        return true;
    }

private:
    Exec& x_;
    StreamPtr in_;
    std::shared_ptr<const std::vector<std::string>> cols_;
    std::vector<Eval> evals_;
    int64_t i_ = 0;
};

int sort_rank(const Value& v) {
    if (v.is_null() || v.is_absent()) return 9;
    if (v.is_number() || v.is_enum()) return 0;
    if (v.is_string()) return 1;
    if (v.is_bool()) return 2;
    return 3;
}

int compare_one(Exec& x, const Value& a, const Value& b, bool desc) {
    int ra = sort_rank(a), rb = sort_rank(b);
    if (ra == 9 || rb == 9) return ra < rb ? -1 : (ra > rb ? 1 : 0);  // nulls last in both directions
    if (ra != rb) return desc ? (rb < ra ? -1 : 1) : (ra < rb ? -1 : 1);
    int c;
    if (ra == 2) {
        c = a.as_bool() == b.as_bool() ? 0 : (a.as_bool() ? 1 : -1);
    } else if (auto cc = rql_compare(x, a, b)) {
        c = *cc;
    } else {
        int t = to_text(a).compare(to_text(b));
        c = t < 0 ? -1 : (t > 0 ? 1 : 0);
    }
    return desc ? -c : c;
}

class SortStream final : public Stream {
public:
    SortStream(Exec& x, StreamPtr in, std::vector<Eval> keys, std::vector<bool> desc)
        : x_(x), in_(std::move(in)), keys_(std::move(keys)), desc_(std::move(desc)) {}
    bool next(Item& out) override {
        if (!sorted_) {
            sorted_ = true;
            Item it;
            int64_t i = 0;
            while (in_->next(it)) {
                set_scope(x_, it, i++);
                std::vector<Value> kv;
                kv.reserve(keys_.size());
                for (auto& k : keys_) kv.push_back(k(x_));
                rows_.push_back({std::move(it), std::move(kv)});
            }
            x_.check_time();
            std::stable_sort(rows_.begin(), rows_.end(), [this](const Row& a, const Row& b) {
                for (size_t k = 0; k < desc_.size(); ++k) {
                    int c = compare_one(x_, a.keys[k], b.keys[k], desc_[k]);
                    if (c != 0) return c < 0;
                }
                return false;
            });
        }
        if (pos_ >= rows_.size()) return false;
        out = rows_[pos_++].item;
        return true;
    }

private:
    struct Row {
        Item item;
        std::vector<Value> keys;
    };
    Exec& x_;
    StreamPtr in_;
    std::vector<Eval> keys_;
    std::vector<bool> desc_;
    bool sorted_ = false;
    std::vector<Row> rows_;
    size_t pos_ = 0;
};

class FirstStream final : public Stream {
public:
    FirstStream(StreamPtr in, int n) : in_(std::move(in)), n_(n) {}
    bool next(Item& out) override {
        if (count_ >= n_) return false;  // stop pulling upstream (lazy)
        if (!in_->next(out)) return false;
        ++count_;
        return true;
    }

private:
    StreamPtr in_;
    int n_;
    int count_ = 0;
};

class SkipStream final : public Stream {
public:
    SkipStream(StreamPtr in, int n) : in_(std::move(in)), n_(n) {}
    bool next(Item& out) override {
        while (skipped_ < n_) {
            Item it;
            if (!in_->next(it)) return false;
            ++skipped_;
        }
        return in_->next(out);
    }

private:
    StreamPtr in_;
    int n_;
    int skipped_ = 0;
};

class GetStream final : public Stream {
public:
    GetStream(Exec& x, StreamPtr in, Eval eval, std::optional<std::string> suffix)
        : x_(x), in_(std::move(in)), eval_(std::move(eval)), suffix_(std::move(suffix)) {}
    bool next(Item& out) override {
        for (;;) {
            if (sub_ && sub_->next(out)) return true;
            sub_.reset();
            Item it;
            if (!in_->next(it)) return false;
            set_scope(x_, it, i_++);
            Value v = eval_(x_);
            if (v.is_error()) {
                ++x_.getter_errors;
                if (x_.first_getter_error.empty()) x_.first_getter_error = v.error_detail().empty() ? v.error_code() : v.error_detail();
                continue;
            }
            PathPtr p = x_.track_paths && it.path && suffix_ ? path_text(it.path, *suffix_) : nullptr;
            sub_ = std::make_unique<ExpandStream>(x_, std::move(v), std::move(p), true);
        }
    }

private:
    Exec& x_;
    StreamPtr in_;
    Eval eval_;
    std::optional<std::string> suffix_;
    StreamPtr sub_;
    int64_t i_ = 0;
};

struct ValueHash {
    Exec* x;
    size_t operator()(const Value& v) const { return distinct_hash(*x, v); }
};
struct ValueEq {
    Exec* x;
    bool operator()(const Value& a, const Value& b) const { return distinct_equal(*x, a, b); }
};

class DistinctStream final : public Stream {
public:
    DistinctStream(Exec& x, StreamPtr in, Eval key) : x_(x), in_(std::move(in)), key_(std::move(key)) {}
    bool next(Item& out) override {
        Item it;
        while (in_->next(it)) {
            Value k = x_.live(it.value);
            if (key_) {
                set_scope(x_, it, i_);
                k = key_(x_);
            }
            ++i_;
            if (seen_.insert(k).second) {
                out = std::move(it);
                return true;
            }
        }
        return false;
    }

private:
    Exec& x_;
    StreamPtr in_;
    Eval key_;
    int64_t i_ = 0;
    std::unordered_set<Value, ValueHash, ValueEq> seen_{16, ValueHash{&x_}, ValueEq{&x_}};
};

class GroupByStream final : public Stream {
public:
    GroupByStream(Exec& x, StreamPtr in, Eval key) : x_(x), in_(std::move(in)), key_(std::move(key)) {}
    bool next(Item& out) override {
        if (!done_) {
            done_ = true;
            std::unordered_map<Value, size_t, ValueHash, ValueEq> index(16, ValueHash{&x_}, ValueEq{&x_});
            std::vector<std::pair<Value, double>> groups;
            Item it;
            int64_t i = 0;
            while (in_->next(it)) {
                set_scope(x_, it, i++);
                Value k = key_(x_);
                if (k.is_missing() || k.is_unknown()) k = Value::null();
                auto f = index.find(k);
                if (f != index.end()) {
                    groups[f->second].second += 1;
                } else {
                    index.emplace(k, groups.size());
                    groups.emplace_back(k, 1.0);
                }
            }
            auto cols = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"key", "count"});
            for (auto& g : groups) rows_.push_back(Value::row(cols, {g.first, Value::number(g.second)}));
        }
        if (pos_ >= rows_.size()) return false;
        out = Item{rows_[pos_++], nullptr, -1};
        return true;
    }

private:
    Exec& x_;
    StreamPtr in_;
    Eval key_;
    bool done_ = false;
    std::vector<Value> rows_;
    size_t pos_ = 0;
};

struct IdKey {
    const void* a;
    const void* b;
    bool operator==(const IdKey& o) const { return a == o.a && b == o.b; }
};
struct IdHash {
    size_t operator()(const IdKey& k) const { return std::hash<const void*>()(k.a) * 31u ^ std::hash<const void*>()(k.b); }
};

class RecurseStream final : public Stream {
public:
    struct Via {
        Eval eval;
        std::optional<std::string> suffix;
    };
    RecurseStream(Exec& x, StreamPtr in, const Stage& st, std::vector<Via> via)
        : x_(x), in_(std::move(in)), st_(st), via_(std::move(via)) {}

    bool next(Item& out) override {
        if (!init_) {
            init_ = true;
            limit_ = st_.max_depth.value_or(x_.engine.defaults().default_recurse_depth);
            if (limit_ > x_.max_depth) {
                x_.diagnostics.push_back(Diagnostic::warning(
                    "budget-exceeded", "recurse max-depth " + std::to_string(limit_) + " clamped to " + std::to_string(x_.max_depth)));
                limit_ = x_.max_depth;
            }
        }
        for (;;) {
            if (has_pending_) {
                has_pending_ = false;
                expand(pending_);
            }
            if (stack_.empty()) {
                Item it;
                if (!in_->next(it)) return false;
                if (it.value.is_null()) continue;
                stack_.push_back(Frame{it.value, it.path, 0, st_.include_self});
                continue;
            }
            Frame f = std::move(stack_.back());
            stack_.pop_back();
            if (f.value.is_null()) continue;
            const void *a, *b;
            if (f.value.identity(a, b) && !visited_.insert(IdKey{a, b}).second) continue;
            if (f.emit) {
                out = Item{f.value, f.path, f.depth};
                pending_ = std::move(f);
                has_pending_ = true;
                return true;
            }
            expand(f);
        }
    }

private:
    struct Frame {
        Value value;
        PathPtr path;
        int depth;
        bool emit;
    };

    void add_object_children(const Value& v, const PathPtr& path, std::vector<std::pair<Value, PathPtr>>& into) {
        ExpandStream s(x_, v, path, true);
        Item c;
        while (s.next(c)) {
            Value cv = x_.live(c.value.is_entry() ? c.value.entry().value : c.value);  // destroyed children are not followed
            if (cv.is_object()) into.emplace_back(cv, c.path);
        }
    }

    void generic_children(const Value& v, const PathPtr& path, std::vector<std::pair<Value, PathPtr>>& into) {
        if (v.is_list() || v.is_map()) {
            add_object_children(v, path, into);
            return;
        }
        if (!v.is_object()) return;
        const TypeView view = x_.view(v.type());
        for (auto& m : view.implicit_members()) {
            if (!m.owning || !m.type) continue;
            if (m.type->kind == TypeKind::Enum || m.type->kind == TypeKind::Struct) continue;
            if (m.type->kind == TypeKind::Scalar && m.type->scalar != ScalarKind::Any) continue;
            Value cv;
            ++x_.members_read;
            try {
                cv = x_.live(m.getter(v.ptr()));
            } catch (const std::exception& ex) {
                x_.getter_threw(m.name, ex.what());
                continue;
            }
            if (cv.is_null() || cv.is_missing() || cv.is_string()) continue;
            PathPtr p = x_.track_paths && path ? path_member(path, m.name) : nullptr;
            if (cv.is_object()) {
                x_.visit();
                into.emplace_back(cv, p);
            } else if (cv.is_list() || cv.is_map()) {
                add_object_children(cv, p, into);
            }
        }
    }

    void expand(const Frame& f) {
        if (f.depth >= limit_) return;
        if (x_.is_dead(f.value)) return;  // no getters run on a destroyed object
        std::vector<std::pair<Value, PathPtr>> children;
        if (st_.has_via) {
            for (auto& v : via_) {
                x_.current = f.value;
                x_.current_path = f.path;
                x_.current_depth = f.depth;
                Value cv = v.eval(x_);
                PathPtr cp = x_.track_paths && f.path && v.suffix ? path_text(f.path, *v.suffix) : nullptr;
                ExpandStream s(x_, std::move(cv), std::move(cp), true);
                Item c;
                while (s.next(c)) children.emplace_back(c.value, c.path);
            }
        } else {
            generic_children(f.value, f.path, children);
        }
        // Push in reverse so the first child is visited first (pre-order).
        for (size_t k = children.size(); k-- > 0;) stack_.push_back(Frame{children[k].first, children[k].second, f.depth + 1, true});
    }

    Exec& x_;
    StreamPtr in_;
    const Stage& st_;
    std::vector<Via> via_;
    bool init_ = false;
    int limit_ = 8;
    std::vector<Frame> stack_;
    Frame pending_;
    bool has_pending_ = false;
    std::unordered_set<IdKey, IdHash> visited_;
};

class GenericStream final : public Stream {
public:
    GenericStream(Exec& x, StreamPtr in, StageHandler h, std::vector<ArgEvaluator> args, std::map<std::string, ArgEvaluator> named)
        : x_(x), in_(std::move(in)), h_(std::move(h)), args_(std::move(args)), named_(std::move(named)) {}
    bool next(Item& out) override {
        if (!done_) {
            done_ = true;
            std::vector<Value> input;
            Item it;
            while (in_->next(it)) input.push_back(it.value);
            output_ = h_(input, args_, named_);
        }
        if (pos_ >= output_.size()) return false;
        x_.visit();
        out = Item{output_[pos_++], nullptr, -1};
        return true;
    }

private:
    Exec& x_;
    StreamPtr in_;
    StageHandler h_;
    std::vector<ArgEvaluator> args_;
    std::map<std::string, ArgEvaluator> named_;
    bool done_ = false;
    std::vector<Value> output_;
    size_t pos_ = 0;
};

// ------------------------------------------------------------------ building

StreamPtr compile_source(Exec& x, Compiler& c, const Expr& src) {
    auto source_names = [&]() {
        std::vector<std::string> n;
        for (auto& s : x.engine.sources()) n.push_back(s.name);
        return n;
    };
    if (src.kind == ExprKind::Path && !src.on && !src.segments.empty() && src.segments[0].kind == SegmentKind::Member) {
        size_t prefix = 0;
        while (prefix < src.segments.size() && src.segments[prefix].kind == SegmentKind::Member) ++prefix;
        for (size_t len = prefix; len >= 1; --len) {
            std::vector<std::string> parts;
            for (size_t i = 0; i < len; ++i) parts.push_back(src.segments[i].name);
            const SourceRegistration* reg = x.engine.find_source(join(parts, "."));
            if (!reg) continue;
            std::vector<Segment> rest(src.segments.begin() + static_cast<std::ptrdiff_t>(len), src.segments.end());
            std::string rest_text;
            for (auto& s : rest) rest_text += s.kind == SegmentKind::Member ? "." + s.name : s.to_string();
            Eval nav;
            if (!rest.empty()) nav = c.compile(*plan::path(rest));
            return std::make_unique<SourceStream>(x, reg, nav, reg->name + rest_text, false, Eval());
        }
        const std::string& head = src.segments[0].name;
        auto names = source_names();
        auto close = suggest_closest(head, names, 3);
        throw RqlException("unknown-source", "unknown source '" + head + "'",
                           (close.empty() ? std::string() : "did you mean " + join(close, " / ") + "? ") + "sources: " + join(names, " "));
    }
    if (src.kind == ExprKind::Call && !src.on && iequals(src.name, "ref")) {
        Eval ev = c.compile(src);
        return std::make_unique<SourceStream>(x, nullptr, Eval(), "", true, ev);
    }
    throw RqlException("unknown-source", "query must start with a source name (e.g. world) or ref(\"Type#id\")",
                       "sources: " + join(source_names(), " "));
}

StreamPtr apply_stage(Exec& x, Compiler& c, StreamPtr in, const Stage& st) {
    switch (st.kind) {
        case StageKind::Where: return std::make_unique<WhereStream>(x, std::move(in), c.compile(*st.expr));
        case StageKind::Select: {
            auto cols = std::make_shared<std::vector<std::string>>();
            std::vector<Eval> evals;
            for (auto& f : st.fields) {
                cols->push_back(f.name);
                evals.push_back(c.compile(*f.expr));
            }
            return std::make_unique<SelectStream>(x, std::move(in), cols, std::move(evals));
        }
        case StageKind::Sort: {
            std::vector<Eval> keys;
            std::vector<bool> desc;
            for (auto& k : st.keys) {
                keys.push_back(c.compile(*k.expr));
                desc.push_back(k.desc);
            }
            return std::make_unique<SortStream>(x, std::move(in), std::move(keys), std::move(desc));
        }
        case StageKind::First: return std::make_unique<FirstStream>(std::move(in), st.n);
        case StageKind::Skip: return std::make_unique<SkipStream>(std::move(in), st.n);
        case StageKind::Get: return std::make_unique<GetStream>(x, std::move(in), c.compile(*st.expr), path_suffix(*st.expr));
        case StageKind::Distinct: return std::make_unique<DistinctStream>(x, std::move(in), st.expr ? c.compile(*st.expr) : Eval());
        case StageKind::GroupBy: return std::make_unique<GroupByStream>(x, std::move(in), c.compile(*st.expr));
        case StageKind::Recurse: {
            std::vector<RecurseStream::Via> via;
            for (auto& v : st.via) via.push_back({c.compile(*v), path_suffix(*v)});
            return std::make_unique<RecurseStream>(x, std::move(in), st, std::move(via));
        }
        case StageKind::Generic: {
            auto it = x.engine.stages().find(st.op);
            if (it == x.engine.stages().end()) {
                std::vector<std::string> names;
                for (auto& kv : x.engine.stages()) names.push_back(kv.first);
                auto close = suggest_closest(st.op, names, 3);
                throw RqlException("unknown-stage", "unknown stage '" + st.op + "'",
                                   !close.empty() ? "did you mean " + join(close, " / ") + "?" : "registered extension stages: " + join(names, " "));
            }
            Exec* xp = &x;
            auto make = [&](const Expr& e) -> ArgEvaluator {
                Eval ev = c.compile(e);
                return [ev, xp](const Value& item) {
                    Value saved = xp->current;
                    xp->current = item;
                    Value r;
                    try {
                        r = ev(*xp);
                    } catch (...) {
                        xp->current = saved;
                        throw;
                    }
                    xp->current = saved;
                    return r;
                };
            };
            std::vector<ArgEvaluator> args;
            for (auto& a : st.args) args.push_back(make(*a));
            std::map<std::string, ArgEvaluator> named;
            for (auto& n : st.named) named[n.first] = make(*n.second);
            return std::make_unique<GenericStream>(x, std::move(in), it->second, std::move(args), std::move(named));
        }
        default:
            throw RqlException("unsupported", "stage '" + st.op + "' is not supported here");
    }
}

bool last_is_rows(const QueryPlan& p) {
    bool rows = false;
    for (auto& st : p.stages) {
        if (st.kind == StageKind::Select || st.kind == StageKind::GroupBy) rows = true;
        else if (st.kind == StageKind::Get || st.kind == StageKind::Sum || st.kind == StageKind::Min || st.kind == StageKind::Max ||
                 st.kind == StageKind::Avg || st.kind == StageKind::Recurse || st.kind == StageKind::Generic)
            rows = false;
    }
    return rows;
}

std::vector<std::string> last_columns(const QueryPlan& p) {
    for (size_t i = p.stages.size(); i-- > 0;) {
        const Stage& st = p.stages[i];
        if (st.kind == StageKind::Select) {
            std::vector<std::string> cols;
            for (auto& f : st.fields) cols.push_back(f.name);
            return cols;
        }
        if (st.kind == StageKind::GroupBy) return {"key", "count"};
    }
    return {};
}

QueryResult collect(Exec& x, Stream& stream, Encoder& enc) {
    std::vector<Value> collected;
    int64_t total = 0;
    Item it;
    while (stream.next(it)) {
        if (total < x.max_rows) collected.push_back(it.value);
        ++total;
    }
    // Rows only when EVERY item is a projected row (a later stage can mix rows and plain values).
    bool rows = !collected.empty() ? std::all_of(collected.begin(), collected.end(), [](const Value& v) { return v.is_row(); })
                                   : last_is_rows(x.plan);
    QueryResult r;
    if (rows) {
        std::vector<std::string> cols = !collected.empty() ? *collected[0].row().columns : last_columns(x.plan);
        std::vector<std::vector<Value>> values;
        values.reserve(collected.size());
        for (auto& c : collected) values.push_back(c.is_row() ? c.row().values : std::vector<Value>{c});
        r = enc.rows(cols, values);
    } else {
        r = enc.items(collected);
    }
    r.total = total;
    if (total > static_cast<int64_t>(collected.size())) r.truncated = true;
    return r;
}

/// Types statically reachable from the sources' root types through member, element and key types,
/// plus registered subtypes of reachable object types.
std::vector<const TypeDescriptor*> reachable_types(Exec& x, std::vector<std::pair<std::string, const TypeDescriptor*>>* roots_out) {
    std::vector<std::pair<std::string, const TypeDescriptor*>> roots;
    for (auto& s : x.engine.sources()) {
        Value v;
        try {
            v = s.getter();
        } catch (const std::exception&) {
            continue;
        }
        if (v.type()) roots.emplace_back(s.name, v.type());
    }
    std::vector<const TypeDescriptor*> types;
    std::vector<const TypeDescriptor*> queue;
    for (auto& r : roots) queue.push_back(r.second);
    auto registered = x.types.registered();
    for (size_t i = 0; i < queue.size() && types.size() < 1000; ++i) {
        const TypeDescriptor* t = queue[i];
        if (!t || std::find(types.begin(), types.end(), t) != types.end()) continue;
        if (t->kind == TypeKind::List || t->kind == TypeKind::Map) {
            queue.push_back(t->key);
            queue.push_back(t->element);
            continue;
        }
        if (t->kind == TypeKind::Scalar) continue;
        types.push_back(t);
        const TypeView view = x.view(t);
        for (auto& m : view.members()) queue.push_back(m.type);
        if (t->kind == TypeKind::Object)
            for (auto* sub : registered)
                if (sub != t && sub->derives_from(t)) queue.push_back(sub);
    }
    if (roots_out) *roots_out = std::move(roots);
    return types;
}

std::vector<std::string> holders(Exec& x, const std::string& type_name) {
    std::vector<std::pair<std::string, const TypeDescriptor*>> roots;
    std::vector<const TypeDescriptor*> types = reachable_types(x, &roots);
    std::vector<const TypeDescriptor*> targets;
    for (auto* t : types)
        if (t->is_a(type_name)) targets.push_back(t);
    std::vector<std::string> out;
    if (targets.empty()) return out;
    auto holds = [&](const TypeDescriptor* declared) {
        const TypeDescriptor* el = declared && (declared->kind == TypeKind::List || declared->kind == TypeKind::Map) ? declared->element : declared;
        if (!el) return false;
        for (auto* t : targets)
            if (t->derives_from(el)) return true;
        return false;
    };
    auto add = [&](std::string s) {
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
    };
    for (auto& r : roots)
        if (holds(r.second)) add("source " + r.first);
    for (auto* t : types) {
        const TypeView view = x.view(t);
        for (auto& m : view.members()) {
            if (t->base && x.view(t->base).find(m.name)) continue;  // declared on the base
            if (holds(m.type)) add(t->name + "." + m.name);
        }
    }
    if (out.size() > 4) out.resize(4);
    return out;
}

void add_warnings(Exec& x, QueryResult& r, bool only_general) {
    for (auto& d : x.diagnostics)
        if (d.severity == Severity::Warning) r.diagnostics.push_back(d);
    if (x.getter_errors > 0)
        r.diagnostics.push_back(Diagnostic::warning(
            "getter-threw", std::to_string(x.getter_errors) + " member read(s) failed; first: " + x.first_getter_error));
    if (only_general) return;
    for (auto& site : x.is_sites) {
        if (site->matched > 0 || site->evaluated == 0) continue;
        auto h = holders(x, site->type_name);
        std::string hint = !h.empty() ? site->type_name + " objects are reached via " + join(h, ", ")
                                      : "no reachable type is named '" + site->type_name + "'; use rql_schema / describe to list types";
        r.diagnostics.push_back(Diagnostic::warning("no-match",
                                                    "none of the " + std::to_string(site->evaluated) + " tested item(s) is a " +
                                                        site->type_name + " (saw " + join(site->seen, ", ") + ")",
                                                    hint));
    }
    std::vector<std::string> reported;
    auto is_reported = [&](const std::string& n) { return std::find(reported.begin(), reported.end(), n) != reported.end(); };
    // Static check for never-evaluated bare names (short-circuit `and`/`or`, empty streams): a name that
    // no reachable type declares and no select produces is certainly a typo (spec §10).
    bool known_ready = false;
    std::vector<std::string> known;
    for (auto& sp : x.sites) {
        MemberSite& site = *sp;
        if (!site.bare || site.hits > 0 || site.misses > 0 || is_reported(site.name)) continue;
        if (!known_ready) {
            known_ready = true;
            known = {"key", "value", "count", "Count", "Length"};
            for (auto* t : reachable_types(x, nullptr))
            {
                const TypeView view = x.view(t);
                for (auto& m : view.members()) known.push_back(m.name);
            }
            for (auto& st : x.plan.stages)
                if (st.kind == StageKind::Select)
                    for (auto& f : st.fields) known.push_back(f.name);
        }
        bool is_known = false;
        for (auto& k : known)
            if (iequals(k, site.name)) { is_known = true; break; }
        if (is_known) continue;
        reported.push_back(site.name);
        std::vector<std::string> uniq;
        for (auto& k : known)
            if (std::find(uniq.begin(), uniq.end(), k) == uniq.end()) uniq.push_back(k);
        auto close = suggest_closest(site.name, uniq, 3);
        r.diagnostics.push_back(Diagnostic::warning(
            "unknown-member", "no type reachable from the sources has a member '" + site.name + "' (not evaluated)",
            !close.empty() ? "did you mean " + join(close, " / ") + "?" : "if '" + site.name + "' is a value, quote it: \"" + site.name + "\""));
    }
    for (auto& sp : x.sites) {
        MemberSite& site = *sp;
        if (site.hits > 0 || site.misses == 0) continue;
        if (is_reported(site.name)) continue;
        reported.push_back(site.name);
        if (!site.row_columns.empty() && site.miss_types.empty()) {
            r.diagnostics.push_back(Diagnostic::warning(
                "unknown-member", "no column '" + site.name + "' in the projected rows (columns: " + join(site.row_columns, ", ") + ")",
                "select produces rows with only the listed columns; to navigate into objects use the path in the source or `get Path`"));
            continue;
        }
        std::vector<std::string> type_names, candidates;
        for (auto* t : site.miss_types) {
            if (type_names.size() < 4 && std::find(type_names.begin(), type_names.end(), t->name) == type_names.end())
                type_names.push_back(t->name);
            const TypeView view = x.view(t);
            for (auto& m : view.members())
                if (std::find(candidates.begin(), candidates.end(), m.name) == candidates.end()) candidates.push_back(m.name);
        }
        auto close = suggest_closest(site.name, candidates, 3);
        std::string hint;
        if (!close.empty()) hint = "did you mean " + join(close, " / ") + "?";
        else if (!candidates.empty()) {
            std::vector<std::string> first(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(24, candidates.size())));
            hint = "members: " + join(first, " ");
        }
        if (close.empty() && !site.name.empty() && site.name[0] >= 'A' && site.name[0] <= 'Z')
            hint += (hint.empty() ? "" : "; ") + std::string("if '") + site.name + "' is a value, quote it: \"" + site.name + "\"";
        r.diagnostics.push_back(Diagnostic::warning("unknown-member", "no member '" + site.name + "' on " + join(type_names, ", "), hint));
    }
}

QueryResult run_core(Exec& x) {
    Compiler c(x);
    // Pre-compile all stages (binding errors surface before any traversal).
    StreamPtr stream = compile_source(x, c, *x.plan.source);
    const Stage* terminal = nullptr;
    for (auto& st : x.plan.stages) {
        if (st.is_terminal()) {
            terminal = &st;
            break;
        }
        stream = apply_stage(x, c, std::move(stream), st);
    }
    Encoder enc(x);
    if (!terminal) return collect(x, *stream, enc);
    switch (terminal->kind) {
        case StageKind::Count: {
            int64_t n = 0;
            Item it;
            while (stream->next(it)) ++n;
            QueryResult r;
            r.ok = true;
            r.kind = ResultKind::Scalar;
            r.has_value = true;
            r.value = Json(n);
            return r;
        }
        case StageKind::Describe: return run_describe(x, *stream);
        case StageKind::Sum:
        case StageKind::Min:
        case StageKind::Max:
        case StageKind::Avg: {
            Eval sel = c.compile(*terminal->expr);
            std::vector<Value> values;
            Item it;
            int64_t i = 0;
            while (stream->next(it)) {
                set_scope(x, it, i++);
                values.push_back(sel(x));
            }
            QueryResult r;
            r.ok = true;
            r.kind = ResultKind::Scalar;
            r.has_value = true;
            r.value = enc.encode(reduce(x, values, terminal->op));
            return r;
        }
        case StageKind::Set: return run_set(x, c, enc, *stream, *terminal);
        case StageKind::Call: return run_call(x, c, enc, *stream, *terminal);
        default: throw RqlException("unsupported", "stage '" + terminal->op + "' is not supported");
    }
}

QueryResult fail_with(Exec& x, const RqlException& ex) {
    std::vector<Diagnostic> diags;
    diags.push_back(Diagnostic::from(ex));
    for (auto& d : x.diagnostics)
        if (d.severity == Severity::Error && d.code != ex.code()) diags.push_back(d);
    QueryResult r = QueryResult::fail(std::move(diags));
    add_warnings(x, r, true);
    return r;
}

}  // namespace

QueryResult run_pipeline(Exec& x) {
    x.track_paths = x.engine.always_track_paths || plan_uses_meta(x.plan, "path");
    QueryResult result;
    try {
        validate_plan_structure(x.plan);
        result = run_core(x);
    } catch (const RqlException& ex) {
        return fail_with(x, ex);
    } catch (const std::exception& ex) {
        // Host code (source getters, extension stages) or a runtime bug threw outside a member read.
        return QueryResult::fail({Diagnostic::error("internal-error", std::string("internal error: ") + ex.what(),
                                                    "this is an RQL runtime bug or a throwing host callback; please report the query")});
    }
    bool any_error = false;
    for (auto& d : x.diagnostics)
        if (d.severity == Severity::Error) any_error = true;
    if (any_error) {
        std::vector<Diagnostic> errs;
        for (auto& d : x.diagnostics) {
            if (d.severity != Severity::Error) continue;
            bool dup = false;
            for (auto& e : errs)
                if (e.message == d.message) dup = true;
            if (!dup) errs.push_back(d);
        }
        return QueryResult::fail(std::move(errs));
    }
    add_warnings(x, result, false);
    return result;
}

}  // namespace detail
}  // namespace rql
