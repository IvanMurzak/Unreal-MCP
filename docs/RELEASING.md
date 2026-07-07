# Releasing Unreal-MCP

This document is the operator runbook for the CI/CD workflows under
`.github/workflows/`. It covers the PR test pipeline, the gated release pipeline,
the self-hosted Unreal runner, the dry-run rehearsal procedure, and the required
secrets / repository variables.

The authoritative design is [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §9
(repo / versioning / tests / CI). This file is the operational complement —
keep the two in lockstep, and keep the CI command surface 1:1 with the
implement-task profile `test.md` (infra repo).

For the short operator "what do I run?" flow, see
[`docs/GITHUB-RELEASE-RUNBOOK.md`](GITHUB-RELEASE-RUNBOOK.md). The wrapper
script documented there (`commands/release-github.ps1`) does **not** bypass this
file's contract — it only wraps `commands/bump-version.ps1` and `release.yml`.

## Release checklist — operator-gated leaves

The following actions are **deliberately operator-gated** and are **not** performed by a normal
merge, by CI on a feature/docs PR, or by any automated task. GitHub Releases and npm publishes are
now CI-backed once a deliberate version bump lands on `main`, but they still require an intentional
operator release decision. Fab submission remains fully manual:

- [ ] **GitHub Release + `v<version>` tag.** Fires only when a deliberate `VersionName` bump lands on
      `main` (run `commands/bump-version.ps1`, commit, merge) on an untagged version — or a manual
      `workflow_dispatch` with `dry_run=false`. `release.yml`'s `check-version` gate keeps this inert
      otherwise.
- [ ] **`unreal-mcp-cli` publish.** The initial npm bootstrap was manual; subsequent publishes are
      CI-owned through `release.yml`'s OIDC `publish-npm` job. Do **not** hand-publish from a feature
      branch or a normal docs/code PR. The historical one-time bootstrap runbook remains below for
      reference.
- [ ] **Fab (Epic marketplace) submission.** Entirely manual and **out of scope of every CI
      workflow** — the release pipeline produces both a dedicated source-plugin zip
      (`unreal-mcp-plugin-source-<version>.zip`, the CLI/Fab install asset) and the packaged
      BuildPlugin zip (`unreal-mcp-plugin-<version>.zip`), but no job submits to Fab. Fab carries
      its own metadata/screenshot requirements and an Epic review; do it by hand when the listing
      is ready. The plugin is
      Fab source-submission READY (#139/#187): the sidecar survives Fab's `Binaries/` strip via
      `Source/ThirdParty/UnrealMcpBridge/<rid>/`, the distributed `.uplugin` ships no test module, and `FilterPlugin.ini`
      is complete — see [Fab source-submission readiness](#fab-epic-marketplace-source-submission-readiness-139187) below.

A normal merge to `main` publishes nothing; the version gate keeps `release.yml` inert. The safe
rehearsal is a `dry_run=true` dispatch (below), which exercises the test + artifact jobs and
hard-skips every tag/Release/npm-publish job.

## Workflows at a glance

| Workflow | Trigger | What it does |
| --- | --- | --- |
| `test_pull_request.yml` | `pull_request` to `main` (+ manual) | Fans out the PR test legs: bridge build+xUnit (ubuntu + windows), cli node 20/22, and — when a runner is registered — the UE 5.7 plugin BuildPlugin + Automation leg. |
| `test_cli.yml` | `workflow_call` (reusable) | Builds + tests `unreal-mcp-cli` on Node 20 & 22. Called by both `test_pull_request.yml` and `release.yml`. |
| `release.yml` | `push` to `main` (+ manual `workflow_dispatch`) | Version-gated release: builds + **code-signs** the self-contained bridge per RID, validates the plugin Automation leg across UE 5.5/5.6/5.7/5.8, publishes a dedicated **source** plugin asset for CLI/Fab installs, **bundles the signed sidecar into the packaged plugin** (BuildPlugin), and (only on a real version bump) cuts the GitHub Release + tag and publishes `unreal-mcp-cli` to npm. Exposes a `dry_run` input to rehearse everything without publishing. |

### The signed self-bootstrapping sidecar bundle (release.yml artifact graph)

The packaged plugin ships the **signed, self-contained** `unreal-mcp-bridge` for
all four RIDs (`win-x64`, `osx-arm64`, `osx-x64`, `linux-x64`), so the end user
installs no .NET and performs no first-run download — the plugin is
self-bootstrapping out of the box (`docs/ARCHITECTURE.md` §6 BUNDLE model). The
artifact jobs in `release.yml`:

- **`build-bridge-macos`** (`macos-latest`) — publishes osx-arm64 / osx-x64 /
  linux-x64 self-contained, **codesigns** the two osx apphosts (hardened runtime
  + `build/entitlements.mac.plist`) and **notarizes** them, then uploads the raw
  signed dirs (staging input) + the per-RID release zips. linux-x64 ships
  unsigned (no Linux code-signing standard — matches GameDev-MCP-Server).
- **`build-bridge-windows`** (`windows-latest`) — publishes win-x64
  self-contained and signs the `.exe` via **Azure Trusted Signing**, then uploads
  the raw signed dir + the release zip. (signtool is Windows-only; codesign /
  notarytool are macOS-only — hence the two-runner split.)
- **`build-plugin-zip`** (self-hosted UE 5.7) — downloads the four signed RID
  dirs, **stages them into the Fab-surviving `UnrealMCP/Source/ThirdParty/UnrealMcpBridge/<rid>/`**
  (#139/#187 — the engine-canonical `Source/ThirdParty/` folder declared in `Config/FilterPlugin.ini`
  that survives a Fab strip; the `UnrealMcpRuntime.Build.cs` `RuntimeDependencies` two-arg form then
  stages it into `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` at compile time, the path the C++
  resolver reads — declared on the runtime module in R2 so the bridge bundles into
  packaged GAMES, not just editor packages; see
  [Packaged-game sidecar bundling](#packaged-game-sidecar-bundling-r2) below), then emits
  **two plugin zips**: the dedicated source install asset
  `unreal-mcp-plugin-source-<version>.zip` (CLI/Fab source form, no `EngineVersion` pin, no test module)
  and the packaged BuildPlugin asset `unreal-mcp-plugin-<version>.zip`.
  A post-BuildPlugin guard copies the binaries from `Source/ThirdParty/UnrealMcpBridge/<rid>/` into the
  package's `Binaries/ThirdParty/...` output if `RuntimeDependencies` did not stage
  them, and asserts the surviving `Source/ThirdParty/UnrealMcpBridge/<rid>/` payload shipped (the form Epic's
  Fab recompile stages from).

### Packaged-game sidecar bundling (R2)

The `RuntimeDependencies.Add(.../UnrealMcpBridge/<rid>/*)` declaration that stages
the sidecar lives in **`UnrealMcpRuntime.Build.cs`** (the `Type: Runtime` module),
NOT the editor module (`docs/ARCHITECTURE.md` §12.5). UBT only stages a module's
RuntimeDependencies into a build whose target includes that module: the runtime
module is part of both the editor target and a packaged Game target, so the bridge
now bundles into packaged **Development/Shipping game** builds — the editor module
is absent from a Game target, so when the declaration lived there a packaged game
shipped without the sidecar.

Conservative staging (adopted defaults, design Open questions 1 & 2):

- **Per-RID, not all four** — only the targeted platform's RID(s) are staged
  (`win-x64`, `linux-x64`, or both macOS slices), since each self-contained slice
  is ~73-80 MB.
- **Desktop-only** — nothing is staged for console/mobile targets (they cannot
  spawn an external .NET process); runtime MCP is Win64/Mac/Linux only.
- **Shipping is NOT bundled by default** — a Shipping game omits the sidecar unless
  the consumer opts in via the `bUnrealMcpAllowShipping` Build flag (the same flag
  that gates runtime `Connect()` in Shipping, §12.8 #3). Development and editor
  builds always stage.

**Staging a packaged game (testbed / consumer build):** the bridge binaries are
gitignored, so a fresh checkout has empty `Source/ThirdParty/UnrealMcpBridge/<rid>/` folders (only the
`README.md` + per-RID `.gitkeep` placeholders are tracked). Before packaging a
**game** (`BuildCookRun -build -cook -stage`, e.g. the `Unreal-Test-Project`
testbed) you must publish the sidecar for the target RID and place it in the
Fab-surviving `Source/ThirdParty/UnrealMcpBridge/<rid>/` folder first (#139/#187) — otherwise the per-RID
`RuntimeDependencies` source wildcard matches nothing and the staged game has no
sidecar:

```bash
# 1. Publish the self-contained sidecar for the target RID (e.g. win-x64):
bash bridge/publish.sh Release win-x64 --no-zip
# 2. Place it in the Fab-surviving source folder (#139/#187) — RuntimeDependencies
#    stages it from here into Binaries/ThirdParty/... at compile time, and the
#    resolver also reads it directly from here:
#    <plugin>/Source/ThirdParty/UnrealMcpBridge/<rid>/unreal-mcp-bridge[.exe]
cp bridge/publish/win-x64/* UnrealMCP/Source/ThirdParty/UnrealMcpBridge/win-x64/
# 3. Package the game (Win64 Development): RunUAT BuildCookRun -build -cook -stage.
#    The staged game then contains the sidecar at
#    <Staged>/<Project>/Plugins/UnrealMCP/Binaries/ThirdParty/UnrealMcpBridge/win-x64/
#    (RuntimeDependencies-staged) AND <...>/Plugins/UnrealMCP/Source/ThirdParty/UnrealMcpBridge/win-x64/
#    (the surviving source) — ResolveBridgeBinaryPath resolves either.
```

In CI this is automatic: `build-plugin-zip` downloads the signed per-RID dirs and
stages them into `Source/ThirdParty/UnrealMcpBridge/<rid>/` before BuildPlugin. The manual sequence above is
for a local packaged-game verification of the bundle (it is not part of a normal
release).

**Binaries are never committed to git.** The `Source/ThirdParty/UnrealMcpBridge/<rid>/` payloads (and
`Binaries/`) are gitignored; the signed binaries exist only transiently on the
runners during a release. A dev source checkout has empty `Source/ThirdParty/UnrealMcpBridge/<rid>/` folders,
so `ResolveBridgeBinaryPath` returns empty and devs use `UNREAL_MCP_BRIDGE_PATH`
(the documented inner loop) — unchanged.

### Fab (Epic marketplace) source-submission readiness (#139/#187)

Fab accepts a **source** plugin and recompiles it per engine version, **stripping
`Binaries/`, `Intermediate/`, `Saved/`** from the submitted zip. In-repo fixes
keep the plugin submittable (`docs/ARCHITECTURE.md` §6.7); submission itself is still
an entirely manual operator step (no CI job submits — see the checklist at the top):

- **C1 — surviving sidecar under the canonical ThirdParty layout.** The prebuilt sidecar lives in the
  Fab-surviving, engine-canonical `Source/ThirdParty/UnrealMcpBridge/<rid>/` folder (declared in
  `Config/FilterPlugin.ini`), staged into `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` by
  `RuntimeDependencies`, and resolved from both paths (`ComposeBundledBridgeCandidates`). So an
  Epic-compiled build resolves the sidecar even though `Binaries/` was stripped.
- **C2 — no shipped test module.** The DISTRIBUTED `UnrealMCP.uplugin` omits
  `UnrealMcpEditorTests` (Fab flags shipped test modules). PR/dev CI re-adds it
  transiently via `commands/test-module-uplugin.ps1` around the Automation BuildPlugin,
  then reverts — so `BuildPlugin -Rocket` on the distributed descriptor is test-free +
  green, while the `UnrealMcp.` specs still run on every PR. **Never commit the
  descriptor with the test module added.**
- **C3 — FilterPlugin hygiene.** `Config/FilterPlugin.ini` ships the
  `Source/ThirdParty/UnrealMcpBridge/...` tree; no source uses hardcoded/absolute paths; all packaged
  paths are ≤140 chars from the plugin root; the zip excludes `Binaries/Intermediate/Saved/.vs` (Fab strips
  them — the surviving `Source/ThirdParty/UnrealMcpBridge/<rid>/` copy is what the recompile uses).
- **C4 — module `PlatformAllowList` (#187).** Both modules in `UnrealMCP.uplugin` declare
  `"PlatformAllowList": ["Win64","Mac","Linux"]` (desktop-only — console/mobile cannot spawn the .NET
  sidecar), which Fab review expects.

**Manual Fab submission (operator):** prefer the release-produced
`unreal-mcp-plugin-source-<version>.zip` asset — it is the distributed, test-free source form with
the signed `Source/ThirdParty/UnrealMcpBridge/<rid>/` payloads already staged. If you must rebuild it
locally, verify the source zip carries `Source/ThirdParty/UnrealMcpBridge/<rid>/` + a test-free `.uplugin`,
then upload that source zip to Fab. **Do not bump `VersionName` for a Fab submission** unless
it is also a coordinated release (the version is release-pipeline-owned).

**Self-hosted runner Live Coding gotcha (UE 5.8+):** the unattended host-project rebuilds inside
`test_pull_request.yml` / `release.yml` run under the Windows service account on `IVANPC-unreal`.
On UE 5.8, UBT can probe the global Live Coding mutex during that host rebuild and fail with
`UnauthorizedAccessException` on the service account. The workflow therefore passes
`-NoLiveCoding` on those `Build.bat ... -project=<HOST_UPROJECT>` calls. Do not remove that flag
unless the runner model changes and the mutex access is re-verified.

**Graceful degradation:** every sign/notarize step is `if: env.X != ''`-guarded.
A run with no signing secrets still produces **unsigned-but-green** artifacts, so
the pipeline merged and runs green before the secrets were provisioned and begins
signing the moment they are mirrored (see [Signing secrets](#signing-secrets-code-signing--notarization-owner-provisioned) below).

## Versioning — the single source of truth

`UnrealMCP/UnrealMCP.uplugin` `VersionName` is the **single source of truth** for
the release version. `release.yml`'s `check-version` job reads it.
`commands/bump-version.ps1` rewrites the version across all version-bearing
files in one shot (the `.uplugin`, the bridge csproj `<Version>`, and
`cli/package.json` + lockfile):

```powershell
.\commands\bump-version.ps1 -NewVersion "0.2.0"     # add -WhatIf to preview
```

**Never hand-edit one of them alone.** The NuGet pins
(`com.IvanMurzak.ReflectorNet`, `com.IvanMurzak.McpPlugin`) are owned by
the upstream release pipelines — never bump them here.

### The shared MCP server is released separately

The local MCP server is the shared
[GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) (binary
`gamedev-mcp-server`, assets `gamedev-mcp-server-<rid>.zip`) — it is **not built or
released by this repo's pipelines**. The CLI downloads the release pinned by the
`SERVER_VERSION` constant in `cli/src/lib/server-version.ts`, which is independent
of the plugin version and deliberately untouched by `bump-version.ps1`.

**Release-order rule:** a CLI release that bumps `SERVER_VERSION` requires the
corresponding `v<SERVER_VERSION>` GameDev-MCP-Server release (with all 7 RID zips)
to **already exist** — cut/verify the shared release first, then bump the pin here.

## The release gate (why a normal merge never publishes)

`release.yml` runs on every push to `main`, but is **inert unless a deliberate
version bump landed**. The gate, computed in `check-version`:

- `should_release` = **no `v<version>` tag exists yet** AND **this push bumped the
  `VersionName` line** in `UnrealMCP/UnrealMCP.uplugin`.
- `dry_run` = the run is a manual `workflow_dispatch` with `dry_run=true`.
- `run_tests` = `should_release` OR `dry_run`.

Outcomes:

| Event | `run_tests` | Publish (tag / Release / npm) |
| --- | --- | --- |
| Merge that does **not** bump `VersionName` (e.g. a feature or workflow-only PR) | ❌ skip | ❌ skip — **guaranteed no-op** |
| `workflow_dispatch` with `dry_run=true` | ✅ run (rehearsal) | ❌ hard-skipped |
| Merge that **bumps** `VersionName` to a new untagged version | ✅ run | ✅ **real release** |
| `workflow_dispatch` with `dry_run=false` on an untagged version | ✅ run | ✅ real release (escape hatch) |

Publish jobs are gated on `should_release == 'true' && dry_run != 'true'`, so a
dry-run can never publish even though it shares the test/artifact jobs.

## Dry-run procedure (rehearse the release without publishing)

> ⚠️ `workflow_dispatch` requires the workflow to exist **on the default branch**.
> Until this PR merges, `release.yml` is not yet dispatchable. Run the dry-run
> **after** the PR lands on `main`.

```bash
# Rehearse: runs the test fan-out + artifact builds, uploads the zips for
# inspection, and HARD-SKIPS every tag/Release/npm-publish job.
gh workflow run release.yml --repo IvanMurzak/Unreal-MCP -f dry_run=true

# Watch it:
gh run watch --repo IvanMurzak/Unreal-MCP "$(gh run list --repo IvanMurzak/Unreal-MCP --workflow release.yml --limit 1 --json databaseId --jq '.[0].databaseId')"
```

Expected: `bridge`, `test-cli`, `build-bridge-macos`, `build-bridge-windows`
succeed (the bridge build + sign jobs run on every rehearsal; with no secrets
they produce unsigned-but-green artifacts); the `plugin` / `build-plugin-zip`
legs run only if the self-hosted runner is registered (otherwise skipped);
`publish-release` and `publish-npm` are **skipped**. A dry-run is the right way
to confirm the bundle staging works end-to-end: when the runner is ready,
inspect `build-plugin-zip`'s log for the per-RID "Packaged bridge for <rid>: N
file(s)." lines, then download both the `unreal-mcp-plugin-source-zip` artifact
(to confirm `Source/ThirdParty/UnrealMcpBridge/<rid>/` shipped without an
`EngineVersion` pin) and the `unreal-mcp-plugin-packaged-zip` artifact (to
confirm `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` is populated for all four
RIDs).
Verify the `publish-release` and `publish-npm` jobs were skipped. A dry-run should upload artifacts
only; it must not create a new tag, GitHub Release, or npm publish for the current `VersionName`.

## First npm publish (one-time, manual)

> **Why manual?** npm's OIDC Trusted Publishing can only be configured for a
> package that **already exists** on the registry. The very first publish of
> `unreal-mcp-cli` therefore cannot run through `release.yml`'s `publish-npm` job
> (which is OIDC-only, no `NPM_TOKEN`). The owner publishes the first version by
> hand from a clean checkout; afterwards a Trusted Publisher is configured and
> CI handles every subsequent version automatically.

The package name `unreal-mcp-cli` was verified free on the registry (exact
`unreal-mcp-cli`, squashed `unrealmcpcli`, and `unreal-mcpcli` all 404).

> **Why `unreal-mcp-cli` and not `unreal-cli`?** The first manual `npm publish` of
> `unreal-cli` was rejected by npm with E403 — "Package name too similar to
> existing package `unrealcli`" (npm's anti-typosquatting check compares
> punctuation-stripped names and only fires at publish time, so the earlier E404
> availability check could not catch it). The package was renamed to
> `unreal-mcp-cli` (2026-06-11), which also matches the sibling precedent of npm
> `unity-mcp-cli` with bin `unity-mcp-cli`.

The `cli/package.json` is already publish-ready (`private` removed, full metadata,
`publishConfig: { "access": "public" }`). Publish the first
version from a **clean checkout of `main`** (not a dev worktree), authenticated
as the package owner (`baizor`):

```bash
# 1. Authenticate as the npm package owner (interactive; opens a browser).
npm login                       # user: baizor

# 2. From a clean checkout, build and publish the cli package.
cd cli
npm ci
npm run build
npm publish                     # publishConfig sets access=public; no provenance (see note)
```

`npm publish` reads `publishConfig` from `cli/package.json`, so `--access public`
need not be passed explicitly. Confirm with
`npm view unreal-mcp-cli version` (expect `0.1.0`).

> **No provenance on the manual first publish — by design.** npm provenance can
> only be generated from a **cloud-hosted CI runner** (GitHub Actions / GitLab
> CI) with an OIDC identity; a local `npm publish` cannot produce it, and a
> `publishConfig.provenance: true` (or `--provenance`) from a workstation hard-errors.
> So `publishConfig` deliberately does **not** set `provenance`, and this manual
> step publishes without it. Provenance is added automatically for every
> *subsequent* version: `release.yml`'s `publish-npm` job runs
> `npm publish --access public --provenance` over OIDC (npm ≥ 11.5.1). The
> hand-published `0.1.0` simply has no provenance attestation; CI-published
> versions do.

Then, **one-time**, configure the Trusted Publisher so all future versions
publish from CI without a token:

1. On npmjs.com → the `unreal-mcp-cli` package → **Settings → Trusted publishing**.
2. Add a GitHub Actions publisher authorizing:
   - **Repository**: `IvanMurzak/Unreal-MCP`
   - **Workflow**: `release.yml`
   - **Environment**: none (leave blank — `publish-npm` runs without a GitHub
     Environment).
3. See <https://docs.npmjs.com/trusted-publishers>.

After that, every subsequent version is published by `release.yml`'s
`publish-npm` job over OIDC (`id-token: write`, npm ≥ 11.5.1) on a real version
bump — no manual `npm publish` and no stored `NPM_TOKEN`. Do **not** hand-publish
again unless recovering a post-tag failure (see "Re-running a release" below).

> **The first CI release must bump past `0.1.0`.** Because `0.1.0` is
> hand-published above, do not let CI publish `0.1.0` again: `publish-npm` runs
> `npm version <v> --allow-same-version` then `npm publish`, which on the
> already-published `0.1.0` fails with `E403 cannot publish over previously
> published version` (a red `publish-npm` leg). So the **first CI-driven release
> must target a new version** (`0.1.1` / `0.2.0`, via `bump-version.ps1`), not
> `v0.1.0`. (npm will carry a `0.1.0` with no corresponding `v0.1.0` tag/Release
> — expected, not a bug.)

## Self-hosted Unreal runner

The plugin leg (compile + Automation specs) needs local Unreal Engine installs,
which are not available on GitHub-hosted runners. It runs on a **self-hosted
Windows runner** labelled `unreal-5-7` — deliberately distinct from any M1/M4
release-runner label so PR compiles do not starve release capacity. The label is
legacy; the release matrix now validates UE 5.5/5.6/5.7/5.8 on that machine.

> **Status (2026-06-11): the runner is LIVE.** A self-hosted Windows runner
> (labels `self-hosted`, `Windows`, `X64`, `unreal-5-7`) is registered against
> `IvanMurzak/Unreal-MCP`, and the repository variable `UNREAL_RUNNER_READY` is
> set to `true`. `UNREAL_HOST_PROJECT` is **also set**, so the Automation pass
> runs against the packaged plugin (not just the BuildPlugin compile). The
> `plugin BuildPlugin + Automation (UE <ver>)` leg — a
> `matrix.ue: ['5.5', '5.6', '5.7', '5.8']` (the single runner has all supported
> engines installed and runs the legs sequentially; the host game module is
> rebuilt for each matrix engine) — now **runs**
> on every same-repo PR and on real releases — it is no longer skipped. The
> registration steps below are retained for re-provisioning the runner.

### Never red-by-absence

Both plugin jobs (`plugin` in `test_pull_request.yml`/`release.yml` and
`build-plugin-zip` in `release.yml`) are gated on the repository variable
`UNREAL_RUNNER_READY == 'true'`. That variable is currently **`true`**, so the
plugin jobs run. (If it is ever unset — e.g. while re-provisioning the runner —
the plugin jobs are SKIPPED rather than failing, so an absent runner never reds
the hosted legs; the bridge / cli legs always provide PR signal.)

### Fork-PR safety (untrusted code never runs on the self-hosted runner)

The self-hosted runner executes the checked-out code, so a pull request from a
**fork** must never reach it. The `plugin` leg's `if` condition adds
`github.event.pull_request.head.repo.full_name == github.repository`, so the
self-hosted job runs ONLY for same-repo (branch) PRs and manual dispatches —
fork PRs skip it (the hosted bridge / cli legs still give them full
signal).

Defense in depth at the repository-settings level (operator):

- *Settings → Actions → General → Fork pull request workflows*: require approval
  for **all outside collaborators** (or all fork PRs) so no fork workflow runs
  without a maintainer's explicit click.
- Prefer an **ephemeral / just-in-time** self-hosted runner (re-provisioned per
  job, e.g. registered with `--ephemeral`) so one job can never leave persistent
  state on disk for a later job to read. Never register the `unreal-5-7` runner
  on a machine holding secrets you would not hand to an untrusted PR author.

### Registration steps (operator)

1. On a Windows machine with UE 5.5, 5.6, 5.7, and 5.8 installed at the standard
   Epic paths (`C:\Program Files\Epic Games\UE_5.5` through `UE_5.8`), register a
   self-hosted runner against `IvanMurzak/Unreal-MCP`:
   *Settings → Actions → Runners → New self-hosted runner* (Windows x64). Follow
   the `./config.cmd` steps GitHub shows.
2. Give the runner the labels `self-hosted`, `windows`, and **`unreal-5-7`**.
3. Prepare a host `.uproject` on the runner that has the `UnrealMCP` plugin
   available (the analog of the infra `Unreal-Test-Project/`, plugin junctioned
   or copied into its `Plugins/`). Note its absolute path.
4. Set the repository variables (below).
5. Open a throwaway PR (or re-run an existing one) and confirm the `plugin` leg
   now runs and is green.

### Repository variables

Set via `gh variable set --repo IvanMurzak/Unreal-MCP <NAME> --body <VALUE>`
(or *Settings → Secrets and variables → Actions → Variables*):

| Variable | Required | Purpose |
| --- | --- | --- |
| `UNREAL_RUNNER_READY` | to enable the plugin legs | **Currently `true`** — a `unreal-5-7` runner is registered (2026-06-11), so the plugin legs run. Unset/anything-else → plugin legs skip. |
| `UNREAL_ENGINE_PATH` | optional | UE 5.7 install root on the runner. Defaults to `C:\Program Files\Epic Games\UE_5.7`. |
| `UNREAL_HOST_PROJECT` | for the Automation pass | **Currently set (2026-06-11)** — absolute path on the runner to the host `.uproject` (with the UnrealMCP plugin) the Automation specs run against (`C:\actions-runner-unreal\host\UnrealTestProject\UnrealTestProject.uproject`, whose `Plugins\UnrealMCP` junctions to the BuildPlugin package output in the runner workspace temp, so Automation exercises the PR's packaged plugin). |
| `UNREAL_SMOKE_READY` | to enable the `connection-smoke` leg | Gates the live relay round-trip leg (client → `gamedev-mcp-server` → sidecar → plugin → editor → tool, via `scripts/connection_smoke.py --mode custom`). **Separate from `UNREAL_RUNNER_READY`** so the smoke leg stays SKIPPED — never red-by-absence — until validated once on the runner. The leg builds the bridge (→ `UNREAL_MCP_BRIDGE_PATH`), runs its own `BuildPlugin` into the `UNREAL_HOST_PROJECT` junction target (`$RUNNER_TEMP\UnrealMCP-Package`), downloads the public `gamedev-mcp-server` release, and asserts a tool battery. Set to `true` to enable; reuses `UNREAL_HOST_PROJECT` + `UNREAL_ENGINE_PATH`. |

Variables (not secrets) are correct here: none of these values are sensitive.

### The host `Plugins\UnrealMCP` junction — the load-bearing invariant (self-healed)

The host project's `Plugins\UnrealMCP` **must** junction to the per-run BuildPlugin
output `$RUNNER_TEMP\UnrealMCP-Package`, **not** the dev submodule source. Why it
matters: the Automation pass BuildPlugins the plugin **with** the `UnrealMcpEditorTests`
module transiently re-added (`commands/test-module-uplugin.ps1 -Action Add`) into
`$RUNNER_TEMP\UnrealMCP-Package`, so that package — and only that package — carries the
`UnrealMcp.*` specs. The Fab-clean **committed** `UnrealMCP.uplugin` deliberately OMITS
the test module (C2, #139), so if the host junction points at the submodule source the
editor loads a spec-free plugin → **0 specs register → the parse step fails with
"index.json MISSING"** (the crash/no-report case), reding the plugin leg.

Off-workflow dev tooling on the runner (`worktree.py` / `testbed.py` / a manual
`mklink`) can silently repoint this junction at the submodule source, which reddened
every Unreal-MCP PR from 2026-06-30 until it was repointed by hand. To make this
self-recovering, the `plugin` job (in both `test_pull_request.yml` and `release.yml`)
and the `connection-smoke` job now **force-recreate the junction** right after the
Automation-pass BuildPlugin populates `$RUNNER_TEMP\UnrealMCP-Package` and before the
editor launches: it removes any existing reparse point/dir and re-runs
`mklink /J <host>\Plugins\UnrealMCP <package>`, then logs the resolved target. The step
is idempotent — a correctly-pointed junction is simply re-created identically, so a
clobbered junction auto-heals with no operator action.

## Secrets (least-privilege)

The workflows use **no long-lived publish secrets**:

| Secret | Used by | Notes |
| --- | --- | --- |
| `GITHUB_TOKEN` | `publish-release` | Auto-provided by GitHub Actions. Scoped per-job: the whole workflow defaults to `permissions: contents: read`; only `publish-release` elevates to `contents: write` to create the Release. |
| *(none)* — npm | `publish-npm` | npm publish uses **OIDC Trusted Publishing** (`id-token: write`), not a stored `NPM_TOKEN`. |

**npm first-publish prerequisite (owner action):** OIDC Trusted Publishing
requires a Trusted Publisher for the `unreal-mcp-cli` package configured on
npmjs.com that authorizes this repository + the `release.yml` workflow — and a
Trusted Publisher can only be configured for a package that already exists. The
`cli/package.json` `private` flag has already been removed and the package is
publish-ready; the very first version is therefore published **manually** by the
owner (see [First npm publish](#first-npm-publish-one-time-manual)), and only
afterwards does the Trusted Publisher get configured so CI's `publish-npm`
handles every subsequent version. See <https://docs.npmjs.com/trusted-publishers>.

No secret is ever echoed to logs; the sidecar IPC token (`UNREAL_MCP_TOKEN`)
travels via stdin and is unrelated to CI.

### Signing secrets (code-signing + notarization, owner-provisioned)

The bundled bridge is code-signed in `build-bridge-macos` (Apple codesign +
notarize) and `build-bridge-windows` (Azure Trusted Signing). These are
**owner-provisioned** and identical in shape to the GameDev-MCP-Server secret
set. **The Unreal-MCP repo has none of them yet** — every signing step is
`if: env.X != ''`-guarded, so the pipeline merges and runs green producing
**unsigned** artifacts until the owner mirrors them. Releases are unsigned until
provisioned; signing begins automatically on the next release after they exist.

Provision them via `gh secret set --repo IvanMurzak/Unreal-MCP <NAME>` (or
*Settings → Secrets and variables → Actions → Secrets*) with the **same values
as `IvanMurzak/GameDev-MCP-Server`** (GitHub Actions secrets are per-repo — there
is no org-level inheritance):

| Secret | Used by | Purpose |
| --- | --- | --- |
| `MAC_CSC_LINK` | `build-bridge-macos` | Base64 Developer ID Application `.p12` certificate. |
| `MAC_CSC_KEY_PASSWORD` | `build-bridge-macos` | Password for the `.p12`. |
| `MAC_SIGN_IDENTITY` | `build-bridge-macos` | `codesign --sign` identity (Developer ID Application: …). |
| `APPLE_API_KEY_B64` | `build-bridge-macos` | Base64 App Store Connect API key (`.p8`) for `notarytool`. |
| `APPLE_API_KEY_ID` | `build-bridge-macos` | App Store Connect API key id. |
| `APPLE_API_ISSUER` | `build-bridge-macos` | App Store Connect API issuer id. |
| `AZURE_TENANT_ID` | `build-bridge-windows` | Azure Trusted Signing tenant id. |
| `AZURE_CLIENT_ID` | `build-bridge-windows` | Azure Trusted Signing client id (also the guard for the win sign step). |
| `AZURE_CLIENT_SECRET` | `build-bridge-windows` | Azure Trusted Signing client secret. |

Notes:

- The macOS sign + notarize steps are guarded on **all three** mac signing
  secrets together (and notarize additionally on `APPLE_API_KEY_B64`), so a
  partial-secrets state skips the whole signing path cleanly rather than failing
  mid-way. The Windows sign step is guarded on `AZURE_CLIENT_ID`.
- The Azure Trusted Signing account (`ivan-murzak`) and certificate profile
  (`ai-game-dev-cert`) are hard-coded in the workflow (non-sensitive), matching
  GameDev-MCP-Server.
- Signing only the single-file apphost is sufficient for the shipped form: the
  `disable-library-validation` entitlement makes the runtime-extracted native
  libs legal at exec time. The C++ side additionally strips the macOS
  `com.apple.quarantine` xattr at spawn (T2) for the offline-exec case.

## Re-running a release — full rerun only

If a release run fails partway, **always use a full re-run, never "re-run failed
jobs"**. The artifact-build jobs (`build-bridge-macos`, `build-bridge-windows`, `build-plugin-zip`)
upload artifacts that the `publish-release` job downloads; a
partial re-run does not re-run the succeeded build jobs, so their artifacts are
absent on the new attempt and `publish-release` fails with `Artifact not found`.

```bash
# Correct: full re-run (re-runs every job, regenerating all artifacts)
gh run rerun <run-id> --repo IvanMurzak/Unreal-MCP

# WRONG for this pipeline — leaves artifact-producing jobs un-run:
# gh run rerun <run-id> --failed
```

**Post-tag failure (the full re-run will NOT recover it).** If `publish-release`
already created the `v<version>` tag + GitHub Release but the run then failed
(asset-upload error, or the expected `publish-npm` auth failure while no Trusted
Publisher is configured), a full re-run cannot
fix it: `check-version` now sees `tag_exists=true` → `should_release=false` →
every job skips, so CI can never publish that version again. Recover one of two
ways:

```bash
# Option A — delete the tag + Release, then full re-run (CI republishes cleanly):
gh release delete v<version> --repo IvanMurzak/Unreal-MCP --cleanup-tag --yes
gh run rerun <run-id> --repo IvanMurzak/Unreal-MCP

# Option B — keep the tag/Release and publish npm by hand from a clean checkout:
cd cli && npm ci && npm run build && npm publish --access public
```

## Operator gate summary

- A normal merge to `main` publishes **nothing** (the version gate keeps it inert).
- A real release is a **deliberate, operator-driven version bump** (run
  `bump-version.ps1`, commit, merge) — or a manual `workflow_dispatch` with
  `dry_run=false` on an untagged version.
- The dry-run (`dry_run=true`) is the safe rehearsal and the only way the release
  pipeline is exercised before the owner intends to publish.
