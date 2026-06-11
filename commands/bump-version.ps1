#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Automated version bumping script for the Unreal-MCP mono-repo

.DESCRIPTION
    Updates the single shared semver across all version-bearing files (design
    docs/ARCHITECTURE.md §9.2): the UE plugin .uplugin VersionName, the bridge
    csproj <Version>, and the cli npm package (package.json + package-lock.json).
    Prevents human errors from hand-editing one of them alone. Supports preview
    mode for safe testing.

    NOTE: the consumed GameDev-MCP-Server version (cli/src/lib/server-version.ts
    SERVER_VERSION) is deliberately NOT touched — the shared server is released
    from its own repo and its version is independent of the plugin version.

.PARAMETER NewVersion
    The new version number in semver format (e.g., "0.2.0")

.PARAMETER WhatIf
    Preview changes without applying them

.EXAMPLE
    .\bump-version.ps1 -NewVersion "0.2.0"

.EXAMPLE
    .\bump-version.ps1 -NewVersion "0.2.0" -WhatIf
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$NewVersion,

    [switch]$WhatIf
)

# Set location to repository root (parent of commands folder)
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
Push-Location $repoRoot

$ErrorActionPreference = "Stop"

# Version file locations (relative to repo root). The .uplugin VersionName is the
# version single-source — Get-CurrentVersion reads it.
$VersionFiles = @(
    @{
        Path        = "UnrealMCP/UnrealMCP.uplugin"
        Pattern     = '"VersionName":\s*"[\d\.]+"'
        Replace     = '"VersionName": "{VERSION}"'
        Description = "UE plugin .uplugin VersionName"
    },
    @{
        Path        = "bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj"
        Pattern     = '<Version>[\d\.]+</Version>'
        Replace     = '<Version>{VERSION}</Version>'
        Description = "Bridge csproj XML version"
    },
    @{
        Path        = "cli/package.json"
        Pattern     = '"version":\s*"[\d\.]+(-[a-zA-Z0-9\-\.]+)?(\+[a-zA-Z0-9\-\.]+)?"'
        Replace     = '"version": "{VERSION}"'
        Description = "CLI npm package version"
    }
)

function Write-ColorText {
    param([string]$Text, [string]$Color = "White")
    Write-Host $Text -ForegroundColor $Color
}

function Test-SemanticVersion {
    param([string]$Version)

    if ([string]::IsNullOrWhiteSpace($Version)) {
        return $false
    }

    # Basic semver pattern: major.minor.patch (with optional prerelease/build)
    $pattern = '^\d+\.\d+\.\d+(-[a-zA-Z0-9\-\.]+)?(\+[a-zA-Z0-9\-\.]+)?$'
    return $Version -match $pattern
}

function Get-CurrentVersion {
    # Extract current version from the .uplugin (the version single-source)
    $upluginPath = "UnrealMCP/UnrealMCP.uplugin"
    if (-not (Test-Path $upluginPath)) {
        throw "Could not find .uplugin at: $upluginPath"
    }

    $content = Get-Content $upluginPath -Raw
    if ($content -match '"VersionName":\s*"([\d\.]+)"') {
        return $Matches[1]
    }

    throw "Could not extract current VersionName from $upluginPath"
}

