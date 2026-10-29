#!/usr/bin/env python3
"""Machine-wide mutex for the self-hosted Unreal Engine CI jobs and local suite runs.

runner-manager registers one Unreal-MCP runner per Unreal machine, beside other repos' runners and
local runs. They share one UE install per engine (AutomationTool refuses a second instance: "A
conflicting instance of AutomationTool is already running") and the fixed smoke port 18080. A
workflow `concurrency:` group cannot serialize them: the run-level group is keyed per PR, ref or
chain lock, and a job-level group keeps only ONE pending job and cancels the older one.
See docs/RELEASING.md "Machine-wide UE lock".

The lock is an exclusive OS lock on one fixed machine path: a share-none CreateFile handle on
Windows, an exclusive flock() elsewhere. scripts/extension_suite.py takes the same lock in-process
(machine_lock()), so CI and local runs exclude each other on every OS. The holder's identity is
written beside it, to `<lock>.owner`, because the locked file itself cannot be read while held.

A CI job must keep the lock across several steps, and every step is its own process, so a small
detached HOLDER process owns it:

  acquire  starts the holder, streams its log into the step, and returns once the holder has the
           lock. It fails when the bounded wait expires or the lock cannot be opened.
  release  (an `if: always()` step) signals the holder, waits for it to exit, and never fails.
  hold     is the holder itself. It is internal; the workflow never calls it.

A job that dies without its release step cannot wedge the machine: the OS drops the lock when the
holder exits, and the holder exits when the job's Runner.Worker process is gone, when the runner's
orphan-process cleanup kills it at job end, or after --max-hold-minutes.

The holder inherits none of the step's handles (a child holding the step's stdout pipe would keep
the runner waiting on the acquire step forever): its standard streams are the null device, and
every other descriptor is closed.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Iterator, Optional

IS_WINDOWS = os.name == "nt"
# Windows ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION: another handle holds the lock.
_WIN_HELD = (32, 33)


def default_lock_path() -> Path:
    """One fixed path for every runner on the machine, whatever its account, workspace or engine."""
    if IS_WINDOWS:
        return Path(os.environ.get("ProgramData", "C:/ProgramData")) / "UnrealMCP-ci" / "ue.lock"
    return Path("/var/tmp/UnrealMCP-ci/ue.lock")


def _stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _write_atomic(path: Path, text: str) -> None:
    tmp = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def prepare_lock_folder(lock_path: Path, log=print) -> None:
    """Create the lock folder so a runner under ANY account can create and open the lock file."""
    folder = lock_path.parent
    if folder.is_dir():
        return
    folder.mkdir(parents=True, exist_ok=True)
    if not IS_WINDOWS:
        # Sticky and world-writable like /tmp; each holder then opens the file to everyone.
        try:
            folder.chmod(0o1777)
        except OSError as error:
            log(f"could not open {folder} to every account ({error}); runners under other accounts may be denied the lock")
        return
    # The first runner to create the folder owns it. Grant Modify to every account a runner can run as
    # (Authenticated Users, LOCAL SERVICE, NETWORK SERVICE; SYSTEM gets Full), so a lock file created
    # under one account stays writable for the next. Best effort: a concurrent creator may have done it.
    result = subprocess.run(["icacls.exe", str(folder), "/grant", "*S-1-5-11:(OI)(CI)M", "*S-1-5-18:(OI)(CI)F",
                             "*S-1-5-19:(OI)(CI)M", "*S-1-5-20:(OI)(CI)M"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if result.returncode != 0:
        log(f"icacls could not grant access on {folder} (exit {result.returncode}); runners under other accounts may be denied the lock")


if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    _kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    _kernel.CreateFileW.restype = wintypes.HANDLE
    _kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel.OpenProcess.restype = wintypes.HANDLE
    _kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _kernel.WaitForSingleObject.restype = wintypes.DWORD
    _kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    _kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _INVALID = wintypes.HANDLE(-1).value
    _SYNCHRONIZE, _QUERY_LIMITED, _TERMINATE = 0x00100000, 0x1000, 0x0001

    class _ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]

    _kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry)]
    _kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry)]


def try_lock(lock_path: Path):
    """Return an open handle that holds the lock, or None while another process holds it.

    Raises OSError for anything waiting will not fix (access denied, a bad path).
    """
    if IS_WINDOWS:
        # GENERIC_READ | GENERIC_WRITE, share none, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL.
        handle = _kernel.CreateFileW(str(lock_path), 0xC0000000, 0, None, 4, 0x80, None)
        if handle != _INVALID:
            return handle
        error = ctypes.get_last_error()
        if error in _WIN_HELD:
            return None
        raise OSError(error, f"cannot open {lock_path}: {ctypes.FormatError(error)}")
    import fcntl
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o666)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        return None
    except OSError:
        os.close(fd)
        raise
    try:
        os.chmod(lock_path, 0o666)  # the next holder may run under another account (the folder is 1777)
    except OSError:
        pass
    return fd


def unlock(handle) -> None:
    if IS_WINDOWS:
        _kernel.CloseHandle(handle)
    else:
        os.close(handle)  # closing the descriptor releases the flock()


def owner_path(lock_path: Path) -> Path:
    return lock_path.with_name(lock_path.name + ".owner")


def read_owner(lock_path: Path) -> str:
    return _read(owner_path(lock_path)) or "(owner not recorded)"


def record_owner(lock_path: Path, text: str) -> None:
    """Best effort: in the sticky POSIX folder another account's stale owner file cannot be replaced."""
    target = owner_path(lock_path)
    try:
        _write_atomic(target, text)
    except OSError:
        try:
            target.with_name(f"{target.name}.{os.getpid()}.tmp").unlink(missing_ok=True)
        except OSError:
            pass


