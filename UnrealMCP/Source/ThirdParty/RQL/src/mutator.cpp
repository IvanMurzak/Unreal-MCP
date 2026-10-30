// Permissioned, all-or-nothing `set` / `call` (spec §8). Mirrors dotnet/Rql.Execution/Mutator.cs.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "exec.hpp"

namespace rql {
namespace detail {

std::string per_item_call_hint(Exec& x, const std::string& method);

namespace {

void check_policy(Exec& x, const std::string& op) {
    if (!x.engine.mutations().enabled)
        throw RqlException("permission-denied", "'" + op + "' is not allowed: this host does not permit mutations");
    if (!x.options.allow_mutation)
        throw RqlException("permission-denied", "'" + op + "' requires a mutation request: run it with " +
                                                    (x.engine.mutate_hint.empty() ? std::string("the mutation tool") : x.engine.mutate_hint));
}

/// A target with the scope its expressions see: its path, depth and position in the stage's input (spec §8).
struct Target {
    Item item;
    int64_t index;
};

/// `$`, `$index`, `$path` and `$depth` in a set value / call argument are the target's (spec §8).
void enter_scope(Exec& x, const Target& t) { set_scope(x, t.item, t.index); }

/// The mutation-limit hint (spec §10). When callers can raise the limit (`argument` non-empty) and the ceiling is above
/// it: raise it to the `matched` count (one all-or-nothing call), or (count unknown: more matched than the ceiling) to
/// the ceiling in batches of the ceiling. Otherwise: batches of `limit`.
std::string mutation_limit_hint(const std::string& op, int limit, std::optional<int> matched, const std::string& argument, int ceiling) {
    const std::string narrow = "narrow the query (where / ref(...))";
    auto batches = [&](int count) {
        std::string n = std::to_string(count);
        return "split the change into batches of " + n + ": \xE2\x80\xA6 | first " + n + " | " + op + " \xE2\x80\xA6, then \xE2\x80\xA6 | skip " + n +
               " | first " + n + " | " + op + " \xE2\x80\xA6 (repeat plain first " + n + " instead when the change removes the items from your where)";
    };
    if (argument.empty() || ceiling <= limit) return narrow + " or " + batches(limit);
    if (matched) {
        std::string c = std::to_string(*matched);
        return "pass " + argument + " = " + c + " (host ceiling " + std::to_string(ceiling) + ") to change all " + c +
               " in one all-or-nothing call (a dry run previews it); or " + narrow;
    }
    return "pass " + argument + " = " + std::to_string(ceiling) + " (the host ceiling) and " + batches(ceiling) + "; or " + narrow;
}

/// The host's batch validation (spec §8): every planned change at once, before any write or call, dry runs too.
void validate_batch(Exec& x, const std::string& op, const std::vector<MutationRecord>& batch) {
    const BatchValidator& validate = x.engine.mutations().validate_batch;
    if (!validate || batch.empty()) return;
    std::string rejected;
    try {
        rejected = validate(batch);
    } catch (const std::exception& ex) {
        rejected = std::string("the batch check threw: ") + ex.what();
    } catch (...) {
        rejected = "the batch check threw";
    }
    if (!rejected.empty())
        throw RqlException("invalid-value", "the host refused this '" + op + "' of " + std::to_string(batch.size()) + " change(s): " + rejected +
                                                "; nothing was changed");
}

/// mutation-limit (spec §8, §10) once `matched` targets passed the limit: counts the rest of the stage's input up to the
/// host ceiling (counting only) so the message states how many items matched, and builds the hint for the host's limit
/// argument.
[[noreturn]] void limit_exceeded(Exec& x, Stream& rest, int64_t matched, const std::string& op, const std::string& matched_text = "objects matched") {
    const MutationPolicy& m = x.engine.mutations();
    const int limit = static_cast<int>(x.max_mutation_targets);
    const int64_t ceiling = std::max<int64_t>(m.hard_max_targets, limit);
    // A budget error while counting propagates: it, not the limit, is what the caller must fix first.
    Item it;
    while (matched <= ceiling && rest.next(it))
        if (!it.value.is_null() && !x.is_dead(it.value)) ++matched;
    const bool ended = matched <= ceiling;
    const std::string op_q = "'" + op + "'";
    std::string message = ended ? std::to_string(matched) + " " + matched_text + " " + op_q + ", more than the limit of " + std::to_string(limit) +
                                      "; nothing was changed"
                                : "more than " + std::to_string(ceiling) + " " + matched_text + " " + op_q + " (the limit is " + std::to_string(limit) +
                                      "); nothing was changed";
    std::string hint = m.limit_hint ? m.limit_hint(op, limit) : std::string();
    if (hint.empty()) {
        std::string argument = x.limit_argument();
        hint = mutation_limit_hint(op, limit, ended ? std::optional<int>(static_cast<int>(matched)) : std::nullopt, argument, static_cast<int>(ceiling));
    }
    throw RqlException("mutation-limit", message, hint);
}

/// `method`: for `call`, the method (hints); `per_item`: a source-qualified call, whose items are only the scope of its
/// arguments, so any value may be one (spec §8).
std::vector<Target> targets(Exec& x, Stream& stream, const std::string& op, const std::string& method = std::string(), bool per_item = false) {
    std::vector<Target> list;
    Item it;
    int64_t index = -1;
    while (stream.next(it)) {
        ++index;  // skipped null/destroyed items keep their positions: $index is the position in the stage's input
        // Null and destroyed items are skipped (spec §13: a destroyed object reads as null).
        if (it.value.is_null() || x.is_dead(it.value)) continue;
        if (per_item) {
            list.push_back({it, index});
            if (static_cast<int64_t>(list.size()) > x.max_mutation_targets) limit_exceeded(x, stream, static_cast<int64_t>(list.size()), op, "items reached");
            continue;
        }
        if (it.value.is_row())
            throw RqlException("invalid-plan", "'" + op + "' needs objects, not projected rows; remove 'select' before '" + op + "'");
        if (it.value.is_entry()) {
            if (op != "set" || !it.value.entry().origin)
                throw RqlException("invalid-plan",
                                   op == "set" ? "'set' cannot target these map entries (they do not come from a map it can write to); set the map member "
                                                 "with a key path instead (Stats[\"key\"] = ...)"
                                               : "'" + op + "' cannot target map entries; call the method on the values instead: ... | get Value | " + op + " ...");
            list.push_back({it, index});  // set Value... writes through the entry's map (spec §8)
            if (static_cast<int64_t>(list.size()) > x.max_mutation_targets) limit_exceeded(x, stream, static_cast<int64_t>(list.size()), op);
            continue;
        }
        const Value& v = it.value;
        bool handle = false;
        if (v.is_struct()) {
            const TypeView tv = x.view(v.type());
            handle = tv.writes_through();  // a handle to shared state: its copies write through (spec §13)
        }
        if ((v.is_struct() && !handle) || v.is_number() || v.is_string() || v.is_bool() || v.is_enum())
            throw RqlException("invalid-plan",
                               "'" + op + "' needs objects with identity; '" + type_name_of(v) + "' items are value copies, so the change would be lost",
                               !method.empty() ? per_item_call_hint(x, method)
                                               : "set the member from its owner instead, e.g. objects | where ... | set Transform.Position.x = 0");
        list.push_back({it, index});
        if (static_cast<int64_t>(list.size()) > x.max_mutation_targets) limit_exceeded(x, stream, static_cast<int64_t>(list.size()), op);
    }
    x.count = index + 1;  // $count: the stage's input, skipped items included, so $index runs 0 ... $count - 1
    return list;
}

std::string raw_label(const std::vector<Segment>& segs) {
    std::string s;
    for (size_t i = 0; i < segs.size(); ++i) {
        const Segment& seg = segs[i];
        s += seg.kind == SegmentKind::Member ? (i == 0 ? "" : ".") + seg.name : seg.to_string();
    }
    return s;
}

std::string label_of(const Expr& p) { return raw_label(p.segments); }

/// A set target only replaces an existing element; adding one means assigning the whole list.
std::string list_grow_hint(const std::string& label, const Segment& seg) {
    size_t at = label.rfind(seg.to_string());
    std::string list = at != std::string::npos && at > 0 ? label.substr(0, at) : "List";
    return "an indexed set replaces an existing element only; to add elements assign the whole list: set " + list + " = append(" + list +
           ", value), set " + list + " = insert(" + list + ", " + std::to_string(seg.index) + ", value) or set " + list + " = [ ... ]";
}

struct Slot {
    enum Kind { Member, ListIndex, MapKey } kind = Member;
    const MemberDescriptor* member = nullptr;
    const TypeDescriptor* owner = nullptr;
    const MemberDescriptor* id = nullptr;  // identity member of the owner's view (always read-only)
    int64_t index = 0;
    Value key;
    const TypeDescriptor* type = nullptr;
    bool nullable = false;

