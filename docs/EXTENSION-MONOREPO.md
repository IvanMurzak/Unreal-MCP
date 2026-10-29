# Optional extensions: one repository, independent releases, one editor per suite

The six first-party extensions live under `extensions/UnrealAI<Feature>/` as independent UE
plugins/modules. Stable extension IDs, plugin names, provider contracts, existing Automation specs,
E2E checks and licenses are preserved. `extensions/migration-sources.json` records imported commits.
Third-party plugins and the external Unreal-AI-Template repository remain supported.

## Local verification

On Windows, macOS or Linux, with the UE toolchain, Node 22 and Python 3.10+ (examples are Windows;
on macOS the engine is e.g. `'/Users/Shared/Epic Games/UE_5.8'`):

```shell
npm ci --prefix cli
npm run build --prefix cli
py -3 -m unittest discover -s scripts -p 'test_*.py'
py -3 scripts/extension_suite.py --engine 'C:/Program Files/Epic Games/UE_5.8' --mode full
```

`full` compiles core alone and core + each extension with no sibling plugin present. Extension
modules use non-unity compilation with no PCH in these isolated copies. It then builds a combined
host and starts **one** UnrealEditor-Cmd process for all core/extension Automation tests. All
preserved extension E2E checks run against that same process, local sidecar and local server.
The CLI itself runs from this checkout's compiled `cli/dist/index.js`.

Each E2E check is a declarative `extensions/<plugin>/Tests/e2e/tools/<name>.e2e.json`, run by
`scripts/run_extension_e2e.py` (`--project-dir`, `--url`, `--plugins`; the suite calls it in-process):

```json
{
  "description": "the count of PCG graphs",
  "tool": "rql-query",
  "input": { "query": "pcgGraphs | count" },
  "assert": { "pattern": "\"text\"\\s*:\\s*\"\\d+\"|value\\\\?\"\\s*:\\s*\\d" }
}
```

`tool` (required) is invoked with `run-tool`, or `run-system-tool` when `"system": true`; `input` (optional
object) becomes `--input`. `assert.pattern` is a regex searched case-insensitively in the compact JSON of
the result (`json.dumps(result, separators=(",", ":"), ensure_ascii=False)`; non-JSON CLI output is a JSON
string); `description` (required) names what it expects and is printed on failure. A check passes when the CLI exits
0 with a JSON object whose `isError` is not true and the pattern matches. `"expectError": true` inverts
that: the tool must fail and `assert` is then required to pin the specific error. A transport failure
(`(network)`, `(timeout)`, `ECONNREFUSED`, `ENOTFOUND`) always fails; a plugin without checks fails.

E2E reuses the already-published bridge version matching the core `.uplugin` and the pinned shared
server; it does not publish/rebuild those packages. `--server` and `--bridge` accept explicit local
binaries when validating unreleased core changes. `verification` runs isolated builds + combined
Automation without live E2E; `automation` builds/runs just combined Automation; `isolated` builds
only; `prepare` creates the combined host without building/launching anything.

Windows defaults to a short `C:/wt/unreal-tests/<hash-of-checkout>/` build-cache directory to avoid
MAX_PATH. Override with `--output`. Each engine installation/version and build profile owns separate
physical plugin copies, including their Intermediate/Binaries. No junctions point into shared source
or another engine's outputs. A marker refuses foreign directories; unexpected sibling plugins fail
closed. Source sync preserves profile-local caches, removes deleted source files and copies Automation
specs under `Source/.../Tests/` (only the top-level E2E directory is excluded from build copies).

`tests/automation-inventory.json` records required prefixes/minimum counts. The exported JSON report
must exist, all tests must complete successfully, and every expected suite must be present. Add
inventory entries for new extensions. A missing report, crash, missing suite or incomplete execution
fails. The runner records editor PID/launch count and stops its own editor/server process trees in
`finally`, including failures. Per-profile logs, Automation reports and `suite-result.json` remain for
review. Headless NullRHI coverage does not replace rendering checks. One shared process cannot prove
runtime startup in the absence of every other plugin; isolated compilation proves build independence.

## Independent versions and packages

