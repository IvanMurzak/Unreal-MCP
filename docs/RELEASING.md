# Releasing Unreal-MCP

This document is the operator runbook for the CI/CD workflows under
`.github/workflows/`. It covers the PR test pipeline, the gated release pipeline,
the self-hosted UE 5.7 runner, the dry-run rehearsal procedure, and the required
secrets / repository variables.

The authoritative design is [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §9
(repo / versioning / tests / CI). This file is the operational complement —
keep the two in lockstep, and keep the CI command surface 1:1 with the
implement-task profile `test.md` (infra repo).

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

## Self-hosted UE 5.7 runner

The plugin leg (compile + Automation specs) needs Unreal Engine 5.7, which is
not available on GitHub-hosted runners. It runs on a **self-hosted Windows
runner** labelled `unreal-5-7` — deliberately distinct from any M1/M4
release-runner label so PR compiles do not starve release capacity.

### Never red-by-absence

Both plugin jobs (`plugin` in `test_pull_request.yml`/`release.yml` and
`build-plugin-zip` in `release.yml`) are gated on the repository variable
`UNREAL_RUNNER_READY == 'true'`. **While that variable is unset, the plugin jobs
are SKIPPED** — a skipped job does not fail a PR, so an unregistered runner never
reds the hosted legs. The bridge / server / cli legs always provide PR signal.

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
| `UNREAL_RUNNER_READY` | to enable the plugin legs | Set to `true` once a `unreal-5-7` runner is registered. Unset/anything-else → plugin legs skip. |
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
npmjs.com that authorizes this repository + the `release.yml` workflow. Until
that is set up — and `cli/package.json`'s `private: true` flag is removed in the
bump commit that cuts the first release — `npm publish` fails auth and nothing is
published. See <https://docs.npmjs.com/trusted-publishers>.

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

## Operator gate summary

- A normal merge to `main` publishes **nothing** (the version gate keeps it inert).
- A real release is a **deliberate, operator-driven version bump** (run
  `bump-version.ps1`, commit, merge) — or a manual `workflow_dispatch` with
  `dry_run=false` on an untagged version.
- The dry-run (`dry_run=true`) is the safe rehearsal and the only way the release
  pipeline is exercised before the owner intends to publish.