    size_t idx(const Value& c) const {
        int64_t n = static_cast<int64_t>(c.list().size());
        return static_cast<size_t>(index < 0 ? n + index : index);
    }

    Value get(const Value& c) const {
        switch (kind) {
            case Member: return member->getter(c.ptr());
            case ListIndex: return c.list().at(idx(c));
            case MapKey: {
                Value out;
                return c.map().find(key, out) == MapLookup::Found ? out : Value::missing();
            }
        }
        return Value::missing();
    }

    /// Adapter validation of a coerced value before any write (members, virtual-map entries): reason or "".
    std::string validate(const Value& c, const Value& v) const {
        if (kind == Member && member->validate) return member->validate(c.ptr(), v);
        if (kind == MapKey) return c.map().validate_entry(key, v);
        return {};
    }

    void set(const Value& c, const Value& v) const {
        switch (kind) {
            case Member: member->setter(c.ptr(), v); break;
            case ListIndex: c.list().set(idx(c), v); break;
            case MapKey:
                if (v.is_missing()) c.map().erase(key);
                else c.map().set(key, v);
                break;
        }
    }
};

/// top: the segment is read on the target itself, so its unknown-member hint may suggest filtering the stream.
Slot slot_of(Exec& x, const Value& container, const Segment& seg, const std::string& label, bool top = false) {
    Slot s;
    if (container.is_map()) {
        if (!container.map().writable()) throw RqlException("readonly-member", "'" + label + "': map " + friendly_type_of(container) + " is read-only");
        if (seg.kind == SegmentKind::Member && iequals(seg.name, "Count"))
            throw RqlException("readonly-member", "'" + label + "': Count of a map is read-only (use [\"Count\"] to address a key literally named Count)");
        Value key;
        if (seg.kind == SegmentKind::Member) {
            key = Value::string(seg.name);
            // Writes address an existing key case-insensitively, like reads do (spec §3); new keys keep the given spelling.
            Value tmp;
            if (container.map().find(key, tmp) == MapLookup::NotFound) {
                container.map().for_each([&](const Value& k, const Value&) {
                    if (k.is_string() && iequals(k.as_string(), seg.name)) {
                        key = k;
                        return false;
                    }
                    return true;
                });
            }
        }
        else if (seg.kind == SegmentKind::Key) key = seg.key.type == Literal::Type::String ? Value::string(seg.key.s) : Value::number(seg.key.n);
        else if (seg.kind == SegmentKind::Index) key = Value::number(static_cast<double>(seg.index));
        if (key.is_missing() || !container.map().accepts_key(key)) throw RqlException("coercion-failed", "invalid map key in '" + label + "'");
        s.kind = Slot::MapKey;
        s.key = key;
        const TypeDescriptor* mt = container.map().type();
        s.type = mt ? mt->element : nullptr;
        s.nullable = mt ? mt->element_nullable : true;
        if (const TypeDescriptor* et = container.map().entry_type(key)) s.type = et;  // mixed-type virtual maps (spec §13)
        return s;
    }
    if (container.is_list()) {
        if (seg.kind != SegmentKind::Index)
            throw RqlException("invalid-plan", "'" + label + "': lists can only be indexed ([i]) in set targets");
        if (!container.list().writable()) throw RqlException("readonly-member", "'" + label + "': collection is not writable by index");
        int64_t n = static_cast<int64_t>(container.list().size());
        int64_t i = seg.index < 0 ? n + seg.index : seg.index;
        if (i < 0 || i >= n)
            throw RqlException("invalid-plan",
                               "'" + label + "': index " + std::to_string(seg.index) + " out of range (count " + std::to_string(n) + ")",
                               list_grow_hint(label, seg));
        s.kind = Slot::ListIndex;
        s.index = seg.index;
        const TypeDescriptor* lt = container.list().type();
        s.type = lt ? lt->element : nullptr;
        s.nullable = lt ? lt->element_nullable : true;
        return s;
    }
    std::string type_name = type_name_of(container);
    std::string name;
    if (seg.kind == SegmentKind::Member) name = seg.name;
    else if (seg.kind == SegmentKind::Key && seg.key.type == Literal::Type::String) name = seg.key.s;
    else throw RqlException("invalid-plan", "'" + label + "': invalid segment " + seg.to_string() + " on " + type_name);
    const TypeDescriptor* t = (container.is_object() || container.is_struct()) ? container.type() : nullptr;
    const std::optional<TypeView> tv = t ? std::optional<TypeView>(x.view(t)) : std::nullopt;
    const MemberDescriptor* m = tv ? tv->find(name) : nullptr;  // hidden members are unwritable (spec §13)
    if (!m) {
        std::vector<std::string> names;
        if (tv)
            for (auto& mm : tv->members()) names.push_back(mm.name);
        std::vector<const TypeDescriptor*> except;
        if (t) except.push_back(t);
        throw RqlException("unknown-member", "no member '" + name + "' on " + type_name,
                           member_hint(name, names, top ? types_declaring(x, name, except) : std::vector<std::string>(), nested_members(x, except)));
    }
    s.kind = Slot::Member;
    s.member = m;
    s.owner = t;
    s.id = tv->id();
    s.type = m->type;
    s.nullable = m->nullable;
    return s;
}

Value safe_get(const Slot& slot, const Value& container, const std::string& label) {
    try {
        return slot.get(container);
    } catch (const std::exception& ex) {
        throw RqlException("getter-threw", "reading '" + label + "' failed: " + ex.what());
    }
}

/// Member slots must be writable and allowed by policy; list/map slots were checked by slot_of.
void check_writable(Exec& x, const Slot& slot, const std::string& label, const Value& root, const std::string& what) {
    if (slot.kind != Slot::Member) return;
    if (!member_writable(*slot.member, slot.id))
        throw RqlException("readonly-member", "'" + label + "' on " + x.ref_of(root) + " is not writable" + what + " (read-only member)");
    auto& policy = x.engine.mutations().can_write;
    if (policy && !policy(*slot.owner, *slot.member))
        throw RqlException("permission-denied", "'" + label + "' on " + x.ref_of(root) + " is not writable" + what + " (denied by host policy)");
}

/// Whether a container read through `slot` is a copy that must be assigned back after writing into it:
/// members whose getter returns a copy, and value types unless they are write-through handles (spec §13).
bool needs_write_back(Exec& x, const Slot& slot, const Value& container) {
    if (slot.kind == Slot::Member && slot.member->copy_on_read) return true;
    if (!container.is_struct()) return false;
    const TypeView tv = x.view(container.type());
    return !tv.writes_through();
}

/// The value read through a copy-on-read member, as a private writable copy (lists / maps; structs are
/// copies already), so writes into it can be assigned back through the member's setter.
Value read_through(const Slot& slot, const Value& next) {
    if (slot.kind != Slot::Member || !slot.member->copy_on_read) return next;
    return owned_copy(next, true);
}

/// Resolve a set target; `owner` is the object whose state changes: the last reference object on the
/// path (walking up through containers that are written back into their parents).
Slot resolve(Exec& x, const Value& root, const std::vector<Segment>& segs, Value& container, Value& owner) {
    std::string label = raw_label(segs);
    Value cur = root;
    std::vector<std::pair<Slot, Value>> chain;
    for (size_t i = 0; i < segs.size(); ++i) {
        Slot slot = slot_of(x, cur, segs[i], label, i == 0);
        if (i + 1 == segs.size()) {
            // Every copy on the way (value type or copy-on-read member) is written back into its parent:
            // those parent slots must be writable and allowed too.
            for (size_t j = chain.size(); j > 0 && needs_write_back(x, chain[j - 1].first, chain[j - 1].second); --j)
                check_writable(x, chain[j - 1].first, label, root,
                               ": '" + segs[j - 1].to_string() + "' is a copy (value type or copy-on-read member) written back through a non-writable member");
            container = cur;
            // The owner (what hosts mark dirty / record for undo) is the nearest enclosing OBJECT on the path:
            // never a list, map (virtual-map views included) or struct (write-through handles included).
            owner = root;
            for (size_t j = chain.size(); j-- > 0;)
                if (chain[j].second.is_object()) {
                    owner = chain[j].second;
                    break;
                }
            return slot;
        }
        // A destroyed intermediate object reads as null: the path cannot be written (nothing changes).
        Value next = x.live(read_through(slot, safe_get(slot, cur, label)));
        if (next.is_null() || next.is_missing())
            throw RqlException("coercion-failed", "cannot set '" + label + "': '" + segs[i].to_string() + "' is null on " + x.ref_of(root));
        chain.emplace_back(slot, next);
        cur = next;
    }
    throw RqlException("invalid-plan", "empty set target");
}

/// Navigate fresh, write, then write back modified struct copies up to the first reference object.
void assign(Exec& x, const Value& root, const std::vector<Segment>& segs, const Value& value) {
    std::string label = raw_label(segs);
    std::vector<Value> containers{root};
    std::vector<Slot> slots;
    Value cur = root;
    for (size_t i = 0; i < segs.size(); ++i) {
        Slot slot = slot_of(x, cur, segs[i], label);
        slots.push_back(slot);
        if (i + 1 == segs.size()) break;
        cur = read_through(slot, slot.get(cur));
        containers.push_back(cur);
    }
    slots.back().set(containers.back(), value);
    for (size_t j = containers.size() - 1; j >= 1; --j) {
        if (!needs_write_back(x, slots[j - 1], containers[j])) break;
        slots[j - 1].set(containers[j - 1], containers[j]);
    }
}

struct PlannedWrite {
    Value item;  // where the write navigates from: the target, or the map of a streamed entry (write_root)
    const std::vector<Segment>* segments;
    std::string label;
    Value old_value;
    Value new_value;
    Value owner;
    Value target;        // the stream item (host records)
    std::string ref;     // the receipt's $ref: the target's, or for an entry the owner of the change (spec §8)
    std::shared_ptr<const std::vector<Segment>> owned;  // rewritten entry segments (`segments` points into it)
    Value planned;  // the coerced value the query asked for (new_value becomes the read-back after the write)
};

/// Where a set target writes (spec §8): for a map entry streamed from a map, `Value.p` (or `p`, which reads through to
/// the value like member access) rewritten to `prefix[key].p` from the entry's origin root — the ordinary write path from
/// the object the map was read from.
std::shared_ptr<const std::vector<Segment>> entry_segments(const Value& entry, const std::vector<Segment>& segs, const std::string& label) {
    if (is_member(segs[0], "key"))
        throw RqlException("readonly-member", "'" + label + "': a map entry's key cannot be set", "keys are fixed: set the value (set Value... = ...) or assign the whole map");
    const Value& key = entry.entry().key;
    if (!key.is_string() && !key.is_number())
        throw RqlException("invalid-plan", "'" + label + "': this runtime writes through map entries with string or number keys only",
                           "set the map member with a key path instead (Map[\"key\"] = ...)");
    auto out = std::make_shared<std::vector<Segment>>(entry.entry().origin->prefix);
    out->push_back(Segment::for_key(key.is_number() ? Literal::number(key.as_number()) : Literal::string(key.as_string())));
    out->insert(out->end(), segs.begin() + (is_member(segs[0], "value") ? 1 : 0), segs.end());
    return out;
}

/// Runs f; the failure message of any exception (std or not), nullopt on success. Host callbacks
/// (setters, methods, hooks) must never leak an exception past the engine.
template <class F>
std::optional<std::string> capture_failure(F&& f) {
    try {
        f();
        return std::nullopt;
    } catch (const std::exception& ex) {
        return std::string(ex.what());
    } catch (...) {
        return std::string("non-standard exception");
    }
}

/// A host write hook (never on dry-run); nullopt = success or no hook, else the failure message.
std::optional<std::string> run_hook(Exec& x, const MutationHook& hook, const std::vector<MutationRecord>& records) {
    if (!hook || x.options.dry_run || records.empty()) return std::nullopt;  // nothing to report
    return capture_failure([&] { hook(records); });
}

bool has_hooks(Exec& x) {
    const MutationPolicy& p = x.engine.mutations();
    return !x.options.dry_run && (p.before_write || p.committed);
}

/// Veto point after planning, before any write: a failing hook → permission-denied, nothing changed.
void before_write(Exec& x, const std::vector<MutationRecord>& records) {
    if (auto failure = run_hook(x, x.engine.mutations().before_write, records))
        throw RqlException("permission-denied", "the host rejected the change before anything was written: " + *failure);
}

/// Notification after the writes succeeded: a failing hook → hook-threw warning, writes stay applied.
void committed(Exec& x, const std::vector<MutationRecord>& records) {
    if (auto failure = run_hook(x, x.engine.mutations().committed, records))
        x.diagnostics.push_back(Diagnostic::warning("hook-threw", "host commit hook threw: " + *failure + "; the writes were applied"));
}

/// The written member's value after the writes, navigated fresh (what the host stored, spec §8); the planned
/// value plus one getter-threw warning when it cannot be read.
Value read_back(Exec& x, const PlannedWrite& w) {
    std::string failure;
    try {
        Value cur = w.item;
        const std::vector<Segment>& segs = *w.segments;
        for (size_t i = 0;; ++i) {
            Slot slot = slot_of(x, cur, segs[i], w.label);
            Value v = slot.get(cur);
            if (i + 1 == segs.size()) return snapshot(v);
            if (v.is_null() || v.is_missing()) return w.new_value;
            cur = read_through(slot, v);  // a private copy of a copy-on-read value, as when the write was planned
        }
    } catch (const std::exception& ex) {
        failure = ex.what();
    } catch (...) {
        failure = "non-standard exception";
    }
    x.warn_once("getter-threw", "reading back '" + w.label + "' after the write failed (" + failure + "); the receipt shows the planned value",
                "read-back");
    return w.new_value;
}

std::optional<Value> coerce_impl(Exec& x, const Value& v, const TypeDescriptor* target, bool nullable, std::string& why, bool probe, int& depth,
                                 const Value& outer);

/// The literal kind of `type` (spec §4.5, §13): the host's literal_kind_of, else the adapters (most specific first), else
/// Engine for a type with a stable_id or is_alive adapter and Value for any other; then the host's
/// can_construct_from_literal (false -> Engine, true -> an Engine type becomes Value).
LiteralKind literal_kind(Exec& x, const TypeDescriptor* type) {
    // The hooks answer per type and cannot change within one execution: ask them once.
    auto cached = x.literal_kinds.find(type);
    if (cached != x.literal_kinds.end()) return cached->second;
    const MutationPolicy& m = x.engine.mutations();
    std::optional<LiteralKind> kind;
    if (m.literal_kind_of) kind = m.literal_kind_of(*type);
    if (!kind)
        for (const TypeAdapter* ad : x.adapters_for(type, Capability::LiteralKind))
            if ((kind = ad->literal_kind(*type))) break;
    LiteralKind k = kind ? *kind
                         : (x.adapters_for(type, Capability::StableId).empty() && x.adapters_for(type, Capability::IsAlive).empty() ? LiteralKind::Value
                                                                                                                                     : LiteralKind::Engine);
    if (m.can_construct_from_literal) {
        if (std::optional<bool> decided = m.can_construct_from_literal(*type))
            k = !*decided ? LiteralKind::Engine : k == LiteralKind::Engine ? LiteralKind::Value : k;
    }
    x.literal_kinds.emplace(type, k);
    return k;
}

/// Whether a factory (the host's or an adapter's) may build `type`: a probe then needs no create().
bool has_factory(Exec& x, const TypeDescriptor* type) {
    return static_cast<bool>(x.engine.mutations().create_for_literal) || !x.adapters_for(type, Capability::Factory).empty();
}

/// The value a factory builds for a literal of `type` (host first, then adapters); nullopt: none answered.
std::optional<Value> create_for_literal(Exec& x, const TypeDescriptor* type, const Value& outer) {
    // A null / missing answer defers to the next factory, like no answer.
    auto answered = [](std::optional<Value>& m) {
        if (m && (m->is_null() || m->is_missing())) m.reset();
        return m.has_value();
    };
    std::optional<Value> made;
    if (const auto& host = x.engine.mutations().create_for_literal) {
        made = host(*type, outer);
        if (answered(made)) return made;
    }
    for (const TypeAdapter* ad : x.adapters_for(type, Capability::Factory)) {
        made = ad->create_for_literal(*type, outer);
        if (answered(made)) return made;
    }
    return std::nullopt;
}

/// The advice ending the engine-object refusal (spec §4.5): the host's for the type, else "pass an existing one".
std::string refusal_advice(Exec& x, const TypeDescriptor* type) {
    if (const auto& hint = x.engine.mutations().literal_refusal_hint) {
        std::string h = hint(*type);
        if (!h.empty()) return h;
    }
    return "pass an existing one (e.g. ref(\"\xE2\x80\xA6\"))";
}

/// Stands for the object a probed literal would build (nothing is constructed), so its nested values have an outer.
const Value& probe_outer() {
    static const Value placeholder = Value::boolean(true);
    return placeholder;
}

/// Build `target` (object or struct) from an object literal (spec §4.5): concrete type (`"$type"`),
/// construction, assignment of the named members (coerced recursively and validated). A `probe` checks the same rules
/// without constructing, assigning or validating anything (its success value is a placeholder).
std::optional<Value> construct(Exec& x, const Value& literal, const TypeDescriptor* target, std::string& why, bool probe, int& depth,
                               const Value& outer) {
    depth = 0;
    const TypeDescriptor* concrete = target;
    Value type_name;
    if (literal.map().find(Value::string("$type"), type_name) == MapLookup::Found) {
        if (!type_name.is_string()) {
            why = "\"$type\" must be a type name string";
            return std::nullopt;
        }
        const std::string& name = type_name.as_string();
        std::vector<const TypeDescriptor*> hits = x.subtypes_named(target, name);
        // A name no registered type matches goes to the host's lazy lookup (spec §13 "Subtype lookup").
        if (const auto& resolve = x.engine.mutations().resolve_subtype; hits.empty() && resolve) {
            const TypeDescriptor* found = nullptr;
            if (auto failure = capture_failure([&] { found = resolve(*target, name); })) {
                why = "resolving \"$type\" '" + name + "' failed: " + *failure;
                return std::nullopt;
            }
            if (found) {
                if (found->kind != target->kind || !found->derives_from(target)) {
                    why = "the host resolved '" + name + "' to " + found->name + ", which does not derive from " + target->friendly_name();
                    return std::nullopt;
                }
                hits.push_back(found);
            }
        }
        if (hits.empty()) {
            why = "no concrete type named '" + name + "' derives from " + target->friendly_name();
            return std::nullopt;
        }
        if (hits.size() > 1) {
            why = "'" + name + "' is ambiguous";
            return std::nullopt;
        }
        concrete = hits[0];
    }
    // Engine objects are never built from literals (spec §4.5), constructible or not: only existing ones can be passed.
    const LiteralKind kind = literal_kind(x, concrete);
    if (kind == LiteralKind::Engine) {
        why = concrete->name + " is an engine object and cannot be built from an object literal; " + refusal_advice(x, concrete);
        return std::nullopt;
    }
    // An instanced subobject is built for the object that owns it (spec §4.5).
    if (kind == LiteralKind::Instanced && (outer.is_null() || outer.is_missing())) {
        why = concrete->name + " is an instanced subobject: it is built only for the object that owns it (write it to a member of that object or pass it to a call)";
        return std::nullopt;
    }
    // A factory builds first (never in a probe, where it counts as constructible), then the type's own create().
    const bool factory = has_factory(x, concrete);
    std::optional<Value> made;
    if (!probe && factory) {
        if (auto failure = capture_failure([&] { made = create_for_literal(x, concrete, outer); })) {
            why = "building " + concrete->name + " failed: " + *failure;
            return std::nullopt;
        }
        if (made && !is_instance_of(*made, concrete))
            throw std::logic_error("the literal factory built " + type_name_of(*made) + ", not " + concrete->name);
    }
    if (!made && !concrete->create && !(probe && factory)) {
        why = "cannot construct " + concrete->friendly_name() + " (register it constructible(), or add an adapter that coerces object literals)";
        return std::nullopt;
    }
    std::shared_ptr<void> data;
    Value instance;
    const bool host_built = made.has_value();
    if (made) {
        instance = *made;
    } else if (!probe) {
        data = concrete->create();
        instance = concrete->kind == TypeKind::Struct ? Value::structure(concrete, data) : Value::object(concrete, data.get(), data);
    }
    // Values nested in an object literal belong to the object it builds; a struct passes its own outer on.
    const Value& inner = concrete->kind == TypeKind::Object ? (probe ? probe_outer() : instance) : outer;
    const TypeView view = x.view(concrete);
    // Members are looked up on `concrete`: a factory that built a subtype hands them the concrete base's pointer.
    void* self = probe ? nullptr : instance.type() == concrete || !instance.type() ? instance.ptr() : instance.type()->upcast(instance.ptr(), concrete);
    bool ok = true;
    literal.map().for_each([&](const Value& k, const Value& v) {
        if (k.is_string() && k.as_string() == "$type") return true;
        const std::string name = to_text(k);
        const MemberDescriptor* m = view.find(name);
        depth = 1;
        if (!m) {
            why = "no member '" + name + "' on " + concrete->name;
        } else if (!view.member_writable(*m)) {
            why = "member '" + m->name + "' of " + concrete->name + " is read-only";
        } else if (x.engine.mutations().can_write && !x.engine.mutations().can_write(*concrete, *m)) {
            throw RqlException("permission-denied",
                               "member '" + m->name + "' of " + concrete->name + " may not be written (denied by host policy)");
        } else {
            std::string w;
            int d = 0;
            auto mv = coerce_impl(x, v, m->type, m->nullable, w, probe, d, inner);
            if (!mv) {
                why = "'" + m->name + "': " + w;
                depth = d + 1;
            } else if (probe) {
                return true;
            } else {
                std::string rejected = m->validate ? m->validate(self, *mv) : std::string();
                if (!rejected.empty()) {
                    why = "'" + m->name + "': " + rejected;
                } else {
                    try {
                        m->setter(self, *mv);
                        return true;
                    } catch (const std::exception& ex) {
                        why = "'" + m->name + "': " + ex.what();
                    } catch (...) {
                        why = "'" + m->name + "': setter threw";
                    }
                }
            }
        }
        ok = false;
        return false;
    });
    if (!ok) return std::nullopt;
    depth = 0;
    if (probe) return Value::null();
    // Engine-built objects are retained by the engine (raw links may reach them); host-factory ones are host-owned.
    if (instance.is_object() && !concrete->host_owned && !host_built) x.constructed.push_back(data);
    return instance;
}

std::optional<Value> coerce_impl(Exec& x, const Value& v, const TypeDescriptor* target, bool nullable, std::string& why, bool probe, int& depth,
                                 const Value& outer) {
    depth = 0;
    if (v.is_absent()) {
        why = "value is " + describe_value(v);
        return std::nullopt;
    }
    if (v.is_null()) {
        if (nullable || !target || (target->kind == TypeKind::Scalar && target->scalar == ScalarKind::Any)) return Value::null();
        why = "null is not allowed for " + target->friendly_name();
        return std::nullopt;
    }
    if (!target || (target->kind == TypeKind::Scalar && target->scalar == ScalarKind::Any)) return v;
    // Values already of the target type bypass adapters.
    if (is_instance_of(v, target)) return v;
    const bool nominal = target->kind == TypeKind::Struct || target->kind == TypeKind::Object || target->kind == TypeKind::Enum;
    for (const TypeAdapter* ad : x.adapters_for(target, Capability::Coerce)) {
        Value adapted;
        std::string reason;
        switch (ad->coerce(v, *target, adapted, reason)) {
            case AdapterOutcome::Ok:
                if (nominal && !adapted.is_null() && !is_instance_of(adapted, target))
                    throw std::logic_error("adapter " + ad->name + " coerced to " + type_name_of(adapted) + ", not " + target->name);
                return adapted;
            case AdapterOutcome::Failed:
                why = reason.empty() ? "rejected by " + ad->name : reason;
                return std::nullopt;
            case AdapterOutcome::NotHandled: break;
        }
    }
    // The host's text form for struct / object types no adapter handled (spec §8).
    const auto& coerce_text = x.engine.mutations().coerce_text;
    if (coerce_text && v.is_string() && (target->kind == TypeKind::Struct || target->kind == TypeKind::Object)) {
        std::string reason;
        std::optional<Value> parsed;
        if (auto failure = capture_failure([&] { parsed = coerce_text(*target, v.as_string(), reason); })) {
            why = *failure;
            return std::nullopt;
        }
        if (!reason.empty()) {
            why = reason;
            return std::nullopt;
        }
        if (parsed && !parsed->is_null() && !parsed->is_missing()) {
            if (!is_instance_of(*parsed, target)) throw std::logic_error("the host's text coercion returned " + type_name_of(*parsed) + ", not " + target->name);
            return parsed;
        }
    }
    if (v.is_map() && v.map().object_literal()) {
        // Object literals (spec §4.5): maps are built entry by entry below; objects / structs are constructed.
        if (target->kind == TypeKind::Object || target->kind == TypeKind::Struct) return construct(x, v, target, why, probe, depth, outer);
        if (target->kind != TypeKind::Map) {
            why = "an object literal cannot become " + target->friendly_name();
            return std::nullopt;
        }
    }
    switch (target->kind) {
        case TypeKind::Scalar:
            switch (target->scalar) {
                case ScalarKind::Bool:
                    if (v.is_bool()) return v;
                    why = "expected true or false";
                    return std::nullopt;
                case ScalarKind::String:
                    if (v.is_string()) return v;
                    why = "expected a string";
                    return std::nullopt;
                case ScalarKind::Integer:
                case ScalarKind::Single:
                case ScalarKind::Double: {
                    double d;
                    if (!try_number(v, d)) {
                        why = "expected a number";
                        return std::nullopt;
                    }
                    if (target->scalar == ScalarKind::Integer) {
                        if (std::floor(d) != d) {
                            why = "expected an integer";
                            return std::nullopt;
                        }
                        if (d < target->int_min || d > target->int_max) {
                            why = "out of range for " + target->friendly_name();
                            return std::nullopt;
                        }
                    }
                    if (target->scalar == ScalarKind::Single) d = to_single(d);
                    return Value::number(d, target);
                }
                default: return v;
            }
        case TypeKind::Enum: {
            if (v.is_string()) {
                // Enum name resolution (spec §8): exact, qualified, case-insensitive, entry prefix; ambiguity refused.
                TypeDescriptor::EnumMatch m = target->enum_resolve(v.as_string());
                if (m.status == TypeDescriptor::EnumMatch::Found && target->enum_defined(m.value)) return Value::enumeration(target, m.value);
                why = m.status == TypeDescriptor::EnumMatch::Found ? target->enum_expected() : m.why;
                return std::nullopt;
            }
            double d;
            if (try_number(v, d) && std::floor(d) == d && target->enum_defined(static_cast<int64_t>(d)))
                return Value::enumeration(target, static_cast<int64_t>(d));
            why = target->enum_expected();
            return std::nullopt;
        }
        case TypeKind::Object:
        case TypeKind::Struct:  // instances returned above
            why = "expected " + target->friendly_name();
            return std::nullopt;
        case TypeKind::List: {
            if (!v.is_list()) {
                why = "expected " + target->friendly_name();
                return std::nullopt;
            }
            std::vector<Value> items;
            auto& l = v.list();
            size_t n = l.size();
            for (size_t i = 0; i < n; ++i) {
                std::string w;
                int d = 0;
                auto c = coerce_impl(x, l.at(i), target->element, target->element_nullable, w, probe, d, outer);
                if (!c) {
                    why = "element: " + w;
                    depth = d + 1;
                    return std::nullopt;
                }
                if (!probe) items.push_back(*c);
            }
            if (probe) return Value::null();
            return Value::list_of(std::move(items));
        }
        case TypeKind::Map: {
            if (!v.is_map()) {
                why = "expected " + target->friendly_name();
                return std::nullopt;
            }
            std::vector<std::pair<Value, Value>> entries;
            bool ok = true;
            v.map().for_each([&](const Value& k, const Value& val) {
                std::string w;
                int d = 0;
                auto c = coerce_impl(x, val, target->element, target->element_nullable, w, probe, d, outer);
                if (!c) {
                    why = "value: " + w;
                    depth = d + 1;
                    ok = false;
                    return false;
                }
                if (!probe) entries.emplace_back(k, *c);
                return true;
            });
            if (!ok) return std::nullopt;
            if (probe) return Value::null();
            return Value::map(std::make_shared<OwnedMap>(target, std::move(entries)));
        }
    }
    why = "expected " + target->friendly_name();
    return std::nullopt;
}

}  // namespace

std::optional<Value> coerce(Exec& x, const Value& v, const TypeDescriptor* target, bool nullable, std::string& why, const Value& outer) {
    int depth = 0;
    return coerce_impl(x, v, target, nullable, why, false, depth, outer);
}

bool can_coerce(Exec& x, const Value& v, const TypeDescriptor* target, bool nullable, std::string& why, int& depth, const Value& outer) {
    return coerce_impl(x, v, target, nullable, why, true, depth, outer).has_value();
}

namespace {

/// A value as the query wrote it (99, "Tag", true), for invalid-value messages; other kinds by description.
std::string value_text(const Value& v) {
    if (v.is_null()) return "null";
    if (v.is_string()) return "\"" + v.as_string() + "\"";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_number()) return format_double(v.as_number());
    return describe_value(v);
}

}  // namespace

/// "only A, B have X: filter first, e.g. ... | where $ is A | stage" (spec §10 mixed streams).
std::string filter_hint(const std::vector<std::string>& having, const std::string& what, const std::string& stage) {
    return "only " + join(having, ", ") + (having.size() == 1 ? " has " : " have ") + what + ": filter first, e.g. \xE2\x80\xA6 | where $ is " +
           having[0] + " | " + stage;
}

/// A set target member that some targets lack while others have it (a mixed stream): unknown-member naming the types that
/// have it, so the agent filters to them (spec §10). All other cases are reported per target by the slot resolution.
void check_mixed_targets(Exec& x, const std::vector<Target>& items, const std::vector<Assignment>& assign) {
    std::vector<const TypeDescriptor*> types;
    for (auto& t : items) {
        const Value& v = t.item.value;
        if ((v.is_object() || v.is_struct()) && std::find(types.begin(), types.end(), v.type()) == types.end()) types.push_back(v.type());
    }
    if (types.size() < 2) return;
    for (auto& a : assign) {
        if (a.target->segments.empty() || a.target->segments[0].kind != SegmentKind::Member) continue;
        const std::string& name = a.target->segments[0].name;
        std::vector<const TypeDescriptor*> lacking;
        std::vector<std::string> having, lacking_names;
        for (auto* t : types) {
            if (x.view(t).find(name)) {
                if (having.size() < 3 && std::find(having.begin(), having.end(), t->name) == having.end()) having.push_back(t->name);
            } else {
                lacking.push_back(t);
                if (lacking_names.size() < 4) lacking_names.push_back(t->name);
            }
        }
        if (lacking.empty() || having.empty()) continue;
        size_t n = 0;
        for (auto& t : items)
            if (std::find(lacking.begin(), lacking.end(), t.item.value.type()) != lacking.end() && (t.item.value.is_object() || t.item.value.is_struct())) ++n;
        throw RqlException("unknown-member",
                           "no member '" + name + "' on " + join(lacking_names, ", ") + " (" + std::to_string(n) + " of " + std::to_string(items.size()) +
                               " targets); nothing was changed",
                           filter_hint(having, "'" + name + "'", "set \xE2\x80\xA6") + ", or \xE2\x80\xA6 | where has(" + name + ") | set \xE2\x80\xA6");
    }
}

QueryResult run_set(Exec& x, Compiler& c, Encoder& enc, Stream& stream, const Stage& s) {
    check_policy(x, "set");
    struct Assign {
        const Expr* target;
        Eval value;
        std::string label;
    };
    std::vector<Assign> assigns;
    c.mutation_values = true;  // $count is defined in set values (spec §3)
    for (auto& a : s.assign) assigns.push_back({a.target.get(), c.compile(*a.value), label_of(*a.target)});
    c.mutation_values = false;
    std::vector<Target> items = targets(x, stream, "set");
    check_mixed_targets(x, items, s.assign);
    std::vector<PlannedWrite> writes;
    for (auto& t : items) {
        const Value& item = t.item.value;
        const EntryOrigin* origin = item.is_entry() ? item.entry().origin.get() : nullptr;
        const std::string item_ref = origin ? x.ref_of(origin->holder.is_missing() ? origin->root : origin->holder) : x.ref_of(item);
        // Without a path from the object the map was read from, the write starts at the map: that object (when known) still
        // owns the change unless an object inside the value does.
        const bool holder_owns = origin && origin->prefix.empty() && !origin->holder.is_missing();
        for (auto& a : assigns) {
            // A streamed map entry writes through its map: [key].path on the map (spec §8).
            std::shared_ptr<const std::vector<Segment>> owned = origin ? entry_segments(item, a.target->segments, a.label) : nullptr;
            const Value& root = origin ? origin->root : item;
            const std::vector<Segment>& segs = owned ? *owned : a.target->segments;
            const std::string label = origin ? raw_label(segs) : a.label;
            enter_scope(x, t);
            Value nv = a.value(x);
            if (nv.is_missing() || nv.is_unknown())
                throw RqlException("coercion-failed", "value for '" + label + "' on " + item_ref + " is missing");
            if (nv.is_error())
                throw RqlException(nv.error_code() == "stale-ref" ? "stale-ref" : "coercion-failed",
                                   "value for '" + label + "' on " + item_ref + " failed: " +
                                       (nv.error_detail().empty() ? nv.error_code() : nv.error_detail()));
            Value container, owner;
            Slot slot = resolve(x, root, segs, container, owner);
            if (holder_owns && owner.is_map()) owner = origin->holder;
            check_writable(x, slot, label, root, "");
            std::string why;
            // The outer of an instanced literal is the owning object, never a map, list or struct (spec §4.5).
            auto coerced = coerce(x, nv, slot.type, slot.nullable, why, owner.is_object() ? owner : Value::null());
            if (!coerced)
                throw RqlException("coercion-failed", "cannot assign " + describe_value(nv) + " to '" + label + "' (" +
                                                          (slot.type ? slot.type->friendly_name() : std::string("object")) + "): " + why);
            // Adapter validation runs before any write (spec §13): members and virtual-map entries. Virtual maps
            // accept writes to existing entries only (a new key could not be removed again on rollback).
            std::string rejected;
            Value existing;
            if (slot.kind == Slot::MapKey && container.map().fixed_keys() && container.map().find(slot.key, existing) != MapLookup::Found)
                rejected = "no entry '" + to_text(slot.key) + "'";
            else
                rejected = slot.validate(container, *coerced);
            if (!rejected.empty())
                throw RqlException("invalid-value", "invalid value " + value_text(nv) + " for '" + label + "' on " + item_ref + ": " + rejected);
            writes.push_back({root, &segs, label, snapshot(safe_get(slot, container, label)), *coerced, owner, item, origin ? x.ref_of(owner) : item_ref, owned, *coerced});
        }
    }

    auto to_records = [&] {
        std::vector<MutationRecord> out;
        for (auto& w : writes) out.push_back({w.target, w.owner, w.label, w.old_value, w.new_value, nullptr, {}});
        return out;
    };
    std::vector<MutationRecord> records;  // built once, shared by the batch validator and both hooks
    if (x.engine.mutations().validate_batch || has_hooks(x)) records = to_records();
    validate_batch(x, "set", records);
    if (!has_hooks(x)) records.clear();
    before_write(x, records);
    if (!x.options.dry_run) {
        size_t applied = 0;
        std::optional<std::string> failure = capture_failure([&] {
            for (; applied < writes.size(); ++applied) assign(x, writes[applied].item, *writes[applied].segments, writes[applied].new_value);
        });
        if (failure) {
            // The failed write itself is restored too: adapter setters may change state before throwing.
            const bool self_restored =
                !capture_failure([&] { assign(x, writes[applied].item, *writes[applied].segments, writes[applied].old_value); });
            size_t rollback_failures = 0;
            for (size_t k = applied; k-- > 0;)
                if (capture_failure([&] { assign(x, writes[k].item, *writes[k].segments, writes[k].old_value); })) ++rollback_failures;
            std::string rolled = rollback_failures == 0
                                     ? std::to_string(applied) + " earlier write(s) were rolled back"
                                     : "rolling back " + std::to_string(applied) + " earlier write(s) FAILED for " +
                                           std::to_string(rollback_failures) + " of them - state may be partially modified";
            if (!self_restored) rolled += "; restoring '" + writes[applied].label + "' itself also failed, so it may be partially modified";
            throw RqlException("coercion-failed", "write '" + writes[applied].label + "' failed (" + *failure + "); " + rolled);
        }
        // The receipt (and the committed hook) report what the host stored, which a setter may have clamped (spec §8).
        for (size_t k = 0; k < writes.size(); ++k) {
            writes[k].new_value = read_back(x, writes[k]);
            if (!records.empty()) records[k].new_value = writes[k].new_value;
        }
    }
    committed(x, records);

    // No-op assignments (the value read back equals the old one) are counted, not listed (spec §8 receipt).
    std::vector<std::vector<Value>> rows;
    int64_t unchanged = 0;
    for (auto& w : writes) {
        // A write the host refused or clamped back to the old value stays listed.
        if (rql_equal(x, w.old_value, w.planned) == std::optional<bool>(true) && rql_equal(x, w.old_value, w.new_value) == std::optional<bool>(true)) {
            ++unchanged;
            continue;
        }
        rows.push_back({Value::string(w.ref), Value::string(w.label), w.old_value, w.new_value});
    }
    QueryResult r = enc.rows({"$ref", "member", "old", "new"}, rows, "changes");
    r.dry_run = x.options.dry_run;
    r.unchanged = unchanged;
    return r;
}

namespace {

/// The source-qualified call text of a hint: … | call src.M(… $ …).
std::string per_item_form(const std::string& source, const std::string& method) {
    const std::string ell = "\xE2\x80\xA6";
    return ell + " | call " + source + "." + method + "(" + ell + " $ " + ell + ")";
}

/// The sources whose value is a single object with a callable method of this name.
std::vector<std::string> source_names_with_callable(Exec& x, const std::string& method) {
    std::vector<std::string> names;
    for (auto& s : x.engine.sources()) {
        Value v;
        try {
            v = x.live(s.getter());
        } catch (...) {
            continue;
        }
        if (!v.is_object() || !v.type()) continue;
        const TypeView view = x.view(v.type());
        for (auto& m : view.methods())
            if (m.callable && iequals(m.name, method)) {
                names.push_back(s.name);
                break;
            }
    }
    return names;
}

}  // namespace

/// Sources whose value is a single object with a callable method of this name, as hint text, or "" (spec §10).
std::string sources_with_callable(Exec& x, const std::string& method) {
    const std::vector<std::string> names = source_names_with_callable(x, method);
    if (names.empty()) return {};
    const std::string ell = "\xE2\x80\xA6";
    const std::string per_item = per_item_form(names[0], method) + " once per item";
    if (names.size() == 1)
        return "; the source '" + names[0] + "' has " + method + ": " + names[0] + " | call " + method + "(" + ell + ") calls it once, " + per_item;
    return "; sources with " + method + ": " + join(names, ", ") + " (e.g. " + names[0] + " | call " + method + "(" + ell + ") once, " + per_item + ")";
}

/// The hint of an unqualified `call` on items that are not objects (numbers from range(), strings, structs): qualify the
/// method with the source that has it, so it runs once per item (spec §8, §10).
std::string per_item_call_hint(Exec& x, const std::string& method) {
    const std::string ell = "\xE2\x80\xA6";
    const std::vector<std::string> having = source_names_with_callable(x, method);
    if (!having.empty()) return "to run a source's method once per item, qualify it with the source: " + per_item_form(having[0], method) + " ($ is the item)";
    std::vector<std::string> names;
    for (auto& src : x.engine.sources()) names.push_back(src.name);
    return "call runs a method of each item, and these items have none; to run a source's method once per item qualify it: " + ell + " | call <source>." +
           method + "(" + ell + " $ " + ell + ") ($ is the item; sources: " + join(names, " ") + ")";
}

namespace {

/// Why one candidate does not fit, ranked by how close it came (spec §8 "Overloads").
struct Miss {
    const MethodDescriptor* method;
    bool arity_fits;
    long long matched;
    int depth;
    std::string why;
    /// Bound by name except one key that is a parameter's name misspelt (spec §8): closer than any failure depth.
    bool misspelt = false;
};

int compare_miss(const Miss& a, const Miss& b) {
    if (a.arity_fits != b.arity_fits) return a.arity_fits ? 1 : -1;
    if (a.matched != b.matched) return a.matched < b.matched ? -1 : 1;
    if (a.misspelt != b.misspelt) return a.misspelt ? 1 : -1;
    return a.depth == b.depth ? 0 : a.depth < b.depth ? -1 : 1;
}

bool ascii_iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

/// The parameter index of each literal key (exact name, else the only case-insensitive match); false with a miss when a key
/// names no parameter or a parameter without a default is not named.
bool bind_names(const MethodDescriptor& m, const Value& literal, std::vector<size_t>& slots, std::vector<Miss>& misses) {
    const auto& ps = m.params;
    std::vector<bool> named(ps.size(), false);
    long long known = 0;
    std::string unknown, twice;
    literal.map().for_each([&](const Value& k, const Value&) {
        const std::string key = to_text(k);
        size_t i = ps.size();
        for (size_t j = 0; j < ps.size() && i == ps.size(); ++j)
            if (ps[j].name == key) i = j;
        if (i == ps.size()) {
            size_t hits = 0, hit = 0;
            for (size_t j = 0; j < ps.size(); ++j)
                if (ascii_iequals(ps[j].name, key)) ++hits, hit = j;
            if (hits == 1) i = hit;
        }
        if (i == ps.size()) {
            if (unknown.empty()) unknown = key;
            return true;
        }
        if (named[i]) {
            twice = ps[i].name;
            return false;
        }
        named[i] = true;
        slots.push_back(i);
        ++known;
        return true;
    });
    std::vector<std::string> names;
    for (auto& p : ps) names.push_back(p.name);
    if (!twice.empty()) {
        misses.push_back({&m, true, known, 0, "parameter '" + twice + "' is named twice"});
        return false;
    }
    if (!unknown.empty()) {
        // A key close to a parameter not named yet is that parameter misspelt, so "no parameter 'klass' (did you mean
        // 'className'?)" beats an overload that cannot take the literal at all (spec §8).
        std::vector<std::string> unnamed;
        for (size_t j = 0; j < ps.size(); ++j)
            if (!named[j]) unnamed.push_back(ps[j].name);
        const std::string close = closest_parameter(unknown, unnamed);
        misses.push_back({&m, true, known, 0,
                          "no parameter '" + unknown + "'" + (close.empty() ? std::string() : " (did you mean '" + close + "'?)") +
                              (ps.empty() ? std::string("; it takes no parameters") : "; parameters: " + join(names, ", ")),
                          !close.empty()});
        return false;
    }
    for (size_t j = 0; j < ps.size(); ++j) {
        if (named[j] || ps[j].has_default) continue;
        misses.push_back({&m, true, known, 0, "missing parameter '" + ps[j].name + "'; parameters: " + join(names, ", ")});
        return false;
    }
    return true;
}

/// The fewest arguments a call can pass: up to the last parameter without a default.
size_t min_arity(const std::vector<ParamDescriptor>& ps) {
    size_t min = 0;
    for (size_t i = 0; i < ps.size(); ++i)
        if (!ps[i].has_default) min = i + 1;
    return min;
}

/// Overload resolution and argument binding for `call` (spec §8 "Overloads"): in order, each candidate is checked with the
/// side-effect-free can_coerce and the first that passes is coerced for real (the next is tried only when that fails); a
/// single object literal that no overload took binds by parameter name. A failure (null result) sets `why`: a host policy
/// denial met on the way (`denied` true), else the best-matching candidate's reason.
const MethodDescriptor* resolve_overload(Exec& x, const TypeDescriptor& type, const std::vector<const MethodDescriptor*>& candidates,
                                         const std::vector<Value>& given, std::vector<Value>& coerced, std::string& why, bool& denied,
                                         const Value& outer) {
    std::vector<Miss> misses;
    std::string denial;
    // Checks every bound value without side effects, then coerces them for real; a policy denial is remembered, not raised,
    // so an overload that would not be chosen cannot veto the call.
    auto bind = [&](const MethodDescriptor& m, const std::vector<Value>& vals, const std::vector<size_t>& slots, const std::string& kind) {
        long long matched = 0;
        int depth = 0;
        std::string first;
        for (size_t k = 0; k < slots.size(); ++k) {
            const ParamDescriptor& p = m.params[slots[k]];
            std::string w;
            int d = 0;
            bool ok = false;
            try {
                ok = can_coerce(x, vals[k], p.type, p.nullable, w, d, outer);
            } catch (const RqlException& ex) {
                if (ex.code() != "permission-denied") throw;
                w = ex.what();
                d = 0;
                if (denial.empty()) denial = kind + " '" + p.name + "': " + w;
            }
            if (ok) {
                ++matched;
                continue;
            }
            if (first.empty()) {
                first = kind + " '" + p.name + "': " + w;
                depth = d;
            }
        }
        if (!first.empty()) {
            // By name, every bound key names a parameter (spec §8): an exact name whose value fails ranks above a misspelt one.
            misses.push_back({&m, true, kind == "parameter" ? static_cast<long long>(slots.size()) : matched, depth, first});
            return false;
        }
        std::vector<Value> cv;
        for (auto& p : m.params) cv.push_back(p.default_value);
        for (size_t k = 0; k < slots.size(); ++k) {
            const ParamDescriptor& p = m.params[slots[k]];
            std::string w;
            auto r = coerce(x, vals[k], p.type, p.nullable, w, outer);
            if (!r) {
                // Only the real coercion sees what the probe cannot (member validators, setters that throw); a candidate that
                // passed the probe is the closest match, so its reason is the one reported.
                misses.push_back({&m, true, std::numeric_limits<long long>::max(), 0, kind + " '" + p.name + "': " + w});
                return false;
            }
            cv[slots[k]] = *r;
        }
        coerced = std::move(cv);
        return true;
    };
    // 1. Positional.
    for (auto* m : candidates) {
        const auto& ps = m->params;
        if (ps.size() < given.size() || min_arity(ps) > given.size()) {
            misses.push_back({m, false, 0, 0, ""});
            continue;
        }
        std::vector<size_t> slots;
        for (size_t i = 0; i < given.size(); ++i) slots.push_back(i);
        if (bind(*m, given, slots, "argument")) return m;
    }
    // 2. By name: a single object literal that no overload took positionally names the parameters.
    if (given.size() == 1 && given[0].is_map() && given[0].map().object_literal()) {
        std::vector<Value> vals;
        given[0].map().for_each([&](const Value&, const Value& v) {
            vals.push_back(v);
            return true;
        });
        for (auto* m : candidates) {
            std::vector<size_t> slots;
            if (bind_names(*m, given[0], slots, misses) && bind(*m, vals, slots, "parameter")) return m;
        }
    }
    denied = !denial.empty();
    if (denied) {
        why = denial;
        return nullptr;
    }
    // The best candidate's reason; candidates that came equally close are listed with their signatures.
    std::vector<const Miss*> fit;
    for (auto& m : misses)
        if (m.arity_fits) fit.push_back(&m);
    if (fit.empty()) {
        // Each overload accepts from its last parameter without a default up to all of them.
        std::vector<std::pair<size_t, size_t>> ranges;
        for (auto& m : misses) ranges.emplace_back(min_arity(m.method->params), m.method->params.size());
        std::sort(ranges.begin(), ranges.end());
        ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());
        std::vector<std::string> parts;
        for (auto& r : ranges) parts.push_back(r.first == r.second ? std::to_string(r.first) : std::to_string(r.first) + " to " + std::to_string(r.second));
        why = "expects " + join(parts, " or ") + " argument(s), got " + std::to_string(given.size());
        return nullptr;
    }
    const Miss* best = fit[0];
    for (auto* m : fit)
        if (compare_miss(*m, *best) > 0) best = m;
    std::vector<const Miss*> tied;
    for (auto* m : fit)
        if (compare_miss(*m, *best) == 0 && std::none_of(tied.begin(), tied.end(), [&](const Miss* o) { return o->method == m->method; }))
            tied.push_back(m);
    if (tied.size() == 1) {
        why = best->why;
        return nullptr;
    }
    std::vector<std::string> parts;
    for (auto* m : tied) parts.push_back(type.signature(*m->method) + ": " + m->why);
    why = "no overload fits: " + join(parts, "; ");
    return nullptr;
}

}  // namespace

