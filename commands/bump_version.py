#!/usr/bin/env python3
"""
Automated version bump for the Unreal-MCP mono-repo.

Updates the single shared semver across all version-bearing files (design docs/ARCHITECTURE.md §9.2):
the UE plugin .uplugin `VersionName`, the bridge csproj `<Version>`, and the cli npm package
(`package.json` + `package-lock.json` + the `cli/src/version.ts` `PACKAGE_VERSION` constant).
Prevents human errors from hand-editing one of them alone. `--what-if` previews without writing.

NOTE: the consumed GameDev-MCP-Server version (`cli/src/lib/server-version.ts` `SERVER_VERSION`) is
deliberately NOT touched: the shared server is released from its own repo and its version is
independent of the plugin version.

Edits are regex substitutions on the raw text, so a bump changes only the version strings: line
endings are preserved and files are written back as UTF-8 without a BOM.

Usage:
    python commands/bump_version.py --new-version 0.2.0
    python commands/bump_version.py --new-version 0.2.0 --what-if
Exit 0 = bumped (or previewed, or already at that version); exit 1 = invalid version, nothing to
update, or an error.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

ROOT = Path(__file__).resolve().parents[1]
UPLUGIN = "UnrealMCP/UnrealMCP.uplugin"
LOCK_FILE = "cli/package-lock.json"
SEMVER_SUFFIX = r"(-[a-zA-Z0-9\-\.]+)?(\+[a-zA-Z0-9\-\.]+)?"
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+" + SEMVER_SUFFIX + r"$")
CURRENT_VERSION_RE = re.compile(r'"VersionName":\s*"([\d\.]+)"', re.IGNORECASE)
# Only the two root-level version fields that follow "name": "unreal-mcp-cli"; dependency versions
# are not touched.
LOCK_RE = re.compile(r'("name":\s*"unreal-mcp-cli",\s+"version":\s*")[\d\.]+' + SEMVER_SUFFIX)


@dataclass(frozen=True)
class VersionFile:
    path: str
    pattern: re.Pattern
    replace: str  # `{VERSION}` is substituted with the new version
    description: str


# The .uplugin VersionName is the version single-source (current_version reads it).
VERSION_FILES = (
    VersionFile(UPLUGIN, re.compile(r'"VersionName":\s*"[\d\.]+"', re.IGNORECASE),
                '"VersionName": "{VERSION}"', "UE plugin .uplugin VersionName"),
    VersionFile("bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj",
                re.compile(r"<Version>[\d\.]+</Version>", re.IGNORECASE),
                "<Version>{VERSION}</Version>", "Bridge csproj XML version"),
    VersionFile("cli/package.json", re.compile(r'"version":\s*"[\d\.]+' + SEMVER_SUFFIX + '"', re.IGNORECASE),
                '"version": "{VERSION}"', "CLI npm package version"),
    VersionFile("cli/src/version.ts",
                re.compile(r"PACKAGE_VERSION: string = '[\d\.]+" + SEMVER_SUFFIX + "'", re.IGNORECASE),
                "PACKAGE_VERSION: string = '{VERSION}'", "CLI runtime PACKAGE_VERSION constant"),
)


@dataclass
class Change:
    path: Path
    description: str
    matches: int
    content: str


def read_text(path: Path) -> str:
    # newline="" keeps CRLF/LF exactly as on disk; utf-8-sig drops a BOM like Get-Content did.
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        return f.read()


def write_text(path: Path, content: str) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write(content)


def is_semantic_version(version: str) -> bool:
    return bool(version and version.strip()) and SEMVER_RE.match(version) is not None


def current_version(root: Path) -> str:
    path = root / UPLUGIN
    if not path.is_file():
        raise RuntimeError(f"Could not find .uplugin at: {UPLUGIN}")
    match = CURRENT_VERSION_RE.search(read_text(path))
    if not match:
        raise RuntimeError(f"Could not extract current VersionName from {UPLUGIN}")
    return match.group(1)


def plan_changes(root: Path, new_version: str) -> List[Change]:
    changes: List[Change] = []
    for vf in VERSION_FILES:
        path = root / vf.path
        if not path.is_file():
            print(f"[warn] File not found: {vf.path}")
            continue
        content = read_text(path)
        replacement = vf.replace.replace("{VERSION}", new_version)
        new_content, count = vf.pattern.subn(lambda _m: replacement, content)
        if new_content == content:
            print(f"[warn] No matches found in: {vf.path}")
            print(f"   Pattern: {vf.pattern.pattern}")
            continue
        changes.append(Change(path, vf.description, count, new_content))
        print(f"[edit] {vf.description}: {count} occurrence(s)")
        for match in vf.pattern.finditer(content):
            print(f"   {match.group(0)} -> {replacement}")
    return changes


def bump_lock_file(root: Path, new_version: str) -> None:
    path = root / LOCK_FILE
    if not path.is_file():
        return
    print("\nUpdating CLI package-lock.json...")
    content = read_text(path)
    new_content, count = LOCK_RE.subn(lambda m: m.group(1) + new_version, content)
    if new_content != content:
        write_text(path, new_content)
        print(f"   CLI package-lock.json updated ({count} occurrences)")


def run(root: Path, new_version: str, what_if: bool) -> int:
    print("Unreal-MCP Version Bump Script")
    print("==============================")

    if not is_semantic_version(new_version):
        print(f"[fail] Invalid semantic version format: {new_version}")
        print("Expected format: major.minor.patch (e.g., '1.2.3')")
        return 1

    current = current_version(root)
    print(f"Current version: {current}")
    print(f"New version:     {new_version}")

    if current == new_version:
        print("[warn] New version is the same as current version")
        return 0

    print("\nScanning for version references...")
    changes = plan_changes(root, new_version)
    if not changes:
        print("[fail] No version references found to update!")
        return 1

    total = sum(c.matches for c in changes)
    if what_if:
        print("\nPreview Summary:")
        print(f"Files to be modified: {len(changes)}")
        print(f"Total replacements: {total}")
        print("\nPreview completed. Use without --what-if to apply changes.")
        return 0

    for change in changes:
        write_text(change.path, change.content)
    bump_lock_file(root, new_version)

    print("\nVersion bump completed successfully!")
    print(f"   Updated {len(changes)} files")
    print(f"   Total replacements: {total}")
    print(f"   Version: {current} -> {new_version}")
    print("\nRemember to commit these changes to git")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--new-version", required=True, help='new semver version, e.g. "0.2.0"')
    parser.add_argument("--what-if", "--dry-run", dest="what_if", action="store_true",
                        help="preview the changes without writing any file")
    parser.add_argument("--root", type=Path, default=ROOT, help="repo root to edit (default: this checkout)")
    args = parser.parse_args(argv)
    try:
        return run(args.root, args.new_version, args.what_if)
    except Exception as exc:  # mirror the .ps1 catch-all: report and exit 1
        print(f"\n[fail] Script failed: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
