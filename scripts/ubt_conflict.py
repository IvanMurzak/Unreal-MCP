"""Retry a UE build that lost UnrealBuildTool's global mutex to another build on the same machine.

The self-hosted UE runner shares the owner's dev machine. A local or agent UBT build there holds
UBT's machine-wide mutex, which the CI lock (ue_machine_lock.py) does not cover, and the CI build
then fails at once instead of compiling:

- ``A conflicting instance of Global\\UnrealBuildTool_Mutex_<hash> is already running.``
  ``Result: Failed (ConflictingInstance)`` (UAT exit 10, UE 5.6+), or
  ``A conflicting instance of UnrealBuildTool is already running.`` (UAT exit 6, UE 5.5);
- ``UnauthorizedAccessException: Access to the path 'Global\\UnrealBuildTool_Mutex_<hash>' is denied``
  (or ``Global\\AutomationTool_<hash>``), when the holder runs in another Windows session (the runner
  is session 0).

`retry` reruns such a build after a backoff and logs each wait. All builds of one process (one CI
step) share one 10-min wait budget, so a step with several builds cannot stretch a job past its
timeout. A build that also printed a compiler or linker error is never retried: that is a real
failure. Stdlib only.
"""
from __future__ import annotations

import re
import time
from typing import Callable

# Seconds to wait before each retry of one build: 5 retries, 600 s.
RETRY_DELAYS = (30, 60, 120, 180, 210)
# The most this process waits in all, over every build it retries.
WAIT_BUDGET = {"left": sum(RETRY_DELAYS)}

_CONFLICT = re.compile(r"A conflicting instance of .+ is already running"
                       r"|Result: Failed \(ConflictingInstance\)"
                       r"|(?:UnrealBuildTool_Mutex|AutomationTool)_\w+.*(?:is denied|UnauthorizedAccess)"
                       r"|UnauthorizedAccessException.*(?:UnrealBuildTool_Mutex|AutomationTool)_\w+")
# MSVC (error C2065, fatal error C1083, error LNK2019), clang/gcc (file.cpp:1:2: error: ...), UHT/UBT
# (file.h(12): Error: ...).
_COMPILE_ERROR = re.compile(r"\b(?:fatal )?error (?:C|LNK)\d{4}\b|:\d+(?::\d+)?: (?:fatal )?error:|\(\d+\): Error:")


class Detector:
    """Fed every output line of one build attempt; says whether it failed only on the UBT mutex."""

    def __init__(self) -> None:
        self.conflict = False
        self.compile_error = False

    def feed(self, line: str) -> None:
        if not self.conflict and _CONFLICT.search(line):
            self.conflict = True
        if not self.compile_error and _COMPILE_ERROR.search(line):
            self.compile_error = True

    @property
    def retryable(self) -> bool:
        return self.conflict and not self.compile_error


def retry(attempt: Callable[[Callable[[str], None]], int], what: str, *,
          delays=None, budget=None, sleep=None, log=None) -> int:
    """Run `attempt(feed)` (it passes each output line to `feed` and returns the exit code) until it
    succeeds, fails for another reason, or the delays or this process's wait `budget` run out.
    Returns the last exit code."""
    delays = RETRY_DELAYS if delays is None else delays
    budget = WAIT_BUDGET if budget is None else budget
    sleep = sleep or time.sleep
    log = log or (lambda message: print(message, flush=True))
    for number, delay in enumerate((*delays, None), start=1):
        detector = Detector()
        code = attempt(detector.feed)
        if code == 0 or not detector.retryable:
            return code
        if delay is None or delay > budget["left"]:
            log(f"::error::{what}: UnrealBuildTool is still held by another build on this machine after "
                f"{number} attempts ({budget['left']} s of this step's wait budget left); giving up (exit {code}).")
            return code
        log(f"::warning::{what}: another UnrealBuildTool on this machine holds its global mutex (exit {code}); "
            f"a local or agent build? Retrying in {delay} s (attempt {number + 1} of {len(delays) + 1}).")
        budget["left"] -= delay
        sleep(delay)
    raise AssertionError("unreachable")