QueryResult run_call(Exec& x, Compiler& c, Encoder& enc, Stream& stream, const Stage& s, const std::function<Value()>& resolve_receiver) {
    check_policy(x, "call");
    // After the policy check: a read request never evaluates the target.
    std::optional<Value> resolved;
    if (resolve_receiver) resolved = resolve_receiver();
    const Value* receiver = resolved ? &*resolved : nullptr;
    std::vector<Eval> args;
    c.mutation_values = true;  // $count is defined in call arguments (spec §3)
    for (auto& a : s.args) args.push_back(c.compile(*a));
    c.mutation_values = false;
    // The callable overloads of the method on a target, by arity; none: unknown-function (or permission-denied for a method
    // the host does not expose), naming the targets' types and the sources that have it (spec §10).
    auto candidates_of = [&](const Value& item, const std::vector<Target>* stream_items) {
        const TypeDescriptor* t = (item.is_object() || item.is_struct()) ? item.type() : nullptr;
        std::vector<const MethodDescriptor*> candidates;
        bool exists = false;
        std::vector<std::string> sigs;
        if (t) {
            const TypeView view = x.view(t);  // host-registered + adapter methods (spec §13)
            for (auto& m : view.methods()) {
                if (!iequals(m.name, s.method)) {
                    if (m.callable) sigs.push_back(t->signature(m));
                    continue;
                }
                exists = true;
                if (m.callable) candidates.push_back(&m);
            }
            // Overloads are chosen by arity, then coercibility.
            std::stable_sort(candidates.begin(), candidates.end(),
                             [](const MethodDescriptor* a, const MethodDescriptor* b) { return a->params.size() < b->params.size(); });
            for (auto* m : candidates) sigs.push_back(t->signature(*m));
        }
        if (!candidates.empty()) return candidates;
        std::string tn = type_name_of(item);
        // In a mixed stream, name the targets' types that do have the method (filter to them first).
        std::vector<std::string> having;
        if (stream_items)
            for (auto& o : *stream_items) {
                const Value& ov = o.item.value;
                if (!(ov.is_object() || ov.is_struct()) || having.size() >= 3) continue;
                if (std::find(having.begin(), having.end(), ov.type()->name) != having.end()) continue;
                const TypeView ovw = x.view(ov.type());
                for (auto& m : ovw.methods())
                    if (m.callable && iequals(m.name, s.method)) {
                        having.push_back(ov.type()->name);
                        break;
                    }
            }
        std::string filter = having.empty() ? std::string() : filter_hint(having, s.method, "call " + s.method + "(\xE2\x80\xA6)") + "; ";
        throw RqlException(exists ? "permission-denied" : "unknown-function",
                           "no callable method '" + s.method + "' on " + (stream_items ? tn : x.ref_of(item)),
                           filter + (!sigs.empty() ? "callable: " + join(sigs, ", ") : tn + " has no callable methods") +
                               (exists ? std::string() : sources_with_callable(x, s.method)));
    };
    // A source-qualified call checks its receiver's method before reading the stream: a typo fails even with no items.
    std::vector<const MethodDescriptor*> fixed;
    if (receiver) fixed = candidates_of(*receiver, nullptr);
    std::vector<Target> items = targets(x, stream, "call", s.method, receiver != nullptr);
    struct Planned {
        Value item;
        const MethodDescriptor* method;
        std::vector<Value> args;
        int64_t index;
    };
    std::vector<Planned> planned;
    // Where a failure is: the target, or for a source-qualified call the receiver and the item's position.
    auto at = [&](const Value& item, int64_t index) { return x.ref_of(item) + (receiver ? " for item " + std::to_string(index) : std::string()); };
    std::vector<const MethodDescriptor*> own;
    for (auto& target : items) {
        const Value& item = receiver ? *receiver : target.item.value;
        const TypeDescriptor* t = item.type();
        if (!receiver) own = candidates_of(item, &items);
        const std::vector<const MethodDescriptor*>& candidates = receiver ? fixed : own;
        enter_scope(x, target);
        std::vector<Value> vals;
        for (auto& a : args) vals.push_back(a(x));
        std::vector<Value> coerced;
        std::string why;
        bool denied = false;
        const MethodDescriptor* chosen = resolve_overload(x, *t, candidates, vals, coerced, why, denied, item);
        if (!chosen && denied) throw RqlException("permission-denied", "cannot call " + s.method + " on " + at(item, target.index) + ": " + why);
        if (!chosen) {
            std::vector<std::string> cs;
            for (auto* m : candidates) cs.push_back(t->signature(*m));
            throw RqlException("coercion-failed", "cannot call " + s.method + " on " + at(item, target.index) + ": " + why, "signature: " + join(cs, ", "));
        }
        // The method's own check, for every target before the first call (and on dry runs): all-or-nothing (spec §8).
        if (chosen->validate) {
            std::string rejected;
            if (auto failure = capture_failure([&] { rejected = chosen->validate(item.ptr(), coerced); }))
                rejected = "its check threw: " + *failure;
            if (!rejected.empty())
                throw RqlException("invalid-value", "cannot call " + t->signature(*chosen) + " on " + at(item, target.index) + ": " + rejected + "; nothing was called");
        }
        planned.push_back({item, chosen, std::move(coerced), target.index});
    }

    auto call_records = [&] {
        std::vector<MutationRecord> out;
        for (auto& p : planned) out.push_back({p.item, p.item, p.item.type()->signature(*p.method), Value(), Value(), p.method, p.args});
        return out;
    };
    std::vector<MutationRecord> records;  // built once, shared by the batch validator and both hooks
    if (x.engine.mutations().validate_batch || has_hooks(x)) records = call_records();
    validate_batch(x, "call", records);
    if (!has_hooks(x)) records.clear();
    before_write(x, records);
    std::vector<std::vector<Value>> rows;
    size_t done = 0;
    for (auto& p : planned) {
        Value result = Value::string("(dry-run)");
        if (!x.options.dry_run) {
            if (auto failure = capture_failure([&] { result = p.method->invoke(p.item.ptr(), p.args); })) {
                // Completed calls are not rolled back, so the host still learns about them.
                if (!records.empty()) committed(x, std::vector<MutationRecord>(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(done)));
                throw RqlException("call-threw", p.method->name + " on " + at(p.item, p.index) + " threw: " + *failure + "; " + std::to_string(done) +
                                                       " earlier call(s) completed and are NOT rolled back");
            }
        }
        ++done;
        // A source-qualified call reports what each invocation returned (the created object, spec §8).
        const bool created = receiver && !x.options.dry_run && result.is_object() && !x.is_dead(result);
        rows.push_back({Value::string(x.ref_of(created ? result : p.item)), Value::string(p.item.type()->signature(*p.method)), result});
    }
    committed(x, records);
    QueryResult r = enc.rows({"$ref", "method", "result"}, rows, "calls");
    r.dry_run = x.options.dry_run;
    return r;
}

}  // namespace detail
}  // namespace rql