function Update-VersionFiles {
    param([string]$OldVersion, [string]$NewVersion, [bool]$PreviewOnly = $false)

    $changes = @()

    foreach ($file in $VersionFiles) {
        $fullPath = $file.Path

        if (-not (Test-Path $fullPath)) {
            Write-ColorText "[warn] File not found: $($file.Path)" "Yellow"
            continue
        }

        $content = Get-Content $fullPath -Raw
        $originalContent = $content

        # Create the replacement string
        $replacement = $file.Replace -replace '\{VERSION\}', $NewVersion

        # Apply the replacement
        $newContent = $content -replace $file.Pattern, $replacement

        # Check if any changes were made
        if ($originalContent -ne $newContent) {
            # Count matches for reporting
            $regexMatches = [regex]::Matches($originalContent, $file.Pattern)

            $changes += @{
                Path            = $file.Path
                Description     = $file.Description
                Matches         = $regexMatches.Count
                Content         = $newContent
                OriginalContent = $originalContent
            }

            Write-ColorText "[edit] $($file.Description): $($regexMatches.Count) occurrence(s)" "Green"

            # Show the actual changes
            foreach ($match in $regexMatches) {
                $newValue = $match.Value -replace $file.Pattern, $replacement
                Write-ColorText "   $($match.Value) -> $newValue" "Gray"
            }
        }
        else {
            Write-ColorText "[warn] No matches found in: $($file.Path)" "Yellow"
            Write-ColorText "   Pattern: $($file.Pattern)" "Gray"
        }
    }

    if ($changes.Count -eq 0) {
        Write-ColorText "[fail] No version references found to update!" "Red"
        Pop-Location
        exit 1
    }

    if ($PreviewOnly) {
        Write-ColorText "`nPreview Summary:" "Cyan"
        Write-ColorText "Files to be modified: $($changes.Count)" "White"
        Write-ColorText "Total replacements: $(($changes | Measure-Object -Property Matches -Sum).Sum)" "White"
        return $null
    }

    # Apply changes
    foreach ($change in $changes) {
        $fullPath = $change.Path
        Set-Content -Path $fullPath -Value $change.Content -NoNewline
    }

    return $changes
}

# Main execution
try {
    Write-ColorText "Unreal-MCP Version Bump Script" "Cyan"
    Write-ColorText "==============================" "Cyan"

    # Validate semantic version format
    if (-not (Test-SemanticVersion $NewVersion)) {
        Write-ColorText "[fail] Invalid semantic version format: $NewVersion" "Red"
        Write-ColorText "Expected format: major.minor.patch (e.g., '1.2.3')" "Yellow"
        Pop-Location
        exit 1
    }

    # Get current version
    $currentVersion = Get-CurrentVersion
    Write-ColorText "Current version: $currentVersion" "White"
    Write-ColorText "New version:     $NewVersion" "White"

    if ($currentVersion -eq $NewVersion) {
        Write-ColorText "[warn] New version is the same as current version" "Yellow"
        Pop-Location
        exit 0
    }

    Write-ColorText "`nScanning for version references..." "Cyan"

    # Update version files
    $changes = Update-VersionFiles -OldVersion $currentVersion -NewVersion $NewVersion -PreviewOnly $WhatIf

    if ($WhatIf) {
        Write-ColorText "`nPreview completed. Use without -WhatIf to apply changes." "Green"
        Pop-Location
        exit 0
    }

    if ($changes -and $changes.Count -gt 0) {
        # Update CLI package-lock.json — only the two root-level version fields
        # that follow "name": "unreal-mcp-cli". Dependency versions are not touched.
        $lockFilePath = "cli/package-lock.json"
        if (Test-Path $lockFilePath) {
            Write-ColorText "`nUpdating CLI package-lock.json..." "Cyan"
            $lockContent = Get-Content $lockFilePath -Raw
            $pattern = '("name":\s*"unreal-mcp-cli",\s+"version":\s*")[\d\.]+(-[a-zA-Z0-9\-\.]+)?(\+[a-zA-Z0-9\-\.]+)?'
            $replacement = "`${1}$NewVersion"
            $newLockContent = [regex]::Replace($lockContent, $pattern, $replacement)
            if ($newLockContent -ne $lockContent) {
                $lockMatches = [regex]::Matches($lockContent, $pattern)
                Set-Content -Path $lockFilePath -Value $newLockContent -NoNewline
                Write-ColorText "   CLI package-lock.json updated ($($lockMatches.Count) occurrences)" "Green"
            }
        }

        Write-ColorText "`nVersion bump completed successfully!" "Green"
        Write-ColorText "   Updated $($changes.Count) files" "White"
        Write-ColorText "   Total replacements: $(($changes | Measure-Object -Property Matches -Sum).Sum)" "White"
        Write-ColorText "   Version: $currentVersion -> $NewVersion" "White"
        Write-ColorText "`nRemember to commit these changes to git" "Cyan"
    }

    Pop-Location
}
catch {
    Write-ColorText "`n[fail] Script failed: $($_.Exception.Message)" "Red"
    Pop-Location
    exit 1
}