@contextmanager
def machine_lock(already_held: bool = False, *, path: Optional[Path] = None,
                 timeout_seconds: float = 3600) -> Iterator[None]:
    """Hold the machine-wide lock for the duration of the block (the CI holder's lock, in-process)."""
    if already_held:
        yield
        return
    lock_path = path or default_lock_path()
    prepare_lock_folder(lock_path)
    deadline = time.monotonic() + timeout_seconds
    announced = False
    while True:
        handle = try_lock(lock_path)
        if handle is not None:
            break
        if time.monotonic() > deadline:
            raise OSError(f"Cannot acquire UE machine lock {lock_path}; held by: {read_owner(lock_path)}")
        if not announced:
            print(f"Waiting for the machine-wide UE lock (held by: {read_owner(lock_path)})", flush=True)
            announced = True
        time.sleep(1)
    record_owner(lock_path, f"local pid {os.getpid()} ({_stamp()})")
    try:
        yield
    finally:
        record_owner(lock_path, "")
        unlock(handle)


# ── processes ────────────────────────────────────────────────────────────────────────────────────

class Watched:
    """A process whose exit releases the lock. On Windows an open handle also stops pid reuse."""

    def __init__(self, pid: int):
        self.pid = pid
        self._handle = None
        if IS_WINDOWS and pid > 0:
            self._handle = _kernel.OpenProcess(_SYNCHRONIZE, False, pid) or None

    def alive(self) -> bool:
        if self.pid <= 0:
            return True
        if IS_WINDOWS:
            # WAIT_TIMEOUT (0x102): still running. No handle: the process was already gone.
            return self._handle is not None and _kernel.WaitForSingleObject(self._handle, 0) == 0x102
        return pid_alive(self.pid)


