param(
    [Parameter(Mandatory)][string]$ProjectDir,
    [Parameter(Mandatory)][string]$Url,
    [Parameter(Mandatory)][string]$Plugins
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $root 'cli/dist/index.js'
$failures = @()
foreach ($plugin in $Plugins.Split(',')) {
    $checks = @(Get-ChildItem (Join-Path $root "extensions/$plugin/Tests/e2e/tools") -Filter '*.e2e.ps1')
    if ($checks.Count -eq 0) { throw "Missing E2E checks for $plugin" }
    foreach ($file in $checks) {
        $check = & $file.FullName
        $verb = if ($check.System) { 'run-system-tool' } else { 'run-tool' }
        $callArgs = @($cli, $verb, $check.Tool, '--path', $ProjectDir, '--url', $Url)
        if ($check.Input) { $callArgs += @('--input', $check.Input) }
        try {
            $out = & node @callArgs 2>&1
            $code = $LASTEXITCODE
            $text = ($out | Out-String).Trim()
            if ($code -ne 0 -and -not $check.ExpectError) { throw "CLI failed: $text" }
            # A transport failure must not count as a defensive tool error.
            if ($text -match '\(network\)|\(timeout\)|ECONNREFUSED|ENOTFOUND') { throw "Transport failure: $text" }
            $result = $null
            try { $result = $text | ConvertFrom-Json } catch { $result = $text }
            if (-not $check.ExpectError -and ($result -is [string] -or $result.isError -eq $true)) { throw "Invalid success: $text" }
            if ($check.ExpectError -and -not $check.Assert) { throw 'Expected-error checks must assert the specific tool error' }
            if ($check.Assert) { & $check.Assert $result }
            Write-Host "PASS $plugin/$($check.Tool)"
        } catch {
            $failures += "$plugin/$($check.Tool): $_"
            Write-Host "FAIL $($failures[-1])"
        }
    }
}
if ($failures.Count) { throw ($failures -join "`n") }
