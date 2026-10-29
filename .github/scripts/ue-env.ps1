# Author: Ivan Murzak (https://github.com/IvanMurzak)
# Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)
# Copyright (c) 2026 Ivan Murzak
# Licensed under the Apache License, Version 2.0.
# See the LICENSE file in the project root for more information.

<#
.SYNOPSIS
    Resolves this runner's Unreal Engine install for one engine version and exports it to the job.

.DESCRIPTION
    The self-hosted UE jobs run on any runner carrying the engine's `unreal-<major>-<minor>` label,
    whatever its OS (Windows, macOS or Linux). This step turns the matrix version into the
    runner-local paths every later step uses, and appends them to GITHUB_ENV:

      UE_ROOT      the engine root. `UE_ROOT_<major>_<minor>` (e.g. UE_ROOT_5_8) in the runner's own
                   environment wins; otherwise the Epic Games Launcher default for this OS
                   (Windows `C:\Program Files\Epic Games\UE_x.y`, macOS `/Users/Shared/Epic Games/UE_x.y`).
                   Linux has no launcher default, so a Linux runner must set UE_ROOT_<major>_<minor>.
      UE_PLATFORM  the UBT/UAT target platform of this host: Win64, Mac or Linux.
      UE_RUNUAT    RunUAT.bat (Windows) or RunUAT.sh.
      UE_BUILD     the host's UBT Build script (Build.bat, Mac/Build.sh, Linux/Build.sh).
      UE_EDITOR_CMD  the host's UnrealEditor-Cmd (.exe on Windows).
      CHAIN_PY     UE's bundled Python 3.11, through a space-free link under RUNNER_TEMP. The runner
                   accounts have no Python on PATH, and the engine path can contain spaces, which
                   ${{ env.CHAIN_PY }} (substituted UNQUOTED) would split. It is the hardened
                   chain_feed.py contract: an ABSOLUTE, SPACE-FREE, forward-slashed interpreter path.
      SSL_CERT_FILE  macOS/Linux only, unless already set: the OS CA bundle. UE's bundled Python
                   ships no CA certificates there, so its HTTPS downloads would fail to verify.
      DEVELOPER_DIR  macOS only, and only when the machine's selected Xcode is outside the engine's
                   supported range: a side-by-side /Applications/Xcode*.app that fits (see below).

    The link is a directory junction on Windows and a symlink elsewhere. The job's cleanup step
    removes it with `-RemoveLink`, never with a recursive delete: that would follow the link into
    the engine install.
