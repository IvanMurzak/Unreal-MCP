#!/usr/bin/env pwsh
# Publish self-contained single-file unreal-mcp-bridge binaries per RID and zip them
# (docs/ARCHITECTURE.md §6: unreal-mcp-bridge-<platform>.zip release artifacts).
# Works on Windows, macOS, and Linux with PowerShell Core.

param(
    [string]$Configuration = "Release",
    [string]$ProjectFile = "src/com.IvanMurzak.Unreal.MCP.Bridge.csproj",
    [string[]]$Platforms = @()
)

$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot

$PublishRoot = Join-Path $PSScriptRoot "publish"
if (Test-Path $PublishRoot) {
    Remove-Item $PublishRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $PublishRoot | Out-Null

# The 4 RIDs the sidecar download flow serves (§6).
$allRuntimes = @("win-x64", "linux-x64", "osx-x64", "osx-arm64")
$runtimes = if ($Platforms.Count -gt 0) { $allRuntimes | Where-Object { $_ -in $Platforms } } else { $allRuntimes }

if ($runtimes.Count -eq 0) {
    Write-Host "No valid runtimes selected. Available: $($allRuntimes -join ', ')" -ForegroundColor Red
    Pop-Location
    exit 1
}

$failed = 0
foreach ($runtime in $runtimes) {
    Write-Host "Publishing $runtime..." -ForegroundColor Yellow
    $outputPath = Join-Path $PublishRoot $runtime
    dotnet publish $ProjectFile -c $Configuration -r $runtime --self-contained true -p:PublishSingleFile=true -o $outputPath
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Failed to publish $runtime" -ForegroundColor Red
        $failed++
        continue
    }

    $zipPath = Join-Path $PublishRoot "unreal-mcp-bridge-$runtime.zip"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($outputPath, $zipPath)
    Write-Host "Created $zipPath" -ForegroundColor Green
}

Pop-Location
if ($failed -gt 0) { exit 1 }
Write-Host "All bridge publishes completed. Artifacts in: $PublishRoot" -ForegroundColor Green
