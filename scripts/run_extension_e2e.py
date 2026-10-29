"""Run each extension's live E2E checks (extensions/<plugin>/Tests/e2e/tools/*.e2e.json).

Every check is declarative JSON, invoked through this checkout's compiled CLI:
`node cli/dist/index.js run-tool|run-system-tool <tool> --path <project> --url <url> [--input <json>]`.

    {
      "description": "the count of PCG graphs",          # what the assert expects (printed on failure)
      "tool": "rql-query",                                # kebab-case tool id
      "system": false,                                    # optional: run-system-tool instead of run-tool
      "input": {"query": "pcgGraphs | count"},           # optional: tool arguments (a JSON object)
      "expectError": false,                               # optional: the tool must fail (then assert is required)
      "assert": {"pattern": "..."}                        # optional: regex that must match the result
    }

`assert.pattern` is searched case-insensitively in the compact JSON serialization of the result
(`json.dumps(result, separators=(",", ":"), ensure_ascii=False)`); a CLI output that is not JSON is
serialized as a JSON string. Rules: a plugin without checks is an error; a non-zero CLI exit fails
unless expectError; a transport failure always fails; success needs a JSON object whose isError is
not true. Python stdlib only; extension_suite.py calls run() in-process. See docs/EXTENSION-MONOREPO.md.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "cli/dist/index.js"
TOOL_ID = re.compile(r"[a-z0-9]+(-[a-z0-9]+)*\Z")
TRANSPORT_FAILURE = re.compile(r"\(network\)|\(timeout\)|ECONNREFUSED|ENOTFOUND", re.IGNORECASE)
KEYS = {"description", "tool", "system", "input", "expectError", "assert"}


def load_check(path: Path) -> dict:
    """Read and validate one *.e2e.json check; raises ValueError naming the file on any schema error."""
    check = json.loads(path.read_text(encoding="utf-8"))

    def require(ok: bool, message: str) -> None:
        if not ok:
            raise ValueError(f"{path.name}: {message}")

    require(isinstance(check, dict), "a check must be a JSON object")
    require(not set(check) - KEYS, f"unknown keys {sorted(set(check) - KEYS)}")
    require(isinstance(check.get("description"), str) and check["description"].strip() != "", "description must be a non-empty string")
    require(isinstance(check.get("tool"), str) and TOOL_ID.match(check["tool"]) is not None, "tool must be a kebab-case tool id")
    require(isinstance(check.get("system", False), bool), "system must be a boolean")
    require(isinstance(check.get("expectError", False), bool), "expectError must be a boolean")
    require(isinstance(check.get("input", {}), dict), "input must be a JSON object")
    assertion = check.get("assert")
    if assertion is not None:
        require(isinstance(assertion, dict) and set(assertion) == {"pattern"} and isinstance(assertion["pattern"], str),
                'assert must be {"pattern": "<regex>"}')
        try:
            re.compile(assertion["pattern"])
        except re.error as error:
            raise ValueError(f"{path.name}: invalid assert pattern: {error}") from None
    require(not check.get("expectError") or assertion is not None, "expected-error checks must assert the specific tool error")
    return check


def checks_for(plugin: str) -> list[Path]:
    return sorted((ROOT / "extensions" / plugin / "Tests/e2e/tools").glob("*.e2e.json"))


def serialize(result: object) -> str:
    return json.dumps(result, separators=(",", ":"), ensure_ascii=False)


def evaluate(check: dict, code: int, text: str) -> None:
    """Apply the runner rules to one CLI outcome (exit code + combined stdout/stderr); raises ValueError on failure."""
    expect_error = check.get("expectError", False)
    if code != 0 and not expect_error:
        raise ValueError(f"CLI failed: {text}")
    # A transport failure must not count as a defensive tool error.
    if TRANSPORT_FAILURE.search(text):
        raise ValueError(f"Transport failure: {text}")
    try:
        result = json.loads(text)
    except ValueError:
        result = text
    if not expect_error and (not isinstance(result, dict) or result.get("isError") is True):
        raise ValueError(f"Invalid success: {text}")
    assertion = check.get("assert")
    if assertion is not None:
        serialized = serialize(result)
        if not re.search(assertion["pattern"], serialized, re.IGNORECASE):
            raise ValueError(f"expected {check['description']}; got: {serialized}")


def run(project_dir: Path | str, url: str, plugins: list[str], env: dict | None = None,
        timeout: float | None = None) -> list[str]:
    """Run every check of every plugin; prints PASS/FAIL lines and returns the failures (empty = all passed)."""
    deadline = None if timeout is None else time.monotonic() + timeout
    failures: list[str] = []
    for plugin in plugins:
        files = checks_for(plugin)
        if not files:
            failures.append(f"{plugin}: Missing E2E checks")
            print(f"FAIL {failures[-1]}", flush=True)
            continue
        for path in files:
            name = f"{plugin}/{path.name[:-len('.e2e.json')]}"
            try:
                check = load_check(path)
                name = f"{plugin}/{check['tool']}"
                args = ["node", str(CLI), "run-system-tool" if check.get("system") else "run-tool", check["tool"],
                        "--path", str(project_dir), "--url", url]
                if "input" in check:
                    args += ["--input", serialize(check["input"])]
                remaining = None if deadline is None else max(1.0, deadline - time.monotonic())
                completed = subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           encoding="utf-8", errors="replace", timeout=remaining)
                evaluate(check, completed.returncode, completed.stdout.strip())
                print(f"PASS {name}", flush=True)
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                failures.append(f"{name}: {error}")
                print(f"FAIL {failures[-1]}", flush=True)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project-dir", type=Path, required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--plugins", required=True, help="Comma-separated plugin names")
    args = parser.parse_args()
    failures = run(args.project_dir, args.url, [p for p in args.plugins.split(",") if p], dict(os.environ))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