#>
[CmdletBinding(DefaultParameterSetName = 'Resolve')]
param(
    # The matrix engine version, `<major>.<minor>`.
    [Parameter(Mandatory = $true, ParameterSetName = 'Resolve')]
    [ValidatePattern('^\d+\.\d+$')]
    [string] $Version,

    # Remove only the CHAIN_PY link under RUNNER_TEMP (best effort, never fails).
    [Parameter(Mandatory = $true, ParameterSetName = 'RemoveLink')]
    [switch] $RemoveLink
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$link = Join-Path $env:RUNNER_TEMP 'chain-py'
# A LINK only: rmdir / unlink remove the junction or symlink, never the engine files behind it.
function Remove-Link {
    if (-not (Test-Path -LiteralPath $link)) { return }
    if ($IsWindows) { cmd /c rmdir "$link" 2>$null } else { [IO.File]::Delete($link) }
}

if ($RemoveLink) {
    try { Remove-Link } catch { Write-Host "ue-env: could not remove $link ($($_.Exception.Message))" }
    exit 0
}

function Add-JobEnv([string] $Name, [string] $Value) {
    "$Name=$Value" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
    Write-Host "ue-env: $Name=$Value"
}

if ($IsWindows) { $platform = 'Win64'; $defaultRoot = "C:\Program Files\Epic Games\UE_$Version"; $pyExe = 'python.exe'; $buildScript = 'Build.bat'; $editorExe = 'UnrealEditor-Cmd.exe' }
elseif ($IsMacOS) { $platform = 'Mac'; $defaultRoot = "/Users/Shared/Epic Games/UE_$Version"; $pyExe = 'bin/python3'; $buildScript = 'Mac/Build.sh'; $editorExe = 'UnrealEditor-Cmd' }
else { $platform = 'Linux'; $defaultRoot = $null; $pyExe = 'bin/python3'; $buildScript = 'Linux/Build.sh'; $editorExe = 'UnrealEditor-Cmd' }

$overrideName = 'UE_ROOT_' + $Version.Replace('.', '_')
$root = [Environment]::GetEnvironmentVariable($overrideName)
if (-not $root) { $root = $defaultRoot }
if (-not $root) { throw "ue-env: no default UE $Version location on this OS; set $overrideName in the runner's environment" }

$build = Join-Path $root 'Engine/Build/Build.version'
if (-not (Test-Path -LiteralPath $build -PathType Leaf)) { throw "ue-env: UE $Version not found at $root (no Engine/Build/Build.version)" }
$installed = Get-Content -LiteralPath $build -Raw | ConvertFrom-Json
if ("$($installed.MajorVersion).$($installed.MinorVersion)" -ne $Version) {
    throw "ue-env: $root is UE $($installed.MajorVersion).$($installed.MinorVersion), not the requested $Version"
}

# macOS: an out-of-range selected Xcode is swapped for a fitting side-by-side one, for this job only
# (UBT finds Xcode with `xcode-select -p`, which honours DEVELOPER_DIR). See docs/RELEASING.md "Which OS".
$developerDir = $null
if ($IsMacOS) {
    $sdk = Get-Content -LiteralPath (Join-Path $root 'Engine/Config/Apple/Apple_SDK.json') -Raw | ConvertFrom-Json
    # Three-part on both sides: [version]"15.2" (Build -1) would sort below "15.2.0".
    function ConvertTo-Xcode3([version] $V) { [version]::new($V.Major, $V.Minor, [Math]::Max($V.Build, 0)) }
    $minXcode = ConvertTo-Xcode3 $sdk.MinVersion
    $maxXcode = ConvertTo-Xcode3 $sdk.MaxVersion
    function Get-XcodeVersion([string] $App) {
        $v = & /usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' (Join-Path $App 'Contents/Info.plist') 2>$null
        if ($LASTEXITCODE -eq 0 -and "$v" -match '^\d+(\.\d+){1,3}$') { return ConvertTo-Xcode3 "$v" }
        return $null
    }
    function Test-InRange($V) { $null -ne $V -and $V -ge $minXcode -and $V -le $maxXcode }
    # No developer directory selected (xcode-select exits non-zero, prints nothing): treat as out of range.
    $selectedDev = (& xcode-select -p 2>$null) | Select-Object -First 1
    $selectedApp = if ($selectedDev) { Split-Path -Parent (Split-Path -Parent $selectedDev) } else { '(none)' }
    $selectedVersion = if ($selectedDev) { Get-XcodeVersion $selectedApp } else { $null }
    if (-not (Test-InRange $selectedVersion)) {
        $pick = Get-ChildItem -Path '/Applications' -Filter 'Xcode*.app' -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { [pscustomobject]@{ App = $_.FullName; Version = Get-XcodeVersion $_.FullName } } |
            Where-Object { Test-InRange $_.Version } | Sort-Object Version -Descending | Select-Object -First 1
        if (-not $pick) {
            throw "ue-env: UE $Version needs Xcode $minXcode..$maxXcode, the selected one is $selectedVersion ($selectedApp) and no /Applications/Xcode*.app fits; install one next to it (see docs/RELEASING.md)"
        }
        $developerDir = Join-Path $pick.App 'Contents/Developer'
        Write-Host "ue-env: selected Xcode $selectedVersion is outside UE $Version's $minXcode..$maxXcode; this job uses Xcode $($pick.Version) ($($pick.App))"
    }
}

# GetFullPath: one separator style on Windows (cmd's mklink and the .bat get backslashes only).
$runUAT = [IO.Path]::GetFullPath((Join-Path $root "Engine/Build/BatchFiles/RunUAT.$(if ($IsWindows) { 'bat' } else { 'sh' })"))
if (-not (Test-Path -LiteralPath $runUAT -PathType Leaf)) { throw "ue-env: $runUAT not found" }
$ueBuild = [IO.Path]::GetFullPath((Join-Path $root "Engine/Build/BatchFiles/$buildScript"))
$editorCmd = [IO.Path]::GetFullPath((Join-Path $root "Engine/Binaries/$platform/$editorExe"))
foreach ($f in $ueBuild, $editorCmd) { if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "ue-env: $f not found" } }
$caBundle = $null
if (-not $IsWindows -and -not $env:SSL_CERT_FILE) {
    $caBundle = @('/etc/ssl/cert.pem', '/etc/ssl/certs/ca-certificates.crt') | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}

# Chain python: link the bundled Python folder (not just the binary, so it finds its stdlib).
$src = [IO.Path]::GetFullPath((Join-Path $root "Engine/Binaries/ThirdParty/Python3/$platform"))
if (-not (Test-Path -LiteralPath (Join-Path $src $pyExe) -PathType Leaf)) { throw "ue-env: UE bundled Python not found under $src" }
if ($env:RUNNER_TEMP -match '\s') { throw "ue-env: RUNNER_TEMP '$env:RUNNER_TEMP' contains whitespace; CHAIN_PY must be space-free" }
Remove-Link
if (Test-Path -LiteralPath $link) { throw "ue-env: could not remove stale $link" }
if ($IsWindows) {
    cmd /c mklink /J "$link" "$src" | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "ue-env: mklink /J $link -> $src failed (exit $LASTEXITCODE)" }
}
else { New-Item -ItemType SymbolicLink -Path $link -Target $src | Out-Null }
$py = (Join-Path $link $pyExe).Replace('\', '/')
if ($py -notmatch '^([A-Za-z]:)?/\S+\z' -or -not (Test-Path -LiteralPath $py -PathType Leaf)) { throw "ue-env: CHAIN_PY '$py' is not an absolute, space-free interpreter path" }
# Prove it RUNS with the same probe the print-fragment resolvers use (C-extension imports + 3.9+).
$global:LASTEXITCODE = $null
$exe = & $py -c 'import sys, select, zlib, hashlib; sys.version_info >= (3, 9) or sys.exit(1); print(sys.executable.replace(chr(92), chr(47)))'
if ($LASTEXITCODE -ne 0) { throw "ue-env: $py is not a runnable Python 3.9+ (exit $LASTEXITCODE)" }
Write-Host "ue-env: probe sys.executable=$((@($exe) -join ' ').Trim())"

Add-JobEnv 'UE_ROOT' $root
Add-JobEnv 'UE_PLATFORM' $platform
Add-JobEnv 'UE_RUNUAT' $runUAT
Add-JobEnv 'UE_BUILD' $ueBuild
Add-JobEnv 'UE_EDITOR_CMD' $editorCmd
Add-JobEnv 'CHAIN_PY' $py
if ($developerDir) { Add-JobEnv 'DEVELOPER_DIR' $developerDir }
if ($caBundle) { Add-JobEnv 'SSL_CERT_FILE' $caBundle }
