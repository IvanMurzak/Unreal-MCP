# Author: Ivan Murzak (https://github.com/IvanMurzak)
# Repository: GitHub (https://github.com/IvanMurzak/Unreal-MCP)
# Copyright (c) 2026 Ivan Murzak
# Licensed under the Apache License, Version 2.0.
# See the LICENSE file in the project root for more information.

<#
.SYNOPSIS
    Machine-wide mutex for the self-hosted Unreal Engine CI jobs.

.DESCRIPTION
    runner-manager registers ephemeral `runner-manager-*` runners on each Unreal machine: one for
    Unreal-MCP plus other repos' runners (the Unreal-AI-* extensions) and local runs. They share one
    UE install per engine (AutomationTool refuses a second instance: "A conflicting instance of
    AutomationTool is already running") and the fixed smoke port 18080. A workflow `concurrency:`
    group cannot serialize them: the run-level group is keyed per PR, ref or chain lock, and a
    job-level group keeps only ONE pending job and cancels the older one.
    See docs/RELEASING.md "Machine-wide UE lock".

    The lock is an exclusive OS file handle on a fixed machine path: a share-Read handle on Windows,
    and on macOS/Linux a share-None handle, which .NET backs with an exclusive flock() (a share-Read
    handle takes no lock there at all). It is the same flock() scripts/extension_suite.py takes for a
    local run, so CI and local runs exclude each other on every OS. A job must keep holding it
    across several steps, and every step is its own process, so a small detached HOLDER process owns
    the handle:

      Acquire  starts the holder, streams its log into the step, and returns once the holder has the
               lock. It throws when the bounded wait expires or the lock cannot be opened.
      Release  (an `if: always()` step) signals the holder, waits for it to exit, and never fails.
      Hold     is the holder itself. It is internal; the workflow never calls it.

    A job that dies without running Release cannot wedge the machine: the OS releases the handle
    when the holder exits, and the holder exits when the job's Runner.Worker process is gone, when
    the runner's orphan-process cleanup kills it at job end, or after -MaxHoldMinutes.

    The holder must inherit none of the step's handles: a child that inherited the step's stdout pipe
    would keep the pipe open and the runner would wait on the Acquire step forever. On Windows it is
    started through ShellExecute, which inherits no handles; elsewhere its standard streams are
    redirected to pipes of its own (.NET opens every other descriptor close-on-exec).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Acquire', 'Release', 'Hold')]
    [string] $Action,

    # One fixed path for every runner on the machine, whatever its account, workspace, or engine.
    # Default: C:\ProgramData\UnrealMCP-ci\ue.lock on Windows, /var/tmp/UnrealMCP-ci/ue.lock elsewhere.
    [string] $LockPath = '',

    # The bounded wait. The job's timeout-minutes must exceed this plus the job's own work, because
    # a job timeout is a GitHub-side cancel, which can wedge a self-hosted runner.
    [int] $WaitMinutes = 60,

    # Backstop only: releases a holder whose job vanished and whose worker could not be resolved.
    [int] $MaxHoldMinutes = 360,

    # Per-job handshake folder. The default lives under RUNNER_TEMP, which no other job shares.
    [string] $StateDir = '',

    # The process whose exit releases the lock. 0 makes Acquire resolve the job's Runner.Worker.
    [int] $WatchPid = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

if ([string]::IsNullOrWhiteSpace($StateDir)) {
    $base = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
    $StateDir = Join-Path $base 'ue-machine-lock'
}
# A trailing backslash before the closing quote of the holder's -StateDir "..." argument would
# escape that quote and scramble every argument after it.
$StateDir = $StateDir.TrimEnd('\')
if ([string]::IsNullOrWhiteSpace($LockPath)) {
    $LockPath = if ($IsWindows) { 'C:\ProgramData\UnrealMCP-ci\ue.lock' } else { '/var/tmp/UnrealMCP-ci/ue.lock' }
}
$LockPath = $LockPath.TrimEnd('\')
# Windows: share Read only, so another holder asking for ReadWrite gets a sharing violation while
# Read-LockOwner can still read who holds the lock. Elsewhere: share None, the only mode .NET backs
# with an exclusive flock(). The error codes that mean "held": Windows 32 = ERROR_SHARING_VIOLATION,
# 33 = ERROR_LOCK_VIOLATION; POSIX EWOULDBLOCK from flock(), 35 on macOS, 11 on Linux.
$lockShare = if ($IsWindows) { [IO.FileShare]::Read } else { [IO.FileShare]::None }
$heldCodes = if ($IsWindows) { @(32, 33) } elseif ($IsMacOS) { @(35) } else { @(11) }
# The POSIX lock file cannot be read while it is held (a reader's shared flock() is refused), so
# the owner is also written beside it.
$ownerSidecar = "$LockPath.owner"
$pidFile = Join-Path $StateDir 'holder.pid'
$ownerFile = Join-Path $StateDir 'owner'
$logFile = Join-Path $StateDir 'holder.log'
$acquiredFile = Join-Path $StateDir 'acquired'
$failedFile = Join-Path $StateDir 'failed'
$releaseFile = Join-Path $StateDir 'release'
$utf8 = [Text.UTF8Encoding]::new($false)

function Get-UtcStamp { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ') }

function Write-FileAtomic([string] $Path, [string] $Text) {
    $tmp = "$Path.$PID.tmp"
    [IO.File]::WriteAllText($tmp, $Text, $utf8)
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

# Reads a file another process may be writing, without ever blocking that writer.
function Read-SharedText([string] $Path) {
    $fs = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
    try { return [IO.StreamReader]::new($fs, $utf8).ReadToEnd() } finally { $fs.Dispose() }
}

function Get-InnermostException([Exception] $Exception) {
    $ex = $Exception
    while ($null -ne $ex.InnerException -and $ex -is [System.Management.Automation.RuntimeException]) {
        $ex = $ex.InnerException
    }
    return $ex
}

# Resolved once and kept: an open process handle stops Windows from reusing the pid, so an
# unrelated later process can never pass for the job's worker and pin the lock.
$script:WatchProcess = $null
function Test-WatchAlive {
    if ($WatchPid -le 0) { return $true }
    if ($null -eq $script:WatchProcess) {
        try {
            $proc = [Diagnostics.Process]::GetProcessById($WatchPid)
            $null = $proc.Handle
            $script:WatchProcess = $proc
        }
        catch [ArgumentException] { return $false }
        catch { return $null -ne (Get-Process -Id $WatchPid -ErrorAction SilentlyContinue) }
    }
    return -not $script:WatchProcess.HasExited
}

function Read-LockOwner {
    try {
        $text = (Read-SharedText $(if ($IsWindows) { $LockPath } else { $ownerSidecar })).Trim()
        if ($text) { return $text }
        return '(owner not recorded yet)'
    }
    catch { return '(owner unreadable)' }
}

function Write-HolderLog([string] $Message) {
    [IO.File]::AppendAllText($logFile, ('{0} {1}{2}' -f (Get-UtcStamp), $Message, [Environment]::NewLine), $utf8)
}

# Prints the holder log lines this step has not printed yet; returns the new count.
function Show-NewHolderLog([int] $Printed) {
    if (-not (Test-Path -LiteralPath $logFile)) { return $Printed }
    try { $lines = @((Read-SharedText $logFile) -split "`r?`n" | Where-Object { $_ -ne '' }) }
    catch { return $Printed }
    for ($i = $Printed; $i -lt $lines.Count; $i++) { Write-Host "ue-machine-lock: $($lines[$i])" }
    return [Math]::Max($Printed, $lines.Count)
}

function Initialize-LockFolder {
    $dir = Split-Path -Parent $LockPath
    if (Test-Path -LiteralPath $dir) { return }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (-not $IsWindows) {
        # Sticky and world-writable like /tmp, so a runner under any account can create the lock file
        # (each holder then opens it to everyone, see Invoke-Hold). Best effort, as icacls below.
        & chmod 1777 $dir
        if ($LASTEXITCODE -ne 0) { Write-HolderLog "chmod could not open $dir to every account (exit $LASTEXITCODE); runners under other accounts may be denied the lock" }
        return
    }
    # The first runner to create the folder owns it. Grant Modify to every account a runner on this
    # machine can run as (Authenticated Users, LOCAL SERVICE, NETWORK SERVICE; SYSTEM gets Full),
    # otherwise a lock file created under one account would be read-only to the next one, and that
    # account would fail to open it. Best effort: a concurrent creator may have done this already.
    & icacls.exe $dir /grant '*S-1-5-11:(OI)(CI)M' '*S-1-5-18:(OI)(CI)F' '*S-1-5-19:(OI)(CI)M' '*S-1-5-20:(OI)(CI)M' | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-HolderLog "icacls could not grant access on $dir (exit $LASTEXITCODE); runners under other accounts may be denied the lock" }
}

function Invoke-Hold {
    $owner = if (Test-Path -LiteralPath $ownerFile) { (Get-Content -LiteralPath $ownerFile -Raw).Trim() } else { "pid $PID" }
    Initialize-LockFolder

    $started = [DateTime]::UtcNow
    $deadline = $started.AddMinutes($WaitMinutes)
    $nextReport = $started
    $stream = $null
    while ($null -eq $stream) {
        if (Test-Path -LiteralPath $releaseFile) { Write-HolderLog 'release requested before the lock was acquired; giving up'; return 5 }
        if (-not (Test-WatchAlive)) { Write-HolderLog "watched process $WatchPid exited before the lock was acquired; giving up"; return 4 }
        try {
            $stream = [IO.FileStream]::new($LockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, $lockShare)
        }
        catch {
            $inner = Get-InnermostException $_.Exception
            $code = $inner.HResult -band 0xFFFF
            # Anything but "held" (access denied, bad path) will never clear by waiting: fail at once.
            if (-not ($inner -is [IO.IOException]) -or $code -notin $heldCodes) {
                $msg = "cannot open $LockPath ($($inner.GetType().FullName)): $($inner.Message)"
                Write-HolderLog $msg
                Write-FileAtomic $failedFile $msg
                return 2
            }
            $now = [DateTime]::UtcNow
            if ($now -ge $deadline) {
                $msg = "timed out after $WaitMinutes min waiting for the machine-wide UE lock $LockPath; held by: $(Read-LockOwner)"
                Write-HolderLog $msg
                Write-FileAtomic $failedFile $msg
                return 3
            }
            if ($now -ge $nextReport) {
                Write-HolderLog ('waiting for the machine-wide UE lock {0} (waited {1:N1} of {2} min); held by: {3}' -f $LockPath, ($now - $started).TotalMinutes, $WaitMinutes, (Read-LockOwner))
                $nextReport = $now.AddSeconds(60)
            }
            Start-Sleep -Seconds 5
        }
    }

    $heldSince = [DateTime]::UtcNow
    $why = 'holder stopped'
    try {
        $bytes = $utf8.GetBytes(('{0} (holder pid {1}, acquired {2})' -f $owner, $PID, (Get-UtcStamp)))
        $stream.SetLength(0)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
        if (-not $IsWindows) {
            # Let the next holder open the file whatever its account (the folder is 1777).
            & chmod 0666 $LockPath 2>$null
            # Best effort: in the sticky folder another account's stale sidecar cannot be replaced.
            try { Write-FileAtomic $ownerSidecar $utf8.GetString($bytes) }
            catch {
                Remove-Item -LiteralPath "$ownerSidecar.$PID.tmp" -Force -ErrorAction SilentlyContinue
                Write-HolderLog "could not record the owner in $ownerSidecar ($($_.Exception.Message))"
            }
        }
        $msg = 'acquired the machine-wide UE lock {0} after waiting {1:N1} min' -f $LockPath, ($heldSince - $started).TotalMinutes
        Write-HolderLog $msg
        Write-FileAtomic $acquiredFile $msg
        while ($true) {
            if (Test-Path -LiteralPath $releaseFile) { $why = 'release requested'; break }
            if (-not (Test-WatchAlive)) { $why = "watched process $WatchPid exited"; break }
            if (([DateTime]::UtcNow - $heldSince).TotalMinutes -ge $MaxHoldMinutes) { $why = "max hold of $MaxHoldMinutes min reached"; break }
            Start-Sleep -Seconds 2
        }
    }
    finally {
        try { $stream.SetLength(0) } catch { }
        if (-not $IsWindows) { try { Write-FileAtomic $ownerSidecar '' } catch { } }
        $stream.Dispose()
    }
    Write-HolderLog ('released the machine-wide UE lock after holding it {0:N1} min ({1})' -f ([DateTime]::UtcNow - $heldSince).TotalMinutes, $why)
    return 0
}

# Walks up from this step's process to the job's Runner.Worker, whose exit ends the job.
function Find-RunnerWorker {
    try {
        $id = $PID
        for ($i = 0; $i -lt 10 -and $id -gt 1; $i++) {
            if ($IsWindows) {
                $proc = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $id" -ErrorAction Stop
                if ($null -eq $proc) { break }
                if ($proc.Name -ieq 'Runner.Worker.exe') { return [int]$proc.ProcessId }
                $id = [int]$proc.ParentProcessId
                continue
            }
            # `ps` prints the parent pid, then the executable path (which may contain spaces).
            $line = (& ps -o ppid= -o comm= -p $id 2>$null | Select-Object -First 1)
            if (-not $line -or $line.Trim() -notmatch '^(\d+)\s+(.+)$') { break }
            if ((Split-Path -Leaf $Matches[2]) -ceq 'Runner.Worker') { return $id }
            $id = [int]$Matches[1]
        }
    }
    catch { }
    return 0
}

function Invoke-Acquire {
    if (Test-Path -LiteralPath $StateDir) { Remove-Item -LiteralPath $StateDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null

    $watch = if ($WatchPid -gt 0) { $WatchPid } else { Find-RunnerWorker }
    if ($watch -le 0) {
        Write-Host "::warning::ue-machine-lock: the job's Runner.Worker process was not found, so a job killed without its Release step holds the lock for up to $MaxHoldMinutes min"
    }
    $owner = if ($env:GITHUB_RUN_ID) {
        '{0} run {1} attempt {2} job {3} ({4}) on runner {5}' -f $env:GITHUB_REPOSITORY, $env:GITHUB_RUN_ID, $env:GITHUB_RUN_ATTEMPT, $env:GITHUB_JOB, $env:UE_ROOT, $env:RUNNER_NAME
    }
    else { "local pid $PID" }
    Write-FileAtomic $ownerFile $owner

    $psi = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
    if ($IsWindows) {
        $psi.UseShellExecute = $true
        $psi.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    }
    else {
        # ShellExecute means "open with the desktop" here. Give the holder pipes of its own instead
        # of the step's stdio (see the header); it writes only to its log file.
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
    }
    # Not the step's working directory: the holder must not keep the workspace folder open.
    $psi.WorkingDirectory = if ($IsWindows) { [Environment]::SystemDirectory } else { '/' }
    $psi.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Action Hold -LockPath "{1}" -StateDir "{2}" -WaitMinutes {3} -MaxHoldMinutes {4} -WatchPid {5}' -f $PSCommandPath, $LockPath, $StateDir, $WaitMinutes, $MaxHoldMinutes, $watch
    $holder = [Diagnostics.Process]::Start($psi)
    # The start time lets Release tell this holder apart from a later process that reused its pid.
    Write-FileAtomic $pidFile ('{0} {1}' -f $holder.Id, $holder.StartTime.ToFileTimeUtc())
    Write-Host "ue-machine-lock: holder pid $($holder.Id) is acquiring $LockPath for: $owner (released when pid $watch exits, or after $MaxHoldMinutes min)"

    $printed = 0
    $deadline = [DateTime]::UtcNow.AddMinutes($WaitMinutes + 5)
    while ($true) {
        # Sample HasExited first: the holder writes acquired/failed before it exits, so the
        # files read below are final whenever $exited is true.
        $exited = $holder.HasExited
        $printed = Show-NewHolderLog $printed
        if (Test-Path -LiteralPath $acquiredFile) { return }
        if (Test-Path -LiteralPath $failedFile) { throw "ue-machine-lock: $((Get-Content -LiteralPath $failedFile -Raw).Trim())" }
        if ($exited) { throw "ue-machine-lock: the holder exited with code $($holder.ExitCode) before acquiring the lock" }
        if ([DateTime]::UtcNow -ge $deadline) {
            Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue
            throw "ue-machine-lock: the holder neither acquired nor gave up within $($WaitMinutes + 5) min"
        }
        Start-Sleep -Seconds 3
    }
}

function Invoke-Release {
    if (-not (Test-Path -LiteralPath $pidFile)) {
        Write-Host 'ue-machine-lock: this job holds no lock (Acquire never ran); nothing to release'
        return
    }
    $fields = (Get-Content -LiteralPath $pidFile -Raw).Trim() -split '\s+'
    $holderId = [int]$fields[0]
    $holderStart = if ($fields.Count -gt 1) { [long]$fields[1] } else { -1 }
    Write-FileAtomic $releaseFile 'release'
    # Only this job's own holder is waited on or killed: a live process with the same pid but a
    # different start time is an unrelated process that reused the pid of an exited holder.
    $proc = Get-Process -Id $holderId -ErrorAction SilentlyContinue
    $isHolder = $false
    if ($proc) {
        try { $isHolder = $proc.StartTime.ToFileTimeUtc() -eq $holderStart } catch { $isHolder = $false }
    }
    if ($isHolder) {
        if (-not $proc.WaitForExit(60000)) {
            Write-Host "ue-machine-lock: holder pid $holderId did not exit within 60 s; killing it (the OS releases its handle)"
            Stop-Process -Id $holderId -Force -ErrorAction SilentlyContinue
        }
    }
    elseif ((Test-Path -LiteralPath $acquiredFile) -and -not (Test-Path -LiteralPath $failedFile)) {
        Write-Host "::warning::ue-machine-lock: holder pid $holderId had already exited before Release, so part of this job may have run without the lock; see the holder log below"
    }
    Show-NewHolderLog 0 | Out-Null
}

switch ($Action) {
    'Hold' {
        try { $code = Invoke-Hold }
        catch {
            $msg = "holder crashed: $($_.Exception.Message)"
            try { Write-HolderLog $msg; Write-FileAtomic $failedFile $msg } catch { }
            $code = 1
        }
        exit ([int]($code | Select-Object -Last 1))
    }
    'Acquire' {
        Invoke-Acquire
        exit 0
    }
    'Release' {
        try { Invoke-Release }
        catch { Write-Host "ue-machine-lock: release warning (never fails the job): $($_.Exception.Message)" }
        exit 0
    }
}
