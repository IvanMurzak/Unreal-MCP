#!/usr/bin/env python3
"""
Safe GitHub release operator wrapper for Unreal-MCP.

Wraps the EXISTING release contract instead of bypassing it:
  prepare   -> runs commands/bump_version.py locally (needs a clean working tree)
  dry-run   -> dispatches release.yml with dry_run=true
  publish   -> dispatches release.yml with dry_run=false
  watch     -> watches the latest release.yml run

This script never creates tags or GitHub Releases directly and never publishes to npm directly.
release.yml remains the only publisher. Requires `git` and `gh` on PATH.

Usage:
    python commands/release_github.py --mode prepare --new-version 0.6.5
    python commands/release_github.py --mode dry-run --wait
    python commands/release_github.py --mode publish --wait
    python commands/release_github.py --mode watch
Exit 0 = done; exit 1 = a precondition or command failed (message on stderr).
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent
MODES = ("prepare", "dry-run", "publish", "watch")


class ReleaseError(Exception):
    pass


def section(text: str) -> None:
    print(f"\n== {text} ==", flush=True)


def require_command(name: str) -> None:
    if shutil.which(name) is None:
        raise ReleaseError(f"Required command not found on PATH: {name}")


def require_clean_working_tree() -> None:
    result = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain"],
                            stdout=subprocess.PIPE, text=True)
    if result.returncode != 0:
        raise ReleaseError("git status failed.")
    if result.stdout.strip():
        raise ReleaseError("Working tree is not clean. Commit or stash changes before running "
                           "release_github.py --mode prepare.")


def require_gh_auth() -> None:
    if subprocess.run(["gh", "auth", "status"], stdout=subprocess.DEVNULL).returncode != 0:
        raise ReleaseError("gh auth status failed. Authenticate gh before dispatching release workflows.")


def latest_release_run_id(repo: str) -> str:
    result = subprocess.run(["gh", "run", "list", "--repo", repo, "--workflow", "release.yml", "--limit", "1",
                             "--json", "databaseId", "--jq", ".[0].databaseId"],
                            stdout=subprocess.PIPE, text=True)
    if result.returncode != 0:
        raise ReleaseError("Failed to query the latest release workflow run.")
    return result.stdout.strip()


def watch_latest_release_run(repo: str) -> None:
    section("Watching latest release.yml run")
    time.sleep(3)
    run_id = latest_release_run_id(repo)
    if not run_id:
        raise ReleaseError("No release.yml run found to watch.")
    if subprocess.run(["gh", "run", "watch", run_id, "--repo", repo]).returncode != 0:
        raise ReleaseError(f"gh run watch failed for run {run_id}.")


def dispatch_release(repo: str, ref: str, dry_run: bool) -> int:
    return subprocess.run(["gh", "workflow", "run", "release.yml", "--repo", repo, "--ref", ref,
                           "-f", f"dry_run={'true' if dry_run else 'false'}"]).returncode


def run(mode: str, new_version: Optional[str], repo: str, ref: str, wait: bool) -> None:
    require_command("git")
    require_command("gh")

    if mode == "prepare":
        if not new_version or not new_version.strip():
            raise ReleaseError("--new-version is required for --mode prepare.")
        section("Preparing release version bump")
        require_clean_working_tree()
        bump = [sys.executable, str(SCRIPT_DIR / "bump_version.py"), "--new-version", new_version]
        if subprocess.run(bump, cwd=ROOT).returncode != 0:
            raise ReleaseError("bump_version.py failed.")
        print("Version bump prepared locally. Commit it on a release branch, open a PR, merge to main, "
              "then run --mode publish (release.yml is dispatch-only).")
    elif mode == "dry-run":
        section("Dispatching release.yml dry-run")
        require_gh_auth()
        if dispatch_release(repo, ref, dry_run=True) != 0:
            raise ReleaseError("Failed to dispatch release.yml dry-run.")
        print(f"Dry-run dispatched on {ref}.")
        if wait:
            watch_latest_release_run(repo)
    elif mode == "publish":
        section("Dispatching release.yml publish")
        require_gh_auth()
        if dispatch_release(repo, ref, dry_run=False) != 0:
            raise ReleaseError("Failed to dispatch release.yml publish.")
        print(f"Publish dispatch sent on {ref}. release.yml fails in check-version if the version on main "
              "is already tagged or the UE runner is not ready.")
        if wait:
            watch_latest_release_run(repo)
    else:  # watch
        require_gh_auth()
        watch_latest_release_run(repo)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--mode", required=True, choices=MODES, type=str.lower, help="prepare | dry-run | publish | watch")
    parser.add_argument("--new-version", help="semver to bump to (required for --mode prepare)")
    parser.add_argument("--repo", default="IvanMurzak/Unreal-MCP", help="GitHub repo (default: %(default)s)")
    parser.add_argument("--ref", default="main", help="git ref to dispatch release.yml on (default: %(default)s)")
    parser.add_argument("--wait", action="store_true", help="watch the dispatched run until it finishes")
    args = parser.parse_args(argv)
    try:
        run(args.mode, args.new_version, args.repo, args.ref, args.wait)
    except ReleaseError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
