#pragma once

namespace rql {

/// Agent-facing text shared by every RQL host: the language rules that do not depend on the host's sources or types
/// (boolean conditions, ?:, list equality, group items, receipts, call and bulk create). Hosts append it to their own
/// guide instead of copying it, so the rules do not drift. The same text ships with the .NET (RqlLanguageGuide.Rules) and
/// TypeScript (LANGUAGE_RULES) runtimes; fixtures/guide/language-rules.txt is the source.
inline const char* language_rules() noexcept {
    return
        "Language rules:\n"
        "- Conditions are booleans. where, and / or / not, ?: and the predicates of .any() .all() .count() .first() take a comparison or has(), never a bare value: where material != null (or has(material)), where count > 0, where name != \"\", where len(tags) > 0. A number, string, object or list there fails with type-mismatch; null counts as false.\n"
        "- cond ? a : b (the same as if(cond, a, b)) picks a value and evaluates only the taken branch: set y = (y < 0 ? 0 : y). Inside select, wrap computed values in parentheses: select kind = (intensity > 2 ? \"bright\" : \"dim\").\n"
        "- Lists compare by value, in order: tags == [\"a\", \"b\"], x in [[1, 2], [3, 4]]; distinct and group-by merge equal lists.\n"
        "- group-by gives rows with key (or one column per key), count and the group's items: group-by kind | select key, count, avg = items.avg(weight); also items.sum(e), items.min(e), items.max(e), items.count(pred), items.first().name.\n"
        "- Receipts list only real changes: a set row whose value stayed the same is left out and counted as unchanged, so no rows with N unchanged means the values already matched.\n"
        "- call Method(args) runs the method of each item (the items must be objects). call source.Method(args) runs the method of that one source object once per stream item, with $, $index, $path and $depth in the arguments being the item's; every call counts against the target limit, and every argument is checked before the first call (a dry run checks without calling).\n"
        "- Bulk create: range(start, end[, step]) streams numbers (end excluded), and a list literal streams its elements, so one call creates many objects: range(0, 300) | call editor.CreateObject(\"Cube\", {position: [($ % 15) * 2, 0, floor($ / 15) * 2]}) builds a 15-wide grid, [[\"A\", 1], [\"B\", 2]] | call editor.Create($[0], $[1]) one object per entry (editor.CreateObject stands for the host's source and creation method). The receipt has one row per call with the $ref of the created object.\n"
        "- Vectors and colors the host marks as component-wise compute component by component: set Scale = Scale * 1.5, Position + [0, 1, 0], Position - Offset, -Velocity, v / 2. Adding a number to a vector is a type-mismatch (add [x, y, z] instead); quaternions and other types without that mark stay type-mismatch.\n"
        "- A missing map entry reads as null: props[\"k\"] == null and props[\"k\"] is null are true without the entry, and has(props[\"k\"]) tests whether it exists. A misspelt member is not null: it stays unknown and is reported.\n"
        "- Arguments by name: call Method({name: value, ...}) with one object literal that no overload takes as a whole binds each key to the parameter of that name; parameters left out keep their defaults, and an unknown name fails listing the parameters: range(0, 10) | call editor.CreateObject({name: \"Tile\" + $, position: [$ * 2, 0, 0]}). Engine objects (scene objects, nodes, actors) are never built from an object literal: pass an existing one, e.g. ref(\"Type#id\").\n"
        ;
}

}  // namespace rql