Core, bridge and CLI retain their existing common version and `vX.Y.Z` release workflow. Extensions
each own a version. This preserves existing core download URLs and npm Trusted Publishing identity.
Extension/index releases are created with `--latest=false`, so `/releases/latest` remains the core.

```text
Unreal-MCP GitHub Releases
  v0.20.2/                  core + bridge (existing assets)
  niagara-v0.2.0/            UnrealAINiagara-0.2.0.zip
  pcg-v0.1.1/                UnrealAIPCG-0.1.1.zip
  extensions-index/          extensions-index.json (small, updated pointer/history)
```

Only the selected extension's source ZIP is packaged/published. No unchanged extension, bridge,
core artifact or npm package is republished. Test compilation is separate from distribution builds.
ZIPs include one plugin root and its source/content/config/resources/license, no build caches.

```shell
# A release-preparation PR, never a feature task:
py -3 scripts/extension_release.py bump --plugin UnrealAINiagara --version 0.2.0
py -3 scripts/extension_release.py package --plugin UnrealAINiagara
```

After that PR is reviewed/merged, dispatch `release_extensions.yml` on main with one plugin name, comma-separated names, or `all`. A batch runs the shared
suite once per engine, then publishes independent releases only for the selected components.
Default `dry_run=true` runs all tooling checks, isolated builds, combined Automation and E2E on
UE 5.7 + 5.8, then uploads the one package as an Actions artifact. Publication needs a separate,
explicitly authorized dispatch with `dry_run=false`. Missing engine checks are never success/skipped
release gates. Local runs and CI use the same exclusive machine-wide file lock, so interactive builds cannot
collide with CI UBT/AutomationTool. The workflow serializes all component/index publication and holds the existing machine
UE lock during each engine run. The package must match the current default-branch source.

The first publication uses `bootstrap_index=true` to initialize from the checked-in seed index.
Later publications download the live index, preserve all historical versions, publish/verify the
component archive first, then update/verify the index. Network failures never silently reseed it.
ZIP checksums prevent overwriting a published component version with different bytes; partial
publication can be retried. Unchanged packages and older versions are referenced at their original
release, never copied into the new one. Do not delete historical releases/index assets.

## CLI catalog and migration

The native Editor extensions panel also reads this index, uses component tags and verifies archive
checksums, with the legacy catalog URL as its migration fallback.

`install-extension` loads the validated online index from the fixed public GitHub Releases URL. It
resolves extension ID/name/pluginName from that index, so new extensions do not require a CLI release.
Each historical release can carry its own minimum core version and engine-plugin dependencies.
The index's `version` is stable latest for that extension; `releases` maps exact versions to repo/tag
and optional SHA-256. `--version` selects history; the default never downgrades an installed newer
version. Component archives are hash-checked when hashes exist, before modifying the installation.

The last validated index is cached under `~/.cache/unreal-mcp-cli/`. Offline/invalid online responses
use a validated cache with an explicit stale-version warning, then the bundled legacy catalog if
none exists. `--source` bypasses the index/network. Hosts/repo names/plugin basenames and versions
are validated before using online data as paths or download coordinates.

`extensions-index.json` in source is only the one-time bootstrap seed, not a list of unpublished
source versions. It points at existing legacy GitHub Release ZIPs and retains historical ControlRig
versions. The live index is owned by the release workflow. New source versions are advertised only
after publication/verification. A newly added plugin's `extension.json` supplies its catalog metadata.

Old clients keep downloading from the old extension repositories; preserve those repositories and
their published releases. New clients find the migrated tags through the index after updating the
CLI once. The infra workspace removes the six extension gitlinks; the legacy chain submodule bump
lever explicitly refuses Unreal monorepo releases instead of silently publishing to old repos.

## Publication checklist

1. Merge implementation with green checks/review; do not publish from feature branches.
2. For the first rollout, release the core/CLI update through the existing authorized core release flow.
3. Rehearse the selected extension with `dry_run=true`; inspect both engine results and package.
4. After owner release authorization, publish with `dry_run=false` (bootstrap only once).
5. Verify the live index and install via the released CLI in a clean project.

No production publication or old-repository archival is performed by the source migration itself.
