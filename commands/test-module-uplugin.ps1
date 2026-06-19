# Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
# See the LICENSE file in the repository root for more information.

<#
.SYNOPSIS
    Toggle the UnrealMcpEditorTests Automation module in/out of UnrealMCP.uplugin.

.DESCRIPTION
    The committed UnrealMCP.uplugin is the DISTRIBUTED descriptor and deliberately
    OMITS the UnrealMcpEditorTests module -- Fab review flags shipped test/automation
    modules, and BuildPlugin -Rocket must produce a marketplace-clean package
    (issue #139, C2). But PR/dev CI still needs to compile + load that module to run
    the UnrealMcp. Automation specs.

    This script edits the .uplugin's Modules array in place:
      -Action Add     -> append the test module (run BEFORE the Automation pass)
      -Action Remove  -> strip it back out  (run AFTER the Automation pass, in finally)
    Both actions are IDEMPOTENT (Add is a no-op if present; Remove a no-op if absent),
    so a crashed run that skipped the revert never leaves a wedged descriptor -- the
    next Add/Remove (or a `git checkout`) restores the intended state.

    NEVER commit the descriptor with the test module added: this script is a transient
    dev/CI mutation only. The distributed/committed form has NO test module.

.PARAMETER Action
    Add | Remove.

.PARAMETER Uplugin
    Path to UnrealMCP.uplugin. Defaults to the repo-relative path from this script.

.EXAMPLE
    ./commands/test-module-uplugin.ps1 -Action Add
    & $editorCmd $proj -ExecCmds="Automation RunTests UnrealMcp.; Quit" ...
    ./commands/test-module-uplugin.ps1 -Action Remove
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Add', 'Remove')]
    [string]$Action,

    [string]$Uplugin
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Uplugin)) {
    $Uplugin = Join-Path $PSScriptRoot '..\UnrealMCP\UnrealMCP.uplugin'
}
if (-not (Test-Path $Uplugin)) {
    throw "UnrealMCP.uplugin not found at '$Uplugin'."
}

$TestModuleName = 'UnrealMcpEditorTests'

# Preserve ordering + formatting predictability by round-tripping through a typed object.
$json = Get-Content $Uplugin -Raw | ConvertFrom-Json
if ($null -eq $json.Modules) {
    throw "UnrealMCP.uplugin has no Modules array -- refusing to edit."
}

# ConvertFrom-Json yields a fixed-size array; rebuild as a mutable list of modules.
$modules = [System.Collections.Generic.List[object]]::new()
foreach ($m in $json.Modules) { $modules.Add($m) }

$present = $false
foreach ($m in $modules) { if ($m.Name -eq $TestModuleName) { $present = $true; break } }

switch ($Action) {
    'Add' {
        if ($present) {
            Write-Host "[$TestModuleName] already present in $([System.IO.Path]::GetFileName($Uplugin)) -- no change."
        }
        else {
            # Mirror the editor module's shape: Type Editor, LoadingPhase Default.
            $entry = [PSCustomObject]@{
                Name         = $TestModuleName
                Type         = 'Editor'
                LoadingPhase = 'Default'
            }
            $modules.Add($entry)
            Write-Host "[$TestModuleName] ADDED for the dev/CI Automation pass (transient -- do NOT commit)."
        }
    }
    'Remove' {
        if (-not $present) {
            Write-Host "[$TestModuleName] not present -- descriptor already in its distributed form."
        }
        else {
            $kept = [System.Collections.Generic.List[object]]::new()
            foreach ($m in $modules) { if ($m.Name -ne $TestModuleName) { $kept.Add($m) } }
            $modules = $kept
            Write-Host "[$TestModuleName] REMOVED -- descriptor restored to its distributed (Fab-clean) form."
        }
    }
}

$json.Modules = $modules.ToArray()

# Write back as valid JSON. This is a TRANSIENT mutation that the caller reverts
# (-Action Remove, or `git checkout`), so exact indentation does not need to match
# the committed 2-space style -- only that UE can parse it. ConvertTo-Json's default
# formatting is valid .uplugin JSON; the committed/distributed form is restored by
# the paired Remove action. Depth 32 comfortably covers the flat Modules/Plugins arrays.
$out = $json | ConvertTo-Json -Depth 32
# Write UTF-8 WITHOUT a BOM (PS 5.1's -Encoding utf8 emits a BOM; UE tolerates it but
# a BOM-less file keeps the transient diff clean and matches the committed descriptor).
[System.IO.File]::WriteAllText($Uplugin, $out + "`n", (New-Object System.Text.UTF8Encoding($false)))