def pid_alive(pid: int) -> bool:
    if IS_WINDOWS:
        handle = _kernel.OpenProcess(_SYNCHRONIZE, False, pid)
        if not handle:
            return False
        try:
            return _kernel.WaitForSingleObject(handle, 0) == 0x102
        finally:
            _kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def process_identity(pid: int) -> str:
    """The process start time, so release never mistakes a process that reused the holder's pid for it."""
    if IS_WINDOWS:
        handle = _kernel.OpenProcess(_QUERY_LIMITED, False, pid)
        if not handle:
            return ""
        try:
            times = [wintypes.FILETIME() for _ in range(4)]
            if not _kernel.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
                return ""
            return str((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime)
        finally:
            _kernel.CloseHandle(handle)
    result = subprocess.run(["ps", "-o", "lstart=", "-p", str(pid)], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


def kill_pid(pid: int) -> None:
    if IS_WINDOWS:
        handle = _kernel.OpenProcess(_TERMINATE, False, pid)
        if handle:
            _kernel.TerminateProcess(handle, 1)
            _kernel.CloseHandle(handle)
        return
    try:
        os.kill(pid, 9)
    except OSError:
        pass


def _windows_processes() -> dict:
    """pid -> (parent pid, executable name), from one Toolhelp snapshot."""
    snapshot = _kernel.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snapshot == _INVALID:
        return {}
    table = {}
    try:
        entry = _ProcessEntry()
        entry.dwSize = ctypes.sizeof(_ProcessEntry)
        ok = _kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            table[entry.th32ProcessID] = (entry.th32ParentProcessID, entry.szExeFile)
            ok = _kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        _kernel.CloseHandle(snapshot)
    return table


def find_runner_worker() -> int:
    """Walk up from this process to the job's Runner.Worker, whose exit ends the job; 0 if not found."""
    try:
        pid = os.getpid()
        table = _windows_processes() if IS_WINDOWS else None
        for _ in range(12):
            if pid <= 1:
                break
            if IS_WINDOWS:
                if pid not in table:
                    break
                parent, name = table[pid]
                if name.lower() == "runner.worker.exe":
                    return pid
                pid = parent
                continue
            line = subprocess.run(["ps", "-o", "ppid=", "-o", "comm=", "-p", str(pid)],
                                  capture_output=True, text=True).stdout.strip()
            parts = line.split(None, 1)
            if len(parts) != 2 or not parts[0].isdigit():
                break
            if os.path.basename(parts[1]) == "Runner.Worker":
                return pid
            pid = int(parts[0])
    except OSError:
        pass
    return 0


# ── the CI holder ────────────────────────────────────────────────────────────────────────────────

class State:
    """Per-job handshake folder between acquire, the holder and release."""

    def __init__(self, folder: Path):
        self.folder = folder
        self.pid = folder / "holder.pid"
        self.owner = folder / "owner"
        self.log = folder / "holder.log"
        self.acquired = folder / "acquired"
        self.failed = folder / "failed"
        self.release = folder / "release"

    def write_log(self, message: str) -> None:
        with self.log.open("a", encoding="utf-8") as handle:
            handle.write(f"{_stamp()} {message}\n")

    def fail(self, message: str) -> None:
        self.write_log(message)
        _write_atomic(self.failed, message)

    def show_log(self, printed: int) -> int:
        """Print the holder log lines not printed yet; return the new count."""
        lines = [line for line in _read(self.log).splitlines() if line]
        for line in lines[printed:]:
            print(f"ue-machine-lock: {line}", flush=True)
        return max(printed, len(lines))


def hold(state: State, lock_path: Path, wait_minutes: float, max_hold_minutes: float, watch_pid: int) -> int:
    owner = _read(state.owner) or f"pid {os.getpid()}"
    prepare_lock_folder(lock_path, state.write_log)
    watched = Watched(watch_pid)
    started = time.monotonic()
    deadline = started + wait_minutes * 60
    next_report = started
    while True:
        if state.release.exists():
            state.write_log("release requested before the lock was acquired; giving up")
            return 5
        if not watched.alive():
            state.write_log(f"watched process {watch_pid} exited before the lock was acquired; giving up")
            return 4
        try:
            handle = try_lock(lock_path)
        except OSError as error:
            state.fail(f"cannot open {lock_path}: {error}")
            return 2
        if handle is not None:
            break
        now = time.monotonic()
        if now >= deadline:
            state.fail(f"timed out after {wait_minutes:g} min waiting for the machine-wide UE lock {lock_path}; "
                       f"held by: {read_owner(lock_path)}")
            return 3
        if now >= next_report:
            state.write_log(f"waiting for the machine-wide UE lock {lock_path} (waited {(now - started) / 60:.1f} of "
                            f"{wait_minutes:g} min); held by: {read_owner(lock_path)}")
            next_report = now + 60
        time.sleep(5)

    held_since = time.monotonic()
    why = "holder stopped"
    try:
        record_owner(lock_path, f"{owner} (holder pid {os.getpid()}, acquired {_stamp()})")
        message = f"acquired the machine-wide UE lock {lock_path} after waiting {(held_since - started) / 60:.1f} min"
        state.write_log(message)
        _write_atomic(state.acquired, message)
        while True:
            if state.release.exists():
                why = "release requested"
                break
            if not watched.alive():
                why = f"watched process {watch_pid} exited"
                break
            if time.monotonic() - held_since >= max_hold_minutes * 60:
                why = f"max hold of {max_hold_minutes:g} min reached"
                break
            time.sleep(2)
    finally:
        record_owner(lock_path, "")
        unlock(handle)
    state.write_log(f"released the machine-wide UE lock after holding it {(time.monotonic() - held_since) / 60:.1f} min ({why})")
    return 0


def acquire(state: State, lock_path: Path, wait_minutes: float, max_hold_minutes: float, watch_pid: int) -> int:
    if state.folder.exists():
        import shutil
        shutil.rmtree(state.folder)
    state.folder.mkdir(parents=True)
    watch = watch_pid if watch_pid > 0 else find_runner_worker()
    if watch <= 0:
        print("::warning::ue-machine-lock: the job's Runner.Worker process was not found, so a job killed without its "
              f"release step holds the lock for up to {max_hold_minutes:g} min", flush=True)
    env = os.environ
    owner = (f"{env['GITHUB_REPOSITORY']} run {env.get('GITHUB_RUN_ID')} attempt {env.get('GITHUB_RUN_ATTEMPT')} "
             f"job {env.get('GITHUB_JOB')} ({env.get('UE_ROOT', '')}) on runner {env.get('RUNNER_NAME')}"
             if env.get("GITHUB_RUN_ID") else f"local pid {os.getpid()}")
    _write_atomic(state.owner, owner)

    command = [sys.executable, str(Path(__file__).resolve()), "hold", "--lock-path", str(lock_path),
               "--state-dir", str(state.folder), "--wait-minutes", str(wait_minutes),
               "--max-hold-minutes", str(max_hold_minutes), "--watch-pid", str(watch)]
    options = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
    if IS_WINDOWS:
        # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP: no console, no Ctrl+C from the step.
        options.update(creationflags=0x00000008 | 0x00000200, cwd=os.environ.get("SystemRoot", "C:\\Windows"))
    else:
        options.update(start_new_session=True, cwd="/")  # not the workspace: the holder must not pin it
    holder = subprocess.Popen(command, **options)
    _write_atomic(state.pid, f"{holder.pid}\n{process_identity(holder.pid)}")
    print(f"ue-machine-lock: holder pid {holder.pid} is acquiring {lock_path} for: {owner} "
          f"(released when pid {watch} exits, or after {max_hold_minutes:g} min)", flush=True)

    printed = 0
    deadline = time.monotonic() + (wait_minutes + 5) * 60
    while True:
        # Sample the exit first: the holder writes acquired/failed before it exits, so the files read
        # below are final whenever it has exited.
        code = holder.poll()
        printed = state.show_log(printed)
        if state.acquired.exists():
            return 0
        if state.failed.exists():
            print(f"::error::ue-machine-lock: {_read(state.failed)}", flush=True)
            return 1
        if code is not None:
            print(f"::error::ue-machine-lock: the holder exited with code {code} before acquiring the lock", flush=True)
            return 1
        if time.monotonic() >= deadline:
            holder.kill()
            print(f"::error::ue-machine-lock: the holder neither acquired nor gave up within {wait_minutes + 5:g} min", flush=True)
            return 1
        time.sleep(3)


def release(state: State) -> int:
    if not state.pid.exists():
        print("ue-machine-lock: this job holds no lock (acquire never ran); nothing to release")
        return 0
    fields = _read(state.pid).split("\n", 1)
    holder_pid = int(fields[0])
    identity = fields[1] if len(fields) > 1 else ""
    _write_atomic(state.release, "release")
    # Only this job's own holder is waited on or killed: a live process with the same pid but another
    # start time is an unrelated process that reused the pid of an exited holder.
    is_holder = pid_alive(holder_pid) and bool(identity) and process_identity(holder_pid) == identity
    if is_holder:
        deadline = time.monotonic() + 60
        while pid_alive(holder_pid) and process_identity(holder_pid) == identity and time.monotonic() < deadline:
            time.sleep(0.5)
        if pid_alive(holder_pid) and process_identity(holder_pid) == identity:
            print(f"ue-machine-lock: holder pid {holder_pid} did not exit within 60 s; killing it (the OS releases its lock)")
            kill_pid(holder_pid)
    elif state.acquired.exists() and not state.failed.exists():
        print(f"::warning::ue-machine-lock: holder pid {holder_pid} had already exited before release, so part of this "
              "job may have run without the lock; see the holder log below")
    state.show_log(0)
    return 0


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["acquire", "release", "hold"])
    parser.add_argument("--lock-path", type=Path, default=None,
                        help="default: C:/ProgramData/UnrealMCP-ci/ue.lock on Windows, /var/tmp/UnrealMCP-ci/ue.lock elsewhere")
    parser.add_argument("--wait-minutes", type=float, default=60,
                        help="the bounded wait; the job's timeout-minutes must exceed it plus the job's own work, "
                             "because a job timeout is a GitHub-side cancel, which can wedge a self-hosted runner")
    parser.add_argument("--max-hold-minutes", type=float, default=360,
                        help="backstop only: releases a holder whose job vanished and whose worker was not resolved")
    parser.add_argument("--state-dir", type=Path, default=None, help="per-job handshake folder (default under RUNNER_TEMP)")
    parser.add_argument("--watch-pid", type=int, default=0,
                        help="the process whose exit releases the lock; 0 makes acquire resolve the job's Runner.Worker")
    args = parser.parse_args(argv)
    lock_path = args.lock_path or default_lock_path()
    state = State(args.state_dir or Path(os.environ.get("RUNNER_TEMP") or __import__("tempfile").gettempdir()) / "ue-machine-lock")
    if args.action == "hold":
        try:
            return hold(state, lock_path, args.wait_minutes, args.max_hold_minutes, args.watch_pid)
        except Exception as error:  # the step only sees the state files
            try:
                state.fail(f"holder crashed: {error}")
            except OSError:
                pass
            return 1
    if args.action == "acquire":
        return acquire(state, lock_path, args.wait_minutes, args.max_hold_minutes, args.watch_pid)
    try:
        return release(state)
    except Exception as error:
        print(f"ue-machine-lock: release warning (never fails the job): {error}")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
