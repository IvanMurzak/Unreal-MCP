#!/usr/bin/env python3
# Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
# See the LICENSE file in the repository root for more information.
"""
Toggle the UnrealMcpEditorTests Automation module in/out of UnrealMCP.uplugin.

The committed UnrealMCP.uplugin is the DISTRIBUTED descriptor and deliberately OMITS the
UnrealMcpEditorTests module: Fab review flags shipped test/automation modules, and
BuildPlugin -Rocket must produce a marketplace-clean package (issue #139, C2). But PR/dev CI still
needs to compile + load that module to run the UnrealMcp. Automation specs.

This script edits the .uplugin's Modules array in place:
  --action Add     -> append the test module (run BEFORE the Automation pass)
  --action Remove  -> strip it back out  (run AFTER the Automation pass, in finally)
Both actions are IDEMPOTENT (Add is a no-op if present; Remove a no-op if absent), so a crashed run
that skipped the revert never leaves a wedged descriptor: the next Add/Remove (or a `git checkout`)
restores the intended state.

NEVER commit the descriptor with the test module added: this script is a transient dev/CI mutation
only. The distributed/committed form has NO test module.

The descriptor is rewritten as 2-space-indented JSON with its key order kept, in its existing line
ending style, UTF-8 without a BOM, newline-terminated. Inline arrays such as
`"PlatformAllowList": [ "Win64", "Mac", "Linux" ]` come back expanded one item per line, so a local
Add -> Remove can leave a cosmetic (formatting-only) working-tree diff on UnrealMCP.uplugin. It is
harmless; clean it with `git checkout UnrealMCP/UnrealMCP.uplugin`.

Usage:
    python commands/test_module_uplugin.py --action Add
    <editor> <project> -ExecCmds="Automation RunTests UnrealMcp.; Quit" ...
    python commands/test_module_uplugin.py --action Remove
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import List, Optional

DEFAULT_UPLUGIN = Path(__file__).resolve().parents[1] / "UnrealMCP" / "UnrealMCP.uplugin"
TEST_MODULE_NAME = "UnrealMcpEditorTests"


def toggle(uplugin: Path, action: str) -> None:
    if not uplugin.is_file():
        raise RuntimeError(f"UnrealMCP.uplugin not found at '{uplugin}'.")

    raw = uplugin.read_bytes().decode("utf-8-sig")
    newline = "\r\n" if "\r\n" in raw else "\n"
    descriptor = json.loads(raw)
    modules = descriptor.get("Modules") if isinstance(descriptor, dict) else None
    if modules is None:
        raise RuntimeError("UnrealMCP.uplugin has no Modules array -- refusing to edit.")
    if isinstance(modules, dict):  # a lone object, as ConvertFrom-Json would have enumerated it
        modules = [modules]

    # Module names compare case-insensitively, like the PowerShell original's -eq.
    def is_test(module: object) -> bool:
        return isinstance(module, dict) and str(module.get("Name", "")).lower() == TEST_MODULE_NAME.lower()

    present = any(is_test(m) for m in modules)
    if action == "Add":
        if present:
            print(f"[{TEST_MODULE_NAME}] already present in {uplugin.name} -- no change.")
        else:
            # Mirror the editor module's shape: Type Editor, LoadingPhase Default.
            modules = modules + [{"Name": TEST_MODULE_NAME, "Type": "Editor", "LoadingPhase": "Default"}]
            print(f"[{TEST_MODULE_NAME}] ADDED for the dev/CI Automation pass (transient -- do NOT commit).")
    else:
        if not present:
            print(f"[{TEST_MODULE_NAME}] not present -- descriptor already in its distributed form.")
        else:
            modules = [m for m in modules if not is_test(m)]
            print(f"[{TEST_MODULE_NAME}] REMOVED -- descriptor restored to its distributed (Fab-clean) form.")

    descriptor["Modules"] = modules
    out = json.dumps(descriptor, indent=2, ensure_ascii=False) + "\n"
    # UTF-8 WITHOUT a BOM keeps the transient diff clean and matches the committed descriptor.
    with uplugin.open("w", encoding="utf-8", newline="") as f:
        f.write(out.replace("\n", newline))


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--action", required=True, choices=("Add", "Remove"), type=_action,
                        help="Add | Remove (case-insensitive)")
    parser.add_argument("--uplugin", type=Path, default=DEFAULT_UPLUGIN,
                        help="path to UnrealMCP.uplugin (default: the one in this checkout)")
    args = parser.parse_args(argv)
    try:
        toggle(args.uplugin, args.action)
    except (RuntimeError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def _action(value: str) -> str:
    # PowerShell's ValidateSet is case-insensitive; normalise to the canonical spelling.
    for choice in ("Add", "Remove"):
        if value.lower() == choice.lower():
            return choice
    return value


if __name__ == "__main__":
    raise SystemExit(main())
