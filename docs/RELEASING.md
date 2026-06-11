# Releasing Unreal-MCP

This document is the operator runbook for the CI/CD workflows under
`.github/workflows/`. It covers the PR test pipeline, the gated release pipeline,
the self-hosted UE 5.7 runner, the dry-run rehearsal procedure, and the required
secrets / repository variables.

The authoritative design is [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §9
(repo / versioning / tests / CI). This file is the operational complement —
keep the two in lockstep, and keep the CI command surface 1:1 with the
implement-task profile `test.md` (infra repo).

## Release checklist — operator-gated leaves (NONE fired automatically)

The following actions are **deliberately operator-gated** and are **not** performed by a normal
merge, by CI on a feature/docs PR, or by any automated task. As of this writing **none of them has
fired** — Unreal-MCP has no GitHub Release, no `v*` tag, no published npm package, and no Fab
listing. Each requires a deliberate human decision (and, for the first release, one-time setup):

- [ ] **First GitHub Release + `v<version>` tag.** Fires only when a deliberate `VersionName` bump
      lands on `main` (run `commands/bump-version.ps1`, commit, merge) on an untagged version — or a
      manual `workflow_dispatch` with `dry_run=false`. `release.yml`'s `check-version` gate keeps
      this inert otherwise. Verify with `gh release list` (expect empty) and
      `git ls-remote --tags` (expect no `v*` tags) until you intend to publish.
- [ ] **First npm `unreal-cli` publish (one-time, manual — operator only).** The first publish of a
      brand-new package **cannot** use OIDC Trusted Publishing — npm requires the package to already
      exist on the registry before a Trusted Publisher can be configured for it. So the first publish
      is a **manual operator step from a clean checkout**; CI takes over for every subsequent version.
      The `cli/package.json` `private` flag has already been removed and the publish metadata
      completed (issue #33 prep). The runbook is **[First npm publish](#first-npm-publish-one-time-manual)** below — do NOT run it from CI.
- [ ] **Fab (Epic marketplace) submission.** Entirely manual and **out of scope of every CI
      workflow** — the release pipeline produces an engine-agnostic source-plugin zip
      (BuildPlugin output), but no job submits to Fab. Fab carries its own metadata/screenshot
      requirements and an Epic review; do it by hand when the listing is ready.

A normal merge to `main` publishes nothing; the version gate keeps `release.yml` inert. The safe
rehearsal is a `dry_run=true` dispatch (below), which exercises the test + artifact jobs and
hard-skips every tag/Release/npm-publish job.

## Workflows at a glance

| Workflow | Trigger | What it does |
| --- | --- | --- |
| `test_pull_request.yml` | `pull_request` to `main` (+ manual) | Fans out the PR test legs: bridge build+xUnit (ubuntu + windows), server build (ubuntu), cli node 20/22, and — when a runner is registered — the UE 5.7 plugin BuildPlugin + Automation leg. |
| `test_cli.yml` | `workflow_call` (reusable) | Builds + tests `unreal-cli` on Node 20 & 22. Called by both `test_pull_request.yml` and `release.yml`. |
| `release.yml` | `push` to `main` (+ manual `workflow_dispatch`) | Version-gated release: builds bridge/server/plugin artifacts and (only on a real version bump) cuts the GitHub Release + tag and publishes `unreal-cli` to npm. Exposes a `dry_run` input to rehearse everything without publishing. |

## Versioning — the single source of truth

`UnrealMCP/UnrealMCP.uplugin` `VersionName` is the **single source of truth** for
the release version. `release.yml`'s `check-version` job reads it.
`commands/bump-version.ps1` rewrites the version across all five version-bearing
files in one shot (the `.uplugin`, both csproj `<Version>`s, `server.json`, and
`cli/package.json` + lockfile):

```powershell
.\commands\bump-version.ps1 -NewVersion "0.2.0"     # add -WhatIf to preview
```

**Never hand-edit one of the five alone.** The NuGet pins
(`com.IvanMurzak.ReflectorNet`, `com.IvanMurzak.McpPlugin[.Server]`) are owned by
the upstream release pipelines — never bump them here.

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

Expected: `bridge`, `test-cli`, `build-bridge-zips`, `build-server-zips` succeed;
the `plugin` / `build-plugin-zip` legs run only if the self-hosted runner is
registered (otherwise skipped); `publish-release` and `publish-npm` are
**skipped**. Verify nothing was published:

```bash
gh release list --repo IvanMurzak/Unreal-MCP     # expect: empty
git ls-remote --tags https://github.com/IvanMurzak/Unreal-MCP   # expect: no v* tags
```

## First npm publish (one-time, manual)

> **Why manual?** npm's OIDC Trusted Publishing can only be configured for a
> package that **already exists** on the registry. The very first publish of
> `unreal-cli` therefore cannot run through `release.yml`'s `publish-npm` job
> (which is OIDC-only, no `NPM_TOKEN`). The owner publishes the first version by
> hand from a clean checkout; afterwards a Trusted Publisher is configured and
> CI handles every subsequent version automatically.

The package name `unreal-cli` was verified free on the registry. The
`cli/package.json` is already publish-ready (`private` removed, full metadata,
`publishConfig: { access: "public", provenance: true }`). Publish the first
version from a **clean checkout of `main`** (not a dev worktree), authenticated
as the package owner (`baizor`):

```bash
# 1. Authenticate as the npm package owner (interactive; opens a browser).
npm login                       # user: baizor

# 2. From a clean checkout, build and publish the cli package.
cd cli
npm ci
npm run build
npm publish                     # publishConfig already sets access=public + provenance
```

`npm publish` reads `publishConfig` from `cli/package.json`, so `--access public`
and `--provenance` need not be passed explicitly. Confirm with
`npm view unreal-cli version` (expect `0.1.0`).

Then, **one-time**, configure the Trusted Publisher so all future versions
publish from CI without a token:

1. On npmjs.com → the `unreal-cli` package → **Settings → Trusted Publisher**.
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

## Self-hosted UE 5.7 runner

The plugin leg (compile + Automation specs) needs Unreal Engine 5.7, which is
not available on GitHub-hosted runners. It runs on a **self-hosted Windows
runner** labelled `unreal-5-7` — deliberately distinct from any M1/M4
release-runner label so PR compiles do not starve release capacity.

> **Status (2026-06-11): the runner is LIVE.** A self-hosted Windows runner
> (labels `self-hosted`, `Windows`, `X64`, `unreal-5-7`) is registered against
> `IvanMurzak/Unreal-MCP`, and the repository variable `UNREAL_RUNNER_READY` is
> set to `true`. The `plugin BuildPlugin + Automation (UE 5.7)` leg now **runs**
> on every same-repo PR and on real releases — it is no longer skipped. The
> registration steps below are retained for re-provisioning the runner.

### Never red-by-absence

Both plugin jobs (`plugin` in `test_pull_request.yml`/`release.yml` and
`build-plugin-zip` in `release.yml`) are gated on the repository variable
`UNREAL_RUNNER_READY == 'true'`. That variable is currently **`true`**, so the
plugin jobs run. (If it is ever unset — e.g. while re-provisioning the runner —
the plugin jobs are SKIPPED rather than failing, so an absent runner never reds
the hosted legs; the bridge / server / cli legs always provide PR signal.)

### Fork-PR safety (untrusted code never runs on the self-hosted runner)

The self-hosted runner executes the checked-out code, so a pull request from a
**fork** must never reach it. The `plugin` leg's `if` condition adds
`github.event.pull_request.head.repo.full_name == github.repository`, so the
self-hosted job runs ONLY for same-repo (branch) PRs and manual dispatches —
fork PRs skip it (the hosted bridge / server / cli legs still give them full
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

1. On a Windows machine with UE 5.7 installed at `C:\Program Files\Epic Games\UE_5.7`
   (or elsewhere — see the `UNREAL_ENGINE_PATH` variable below), register a
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
| `UNREAL_HOST_PROJECT` | for the Automation pass | Absolute path on the runner to the host `.uproject` (with the UnrealMCP plugin) the Automation specs run against. |

Variables (not secrets) are correct here: none of these values are sensitive.

## Secrets (least-privilege)

The workflows use **no long-lived publish secrets**:

| Secret | Used by | Notes |
| --- | --- | --- |
| `GITHUB_TOKEN` | `publish-release` | Auto-provided by GitHub Actions. Scoped per-job: the whole workflow defaults to `permissions: contents: read`; only `publish-release` elevates to `contents: write` to create the Release. |
| *(none)* — npm | `publish-npm` | npm publish uses **OIDC Trusted Publishing** (`id-token: write`), not a stored `NPM_TOKEN`. |

**npm first-publish prerequisite (owner action):** OIDC Trusted Publishing
requires a Trusted Publisher for the `unreal-cli` package configured on
npmjs.com that authorizes this repository + the `release.yml` workflow — and a
Trusted Publisher can only be configured for a package that already exists. The
`cli/package.json` `private` flag has already been removed and the package is
publish-ready; the very first version is therefore published **manually** by the
owner (see [First npm publish](#first-npm-publish-one-time-manual)), and only
afterwards does the Trusted Publisher get configured so CI's `publish-npm`
handles every subsequent version. See <https://docs.npmjs.com/trusted-publishers>.

No secret is ever echoed to logs; the sidecar IPC token (`UNREAL_MCP_TOKEN`)
travels via stdin and is unrelated to CI.

## Re-running a release — full rerun only

If a release run fails partway, **always use a full re-run, never "re-run failed
jobs"**. The artifact-build jobs (`build-bridge-zips`, `build-server-zips`,
`build-plugin-zip`) upload artifacts that the `publish-release` job downloads; a
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
Publisher is configured / `private: true` is still set), a full re-run cannot
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
