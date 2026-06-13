#!/usr/bin/env pwsh
# Publish self-contained single-file unreal-mcp-bridge binaries per RID and zip them
# (docs/ARCHITECTURE.md §6 BUNDLE model, issue #45: unreal-mcp-bridge-<rid>.zip release artifacts).
# Works on Windows, macOS, and Linux with PowerShell Core.
#
# The published binary is SELF-CONTAINED — the end user needs no .NET SDK/runtime installed.
# Trimming is OFF (McpPlugin/ReflectorNet/SignalR are reflection-heavy; see the bridge csproj).
#
# Usage:
#   ./publish.ps1                                          # Release, all 4 RIDs, zipped
#   ./publish.ps1 -Platforms win-x64                       # Release, only win-x64, zipped
#   ./publish.ps1 -Platforms win-x64 -NoZip               # publish dir only (e.g. before signing)
#
# -NoZip skips zipping (T3 Authenticode-signs the raw .exe before re-zip).

param(
    [string]$Configuration = "Release",
    [string]$ProjectFile = "src/com.IvanMurzak.Unreal.MCP.Bridge.csproj",
    [string[]]$Platforms = @(),
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot

$PublishRoot = Join-Path $PSScriptRoot "publish"
if (-not (Test-Path $PublishRoot)) {
    New-Item -ItemType Directory -Path $PublishRoot | Out-Null
}

# The 4 RIDs the bundle model ships (§6).
$allRuntimes = @("win-x64", "linux-x64", "osx-x64", "osx-arm64")
$runtimes = if ($Platforms.Count -gt 0) { $allRuntimes | Where-Object { $_ -in $Platforms } } else { $allRuntimes }

if ($runtimes.Count -eq 0) {
    Write-Host "No valid runtimes selected. Available: $($allRuntimes -join ', ')" -ForegroundColor Red
    Pop-Location
    exit 1
}

$failed = 0
foreach ($runtime in $runtimes) {
    Write-Host "Publishing $runtime ($Configuration)..." -ForegroundColor Yellow
    $outputPath = Join-Path $PublishRoot $runtime
    if (Test-Path $outputPath) {
        Remove-Item $outputPath -Recurse -Force
    }
    dotnet publish $ProjectFile -c $Configuration -r $runtime --self-contained true -p:PublishSingleFile=true -o $outputPath
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Failed to publish $runtime" -ForegroundColor Red
        $failed++
        continue
    }

    if (-not $NoZip) {
        $zipPath = Join-Path $PublishRoot "unreal-mcp-bridge-$runtime.zip"
        if (Test-Path $zipPath) {
            Remove-Item $zipPath -Force
        }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::CreateFromDirectory($outputPath, $zipPath)
        Write-Host "Created $zipPath" -ForegroundColor Green
    }
}

Pop-Location
if ($failed -gt 0) { exit 1 }
Write-Host "All bridge publishes completed. Artifacts in: $PublishRoot" -ForegroundColor Green
