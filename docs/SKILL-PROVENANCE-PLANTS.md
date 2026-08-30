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

**The marker's contract — its shape, its position, the fail-open expectation, and how it relates to the
shared generator's — lives in [`ARCHITECTURE.md`](ARCHITECTURE.md) §2.4, which is authoritative.** This
file does not restate it; it records only the evidence that the tests pinning it can actually fail.

## Why a plant log

Every assertion below was proved to be able to FAIL, by mutating the generator and observing the
specific test go red. A test that passes is only evidence once you know it can fail.

Each plant was applied on its own, verified against the whole bridge suite, then restored from an
out-of-tree backup and checksum-verified — never reverted with `git checkout --`, which would have
taken uncommitted work with it.

- Verify command (per plant): `dotnet test bridge/Unreal-MCP-Bridge.sln --configuration Debug --verbosity normal`
- Every plant collected **359 tests**, so no plant reddened the suite through an import or compile
  break instead of through its own mutation.
- Green before the round: `Test Run Successful. Total tests: 359 / Passed: 359`
- Green after the round, on the restored tree: `Test Run Successful. Total tests: 359 / Passed: 359`

## The round

```
SUMMARY  11/11 matched their expect | 11 RED | 0 GREEN | restore-failures 0
```

**The "Attributed test" column names the test the plant is marked on — the one whose failure is the
evidence — not the only test that went red.** A mutation to shared emission code legitimately reddens
several tests at once; the per-plant total is given so that breadth is visible rather than implied
away. Where a plant reddened exactly **one** test, that is called out, because it means the claim is
proved on its own rather than through a sibling assertion.

**Every plant in a shared-marker group fails with different text.** Proving a mutation *can* redden a
test and being able to *tell two reds apart* are different properties; the second is what makes a
future failure diagnosable, and it is the reason N1/N4 and N2/N8 carry the wording they do.

| # | Mutation (in `SkillFileGenerator.cs`) | Attributed test | Runner output | Reds |
|---|---|---|---|---|
| N1 | Marker block deleted — the front matter closes straight after `description:` | `Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed` | `RED (exit 1)` · `'metadata:' is ABSENT from the generated document entirely` | 5 |
| N2 | Marker block emitted **twice** inside the front matter | `BuildSkillMarkdown_ProvenanceMarkerAppearsExactlyOnce_InTheFrontMatterAndNowhereElse` | `RED (exit 1)` · `in-front-matter half: expected exactly ONE 'metadata:' — found 2` | 3 |
| N3 | Marker entry emitted **without the two-space indent** (a sibling scalar, not a nested mapping) | `Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed` | `RED (exit 1)` · `Assert.Equal() Failure: Strings differ / Expected: "  generated-by: mcp-plugin-dotnet" / Actual: "generated-by: mcp-plugin-dotnet"` | 5 |
| N4 | Marker moved **after** the closing `---` (outside the front matter a consumer parses) | `Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed` | `RED (exit 1)` · `Assert.Equal() Failure: Values differ / Expected: 3 / Actual: 4` (the marker's index moved out of the front matter) | 4 |
| N5 | `SingleLineCapped` dropped from the `description:` scalar, so a multi-line tool description survives into the front matter | `BuildSkillMarkdown_DescriptionCannotForgeASecondTopLevelMarker` | `RED (exit 1)` · `Assert.Equal() Failure: Values differ / Expected: 1 / Actual: 2` — **the only test that reddened**, so the forgery guard is proved on its own | 1 |
| N6 | Marker value suffixed with `-{DateTime.UtcNow.Ticks}` (a version/timestamp in the value) | `Generate_IsByteStable_AcrossAFreshGeneratorAndAFreshEqualValuedInput` | `RED (exit 1)` · `Assert.Equal() Failure: Collections differ / Expected: [···, 55, 52, 51, 50, 50, ···] / Actual: [···, 55, 52, 52, 51, 53, ···]` | 6 |
| N7 | Document joined with `"\r\n"` instead of `"\n"` | `Generate_WritesAnLfOnlyFrontMatter_SoTheMarkerNeedsNoCrlfNormalisation` | `RED (exit 1)` · `front matter is never closed with an LF-delimited ---` | 4 |
| N8 | A **second** marker emitted below the front matter, leaving the front matter itself correct | `BuildSkillMarkdown_ProvenanceMarkerAppearsExactlyOnce_InTheFrontMatterAndNowhereElse` | `RED (exit 1)` · `nowhere-else half: expected exactly ONE 'metadata:' — found 2` — **the only test that reddened**; the positional test stayed green, which is what makes the "and nowhere else" half independently proved | 1 |
| N9 | `name:` and `description:` emitted in the opposite order | `Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed` | `RED (exit 1)` · `Assert.Equal() Failure: Strings differ / Expected: "name: demo-tool" / Actual: "description: "Does a demo thing.""` — **the only test that reddened** | 1 |
| N10 | An extra front-matter key emitted **after** the marker — marker present, correctly indented, simply no longer the last block | `Generate_StampsProvenanceMarkerAsTheLastFrontMatterBlock_PositionalAndUntrimmed` | `RED (exit 1)` · `Assert.Equal() Failure: Strings differ / Expected: "---" / Actual: "extra: 1"` | 2 |
| N11 | A lone `\r` glued to the marker value while the document join stays `"\n"` | `Generate_WritesAnLfOnlyFrontMatter_SoTheMarkerNeedsNoCrlfNormalisation` | `RED (exit 1)` · `Assert.DoesNotContain() Failure: Sub-string found / String: ···":\n  generated-by: mcp-plugin-dotnet\r\n---\n" / Found: "\r"` | 4 |

Both directions are covered. **Removal:** N1. **Wrong shape or wrong place:** N2 / N3 / N4 / N8 / N10
(twice, un-indented, outside the front matter, duplicated below it, no longer last). **Order:** N9
pins that `name:` stays first. **Value stability:** N6. **Line endings:** N7 and N11 — and they are
not redundant, because N7 aborts the test at its earlier "closed with an LF-delimited `---`" guard,
so N11 is what proves the `DoesNotContain("\r")` assertion itself can fail.
