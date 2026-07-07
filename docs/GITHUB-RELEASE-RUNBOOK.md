# GitHub release runbook

This is the short operator guide for cutting a new Unreal-MCP GitHub release **without bypassing the existing CI contract**.

The key rule is simple:

- **`commands/bump-version.ps1` owns version edits**
- **`release.yml` owns GitHub Release creation, tag creation, asset upload, and npm publish**
- **`commands/release-github.ps1` is only a safe wrapper around those two pieces**

For the deep release architecture and artifact graph, read [`docs/RELEASING.md`](RELEASING.md). This file is the quick "what do I run?" layer.

## Recommended path

### 1. Rehearse first

Run the release workflow in **dry-run** mode before doing a real release:

```powershell
.\commands\release-github.ps1 -Mode dry-run -Wait
```

That dispatches:

```text
gh workflow run release.yml --ref main -f dry_run=true
```

What it does:

- runs the test + artifact legs
- uploads the plugin/source/bridge artifacts
- **does not** create a tag
- **does not** create a GitHub Release
- **does not** publish `unreal-mcp-cli`

## 2. Prepare the new version

From a clean checkout of the Unreal-MCP repo:

```powershell
.\commands\release-github.ps1 -Mode prepare -NewVersion 0.6.5
```

That runs `commands/bump-version.ps1` and updates the version-bearing files together.

Then:

1. commit the version bump on a release branch
2. open a PR
3. merge it to `main`

When that merge lands, `release.yml` sees the deliberate version bump and performs the real release automatically.

## 3. Optional escape hatch

If the release version is already on `main` and still untagged, you can manually dispatch the real release workflow:

```powershell
.\commands\release-github.ps1 -Mode publish -Wait
```

That dispatches:

```text
gh workflow run release.yml --ref main -f dry_run=false
```

The workflow still self-gates. If the version on `main` is already tagged or not release-eligible, the run will no-op safely.

## What the wrapper script does not do

It intentionally does **not**:

- create GitHub tags directly
- create GitHub Releases directly
- run `npm publish` directly
- merge PRs
- push commits for you

Those actions remain CI-owned or operator-reviewed on purpose.

## Commands summary

### Prepare a version bump

```powershell
.\commands\release-github.ps1 -Mode prepare -NewVersion 0.6.5
```

### Dispatch a rehearsal

```powershell
.\commands\release-github.ps1 -Mode dry-run -Wait
```

### Dispatch a real publish

```powershell
.\commands\release-github.ps1 -Mode publish -Wait
```

### Watch the latest release workflow

```powershell
.\commands\release-github.ps1 -Mode watch
```

## Safety notes

1. `prepare` requires a **clean working tree**.
2. `dry-run` and `publish` require `gh` authentication.
3. Do **not** use `gh run rerun --failed` for release recovery when artifacts are involved; use a full rerun.
4. The preferred real-release path is still:
   - prepare version bump
   - PR
   - merge to `main`
   - let `release.yml` publish

## Recovery

If a release run fails:

1. inspect the failing `release.yml` run
2. fix the underlying issue
3. rerun the **entire** workflow, not `--failed`

If you only need to inspect the latest run:

```powershell
.\commands\release-github.ps1 -Mode watch
```
