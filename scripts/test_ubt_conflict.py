"""Tests for ubt_conflict.py: which UE build failures are waited out, and the bounded retry."""
from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import extension_suite as suite  # scripts/ is on sys.path under `unittest discover -s scripts`
import ubt_conflict

# Verbatim from the failures this handles (DEV-287): Unreal-MCP PR #496 run 45903738104 (UE 5.7) and release run
# 45905080818 attempt 1 (UE 5.5), and a local build that lost the mutex to the session-0 runner.
CONFLICT_57 = [r"A conflicting instance of Global\UnrealBuildTool_Mutex_cc1e4ec248d9b3ed35259397698050712f573880 is already running.",
               "", "Result: Failed (ConflictingInstance)", "Took 0.31s to run dotnet.exe, ExitCode=10",
               "AutomationTool exiting with ExitCode=10 (Error_SDKNotFound)"]
CONFLICT_55 = ["A conflicting instance of UnrealBuildTool is already running.", "Took 0.34s to run dotnet.exe, ExitCode=6",
               "AutomationTool exiting with ExitCode=6 (6)"]
ACCESS_DENIED = [r"Unhandled exception: System.UnauthorizedAccessException: Access to the path "
                 r"'Global\UnrealBuildTool_Mutex_cc1e4ec248d9b3ed35259397698050712f573880' is denied."]
COMPILE_ERRORS = [r"C:\p\Foo.cpp(12): error C2065: 'Bar': undeclared identifier",
                  "Foo.cpp.obj : error LNK2019: unresolved external symbol",
                  "/p/Foo.cpp:12:3: error: use of undeclared identifier 'Bar'",
                  r"C:\p\Foo.h(4): Error: Missing '*' in Expected a pointer type",
                  r"C:\p\Foo.cpp(1): fatal error C1083: Cannot open include file"]


def detect(lines) -> ubt_conflict.Detector:
    detector = ubt_conflict.Detector()
    for line in lines:
        detector.feed(line)
    return detector


class DetectorTests(unittest.TestCase):
    def test_the_mutex_conflicts_are_retryable(self):
        for lines in (CONFLICT_57, CONFLICT_55, ACCESS_DENIED, ["Result: Failed (ConflictingInstance)"],
                      ["A conflicting instance of AutomationTool is already running."],
                      [r"System.UnauthorizedAccessException: Access to the path 'Global\AutomationTool_0f3a9c' is denied."]):
            self.assertTrue(detect(lines).retryable, lines)

    def test_a_compile_error_is_never_retried(self):
        for error in COMPILE_ERRORS:
            self.assertFalse(detect([error]).retryable, error)
            self.assertFalse(detect([error, *CONFLICT_57]).retryable, error)

    def test_other_failures_are_not_retried(self):
        for lines in (["BUILD FAILED", "AutomationTool exiting with ExitCode=10 (Error_SDKNotFound)"],
                      [r"System.UnauthorizedAccessException: Access to the path 'C:\x\Log.txt' is denied."],
                      ["warning C4996: 'X': deprecated", "Result: Succeeded"], []):
            self.assertFalse(detect(lines).retryable, lines)


class RetryTests(unittest.TestCase):
    def run_retry(self, outcomes, delays=(1, 2, 3), budget=None):
        """`outcomes`: per attempt, (exit code, output lines). Returns (exit code, attempts, sleeps, log)."""
        attempts, sleeps, logged = [], [], []
        budget = {"left": 100} if budget is None else budget

        def attempt(feed):
            code, lines = outcomes[len(attempts)]
            attempts.append(code)
            for line in lines:
                feed(line)
            return code

        code = ubt_conflict.retry(attempt, "Build", delays=delays, budget=budget, sleep=sleeps.append, log=logged.append)
        return code, attempts, sleeps, logged

    def test_a_conflict_is_retried_until_the_build_succeeds(self):
        code, attempts, sleeps, logged = self.run_retry([(10, CONFLICT_57), (6, CONFLICT_55), (0, [])])
        self.assertEqual((code, attempts, sleeps), (0, [10, 6, 0], [1, 2]))
        self.assertTrue(all(line.startswith("::warning::Build: another UnrealBuildTool") for line in logged), logged)

    def test_the_wait_is_bounded(self):
        code, attempts, sleeps, logged = self.run_retry([(10, CONFLICT_57)] * 4)
        self.assertEqual((code, len(attempts), sleeps), (10, 4, [1, 2, 3]))
        self.assertIn("::error::Build: UnrealBuildTool is still held", logged[-1])
        self.assertIn("after 4 attempts", logged[-1])

    def test_every_build_of_a_step_shares_one_wait_budget(self):
        budget = {"left": 4}
        self.assertEqual(self.run_retry([(10, CONFLICT_57), (0, [])], budget=budget)[:3], (0, [10, 0], [1]))
        # 3 s left: the second build waits 1 + 2 s, then gives up instead of waiting 3 s more.
        code, attempts, sleeps, logged = self.run_retry([(6, CONFLICT_55)] * 4, budget=budget)
        self.assertEqual((code, attempts, sleeps, budget), (6, [6, 6, 6], [1, 2], {"left": 0}))
        self.assertIn("0 s of this step's wait budget left", logged[-1])

    def test_a_real_failure_fails_at_once(self):
        for outcome in ((5, ["BUILD FAILED"]), (6, [COMPILE_ERRORS[0], *CONFLICT_55])):
            self.assertEqual(self.run_retry([outcome]), (outcome[0], [outcome[0]], [], []))

    def test_success_and_the_default_budget(self):
        self.assertEqual(self.run_retry([(0, CONFLICT_57)]), (0, [0], [], []))
        self.assertEqual(sum(ubt_conflict.RETRY_DELAYS), 600)


class ExtensionSuiteBuildTests(unittest.TestCase):
    """extension_suite.build: the UBT build of a suite project, its output in <project>/build.log."""

    def build(self, outputs):
        """Each fake UBT run writes its (exit code, lines) of `outputs` to the log. Returns (runs, sleeps, error)."""
        runs, sleeps = [], []

        def fake_run(command, stdout, stderr):
            code, lines = outputs[len(runs)]
            runs.append(command)
            stdout.write("\n".join(lines) + "\n")
            return subprocess.CompletedProcess(command, code)

        with tempfile.TemporaryDirectory() as folder, mock.patch.object(suite.subprocess, "run", fake_run), \
                mock.patch.object(ubt_conflict.time, "sleep", sleeps.append), mock.patch("builtins.print"), \
                mock.patch.dict(ubt_conflict.WAIT_BUDGET, {"left": 600}):
            project = Path(folder) / "Suite/Suite.uproject"
            project.parent.mkdir()
            try:
                suite.build(Path(folder) / "UE", project)
                error = None
            except subprocess.CalledProcessError as raised:
                error = raised.returncode
        return len(runs), sleeps, error

    def test_a_cross_session_mutex_denial_is_waited_out(self):
        self.assertEqual(self.build([(6, ACCESS_DENIED), (0, ["Result: Succeeded"])]), (2, [ubt_conflict.RETRY_DELAYS[0]], None))

    def test_a_compile_error_fails_the_build_at_once(self):
        self.assertEqual(self.build([(6, [COMPILE_ERRORS[0]])]), (1, [], 6))


if __name__ == "__main__":
    sys.exit(unittest.main())
