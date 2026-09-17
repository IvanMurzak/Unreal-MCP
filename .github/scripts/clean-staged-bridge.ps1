# Author: Ivan Murzak (https://github.com/IvanMurzak)
# Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)
# Copyright (c) 2026 Ivan Murzak
# Licensed under the Apache License, Version 2.0.
# See the LICENSE file in the project root for more information.

<#
.SYNOPSIS
    Removes the gitignored, transiently staged bridge binaries from a persistent self-hosted workspace.

.DESCRIPTION
    The self-hosted Unreal jobs check out with `clean: false`, so untracked files survive between
    jobs on the same runner slot. release.yml's build-plugin-zip stages the self-contained bridge
    into the gitignored UnrealMCP/Source/ThirdParty/UnrealMcpBridge/<rid>/ folder. Left behind, a
    later PR plugin job BuildPlugin-packages those binaries, FUnrealMcpSidecarManager then finds a
    "bundled" bridge, and the "degrades when no bundled binary exists" spec fails.

    Removes ONLY ignored files:
      * UnrealMCP/Source/ThirdParty/UnrealMcpBridge  via `git clean -fdX` (tracked README.md and
        <rid>/.gitkeep are kept: -X never touches tracked or non-ignored files).
      * UnrealMCP/Binaries/ThirdParty                via Remove-Item, after proving nothing under it is
        tracked. Not `git clean`: Binaries/ is ignored as a whole, so git would report and delete the
        entire UnrealMCP/Binaries/ directory even with the narrower pathspec.

    Throws if git fails, if Binaries/ThirdParty holds tracked files, or if an ignored file remains.
#>
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $env:GITHUB_WORKSPACE
$safe = "safe.directory=$env:GITHUB_WORKSPACE"

$bridgeSrc = 'UnrealMCP/Source/ThirdParty/UnrealMcpBridge'
$bridgeBin = 'UnrealMCP/Binaries/ThirdParty'

git -c $safe clean -fdX -- $bridgeSrc
if ($LASTEXITCODE -ne 0) { throw "git clean -fdX -- $bridgeSrc failed (exit $LASTEXITCODE)" }

$tracked = git -c $safe ls-files -- $bridgeBin
if ($LASTEXITCODE -ne 0) { throw "git ls-files -- $bridgeBin failed (exit $LASTEXITCODE)" }
if ($tracked) { throw "Refusing to delete $bridgeBin : it contains tracked files:`n$($tracked -join "`n")" }
if (Test-Path -LiteralPath $bridgeBin) {
    Remove-Item -LiteralPath $bridgeBin -Recurse -Force
    Write-Host "Removed $bridgeBin"
}

$left = git -c $safe status --porcelain=v1 --ignored -- $bridgeSrc $bridgeBin
if ($LASTEXITCODE -ne 0) { throw "git status failed (exit $LASTEXITCODE)" }
$ignoredLeft = @($left | Where-Object { $_ -like '!! *' })
if ($ignoredLeft.Count -gt 0) { throw "Staged bridge files survived the clean:`n$($ignoredLeft -join "`n")" }
Write-Host "Staged bridge binaries cleaned ($bridgeSrc, $bridgeBin)."
