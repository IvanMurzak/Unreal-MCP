#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Safe GitHub release operator wrapper for Unreal-MCP

.DESCRIPTION
    Wraps the EXISTING release contract instead of bypassing it:
      - prepare   -> runs commands/bump-version.ps1 locally
      - dry-run   -> dispatches release.yml with dry_run=true
      - publish   -> dispatches release.yml with dry_run=false
      - watch     -> watches the latest release.yml run

    This script never creates tags or GitHub Releases directly and never publishes
    to npm directly. release.yml remains the only publisher.
#>

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('prepare', 'dry-run', 'publish', 'watch')]
    [string]$Mode,

    [string]$NewVersion,

    [string]$Repo = 'IvanMurzak/Unreal-MCP',

    [string]$Ref = 'main',

    [switch]$Wait
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

function Write-Section {
    param([string]$Text)
    Write-Host "`n== $Text ==" -ForegroundColor Cyan
}

function Require-Command {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found on PATH: $Name"
    }
}

function Require-CleanWorkingTree {
    $status = git -C $repoRoot status --porcelain
    if ($LASTEXITCODE -ne 0) {
        throw "git status failed."
    }
    if (-not [string]::IsNullOrWhiteSpace(($status -join "`n"))) {
        throw "Working tree is not clean. Commit or stash changes before running release-github.ps1 -Mode prepare."
    }
}

function Require-GhAuth {
    & gh auth status | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "gh auth status failed. Authenticate gh before dispatching release workflows."
    }
}

function Get-LatestReleaseRunId {
    $runId = & gh run list --repo $Repo --workflow release.yml --limit 1 --json databaseId --jq '.[0].databaseId'
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to query the latest release workflow run."
    }
    return ($runId | Out-String).Trim()
}

function Watch-LatestReleaseRun {
    Write-Section "Watching latest release.yml run"
    Start-Sleep -Seconds 3
    $runId = Get-LatestReleaseRunId
    if ([string]::IsNullOrWhiteSpace($runId)) {
        throw "No release.yml run found to watch."
    }
    & gh run watch $runId --repo $Repo
    if ($LASTEXITCODE -ne 0) {
        throw "gh run watch failed for run $runId."
    }
}

Push-Location $repoRoot
try {
    Require-Command git
    Require-Command gh

    switch ($Mode) {
        'prepare' {
            if ([string]::IsNullOrWhiteSpace($NewVersion)) {
                throw "-NewVersion is required for -Mode prepare."
            }
            Write-Section "Preparing release version bump"
            Require-CleanWorkingTree
            & pwsh -File (Join-Path $scriptDir 'bump-version.ps1') -NewVersion $NewVersion
            if ($LASTEXITCODE -ne 0) {
                throw "bump-version.ps1 failed."
            }
            Write-Host "Version bump prepared locally. Commit it on a release branch, open a PR, and merge to main to let release.yml publish." -ForegroundColor Green
        }

        'dry-run' {
            Write-Section "Dispatching release.yml dry-run"
            Require-GhAuth
            & gh workflow run release.yml --repo $Repo --ref $Ref -f dry_run=true
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to dispatch release.yml dry-run."
            }
            Write-Host "Dry-run dispatched on $Ref." -ForegroundColor Green
            if ($Wait) {
                Watch-LatestReleaseRun
            }
        }

        'publish' {
            Write-Section "Dispatching release.yml publish"
            Require-GhAuth
            & gh workflow run release.yml --repo $Repo --ref $Ref -f dry_run=false
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to dispatch release.yml publish."
            }
            Write-Host "Publish dispatch sent on $Ref. release.yml will still self-gate if the version on main is already tagged or not release-eligible." -ForegroundColor Green
            if ($Wait) {
                Watch-LatestReleaseRun
            }
        }

        'watch' {
            Require-GhAuth
            Watch-LatestReleaseRun
        }
    }
}
finally {
    Pop-Location
}
