# SKILL.md provenance marker — plant log

Verification artifact for the front-matter provenance marker emitted by
`bridge/src/AgentConfig/SkillFileGenerator.cs`. Every generated per-tool `SKILL.md` carries, as the
**last block inside the YAML front matter**:

```yaml
---
name: actor-create
description: "..."
metadata:
  generated-by: mcp-plugin-dotnet
---
```

Byte-identical to the marker the shared `com.IvanMurzak.McpPlugin` AgentConfig generator writes (which
Unity and the Godot C# addon inherit by subclassing). The two-space indent is load-bearing: it is what
makes `generated-by` a nested mapping under `metadata` rather than a sibling top-level scalar.

The point of the marker is that a consumer can tell a generated skill file from a hand-authored one and
dedup only its own output. It is **fail-open**: an unmarked file is never excluded.

## Why a plant log

Every assertion below was proved to be able to FAIL, by mutating the generator and observing the
specific test go red. A test that passes is only evidence once you know it can fail.

Each plant was applied, verified and byte-exactly restored by the shared
`plant_and_revert.py` harness (out-of-tree backups, checksum-verified restore — never a
`git checkout --`), one at a time, against the whole bridge suite.

- Verify command (per plant): `dotnet test bridge/Unreal-MCP-Bridge.sln --configuration Debug --verbosity normal`
- Every plant collected **359 tests**, so no plant reddened the suite through an import or compile
  break instead of through its own mutation.
- Green before the round: `Test Run Successful. Total tests: 359 / Passed: 359`
- Green after the round, on the restored tree: `Test Run Successful. Total tests: 359 / Passed: 359`

## The round

```
SUMMARY  8/8 matched their expect | 8 RED | 0 GREEN | restore-failures 0
```

| # | Mutation (in `SkillFileGenerator.cs`) | Reddened test | Runner output |
|---|---|---|---|
| N1 | Marker block deleted — the front matter closes straight after `description:` | `Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed` | `RED (exit 1)` · `Assert.Equal() Failure: Strings differ / Expected: "metadata:" / Actual: "---"` |
| N2 | Marker block emitted **twice** inside the front matter | `BuildSkillMarkdown_ProvenanceMarkerAppearsExactlyOnce_InTheFrontMatterAndNowhereElse` | `RED (exit 1)` · `Assert.Equal() Failure: Values differ / Expected: 1 / Actual: 2` |
| N3 | Marker entry emitted **without the two-space indent** (a sibling scalar, not a nested mapping) | `Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed` | `RED (exit 1)` · `Expected: "  generated-by: mcp-plugin-dotnet" / Actual: "generated-by: mcp-plugin-dotnet"` |
| N4 | Marker moved **after** the closing `---` (outside the front matter a consumer parses) | `Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed` | `RED (exit 1)` · `Expected: "metadata:" / Actual: "---"` |
| N5 | `SingleLineCapped` dropped from the `description:` scalar, so a multi-line tool description survives into the front matter | `BuildSkillMarkdown_DescriptionCannotForgeASecondTopLevelMarker` | `RED (exit 1)` · `Assert.Equal() Failure: Values differ / Expected: 1 / Actual: 2` — **the only test that reddened**, so the forgery guard is proved on its own |
| N6 | Marker value suffixed with `-{DateTime.UtcNow.Ticks}` (a version/timestamp in the value) | `Generate_IsByteStable_AcrossAFreshGeneratorAndAFreshEqualValuedInput` | `RED (exit 1)` · `Assert.Equal() Failure: Collections differ / Expected: [···, 48, 57, 49, 54, 52, ···] / Actual: [···, 48, 57, 50, 54, 52, ···]` |
| N7 | Document joined with `"\r\n"` instead of `"\n"` | `Generate_WritesAnLfOnlyFrontMatter_SoTheMarkerNeedsNoCrlfNormalisation` | `RED (exit 1)` · `front matter is never closed with an LF-delimited ---` |
| N8 | A **second** marker emitted below the front matter, leaving the front matter itself correct | `BuildSkillMarkdown_ProvenanceMarkerAppearsExactlyOnce_InTheFrontMatterAndNowhereElse` | `RED (exit 1)` · `Assert.Equal() Failure: Values differ / Expected: 1 / Actual: 2` — **the only test that reddened**; the positional test stayed green, which is what makes the "and nowhere else" half independently proved |

Both directions are covered: N1 proves the marker test reddens when the marker is **removed**; N2 / N3 /
N4 / N8 prove the exactly-once and positional tests redden when it is emitted **twice, at the wrong
indent, or in the wrong place**.

## Line endings

The document skeleton is joined with `"\n"`, so the **front matter is LF-only on every platform,
Windows included** — a marker parser never sees a `\r` glued to the value, and needs no CRLF
normalization for this generator's output. Pinned by `Generate_WritesAnLfOnlyFrontMatter_...` and
proved falsifiable by N7.

Further down the document the `### Input JSON Schema` / `### Output JSON Schema` fences are serialized
by `System.Text.Json` with `WriteIndented = true`, whose `JsonWriterOptions.NewLine` defaults to
`Environment.NewLine` — so a generated `SKILL.md` is **mixed-ending on Windows** (LF skeleton, CRLF
inside those fences). That is pre-existing behaviour, outside the front matter, and is not changed
here; it is recorded so nobody re-derives it from a surprising diff.
