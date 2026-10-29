#!/usr/bin/env python3
"""Resolve this runner's Unreal Engine install for one engine version and export it to the job.

The self-hosted UE jobs run on any runner carrying the engine's `unreal-<major>-<minor>` label, whatever
its OS (Windows, macOS or Linux). The `.github/actions/ue-env` action runs this script with the engine's
own bundled Python, found in the engine the action picked (`UE_ROOT_<major>_<minor>` in the runner's own
environment, else the Epic Games Launcher default; Linux has no default). It turns that engine into the
runner-local paths every later step uses, and appends them to GITHUB_ENV:

  UE_ROOT        the engine root: the install whose bundled Python runs this script, checked against the
                 requested version.
  UE_PLATFORM    the UBT/UAT target platform of this host: Win64, Mac or Linux.
  UE_RUNUAT      RunUAT.bat (Windows) or RunUAT.sh.
  UE_BUILD       the host's UBT Build script (Build.bat, Mac/Build.sh, Linux/Build.sh).
  UE_EDITOR_CMD  the host's UnrealEditor-Cmd (.exe on Windows).
  CHAIN_PY       UE's bundled Python (the interpreter running this script) through a space-free link
                 under RUNNER_TEMP. The runner accounts have no Python on PATH, and the engine path can
                 contain spaces, which ${{ env.CHAIN_PY }} (substituted UNQUOTED) would split. It is the
                 hardened chain_feed.py contract: an ABSOLUTE, SPACE-FREE, forward-slashed interpreter
                 path. Every later Python step runs as `${{ env.CHAIN_PY }} <script>`.
  SSL_CERT_FILE  macOS/Linux only, unless already set: the OS CA bundle. UE's bundled Python ships no CA
                 certificates there, so its HTTPS downloads would fail to verify.
  DEVELOPER_DIR  macOS only, and only when the machine's selected Xcode is outside the engine's
                 supported range: a side-by-side /Applications/Xcode*.app that fits.

The link is a directory junction on Windows and a symlink elsewhere. The job's cleanup step removes it
with `--remove-link`, never with a recursive delete: that would follow the link into the engine install.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from extension_suite import UE_BUILD, UE_EDITOR, UE_PLATFORM  # noqa: E402  (the one host-platform table)
import ue_json  # noqa: E402  (engine JSON is JSONC)

IS_WINDOWS = os.name == "nt"
IS_MAC = sys.platform == "darwin"


def link_path() -> Path:
    return Path(os.environ["RUNNER_TEMP"]) / "chain-py"


def remove_link(link: Path) -> None:
    """Remove ONLY the link: rmdir on a junction / unlink on a symlink never touch the files behind it."""
    if not os.path.lexists(link):
        return
    if IS_WINDOWS:
        os.rmdir(link)
    else:
        os.unlink(link)


def add_job_env(name: str, value: str) -> None:
    if any(c in value for c in "\r\n\0"):
        raise SystemExit(f"refusing to export {name}: its value contains a line break or NUL")
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as handle:
        handle.write(f"{name}={value}\n")
    print(f"ue-env: {name}={value}", flush=True)


def engine_root(version: str) -> Path:
    """The install whose bundled Python runs this script (<root>/Engine/Binaries/ThirdParty/Python3/<platform>/...)."""
    executable = Path(sys.executable).absolute()
    root = next((p.parents[1] for p in executable.parents if p.name == "Binaries" and p.parent.name == "Engine"), None)
    if root is None or not (root / "Engine/Build/Build.version").is_file():
        raise SystemExit(f"ue-env: run me with the engine's bundled Python (the .github/actions/ue-env action); got {executable}")
    installed = ue_json.read(root / "Engine/Build/Build.version")
    found = f"{installed['MajorVersion']}.{installed['MinorVersion']}"
    if found != version:
        raise SystemExit(f"ue-env: {root} is UE {found}, not the requested {version}")
    return root


def xcode3(text: str):
    """A three-part comparable version: "15.2" must not sort below "15.2.0"."""
    if not re.fullmatch(r"\d+(\.\d+){1,3}", text or ""):
        return None
    parts = [int(p) for p in text.split(".")][:3]
    return tuple(parts + [0] * (3 - len(parts)))


def xcode_version(app: Path):
    result = subprocess.run(["/usr/libexec/PlistBuddy", "-c", "Print :CFBundleShortVersionString",
                             str(app / "Contents/Info.plist")], capture_output=True, text=True)
    return xcode3(result.stdout.strip()) if result.returncode == 0 else None


def xcode_range(root: Path) -> tuple:
    """The engine's supported Xcode range from Apple_SDK.json (JSONC in UE 5.5: comments, trailing commas)."""
    sdk = ue_json.read(root / "Engine/Config/Apple/Apple_SDK.json")
    return xcode3(sdk["MinVersion"]), xcode3(sdk["MaxVersion"])


def developer_dir(root: Path, version: str):
    """macOS: a fitting side-by-side Xcode when the selected one is outside the engine's range, else None.

    UBT finds Xcode with `xcode-select -p`, which honours DEVELOPER_DIR. See docs/RELEASING.md "Which OS".
    """
    low, high = xcode_range(root)

    def fits(v) -> bool:
        return v is not None and low <= v <= high

    # No developer directory selected (xcode-select exits non-zero, prints nothing): treat as out of range.
    selected = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True).stdout.strip()
    selected_app = Path(selected).parent.parent if selected else None
    selected_version = xcode_version(selected_app) if selected_app else None
    if fits(selected_version):
        return None
    candidates = sorted(((xcode_version(app), app) for app in Path("/Applications").glob("Xcode*.app") if app.is_dir()),
                        key=lambda pair: pair[0] or (0, 0, 0), reverse=True)
    pick = next(((v, app) for v, app in candidates if fits(v)), None)
    shown = ".".join(map(str, selected_version)) if selected_version else "none"
    if pick is None:
        raise SystemExit(f"ue-env: UE {version} needs Xcode {'.'.join(map(str, low))}..{'.'.join(map(str, high))}, the selected "
                         f"one is {shown} ({selected_app or '(none)'}) and no /Applications/Xcode*.app fits; install one "
                         "next to it (see docs/RELEASING.md)")
    print(f"ue-env: selected Xcode {shown} is outside UE {version}'s range; this job uses Xcode "
          f"{'.'.join(map(str, pick[0]))} ({pick[1]})", flush=True)
    return pick[1] / "Contents/Developer"


def link_python() -> str:
    """Link this interpreter's folder (not just the binary, so it finds its stdlib) to a space-free path."""
    link = link_path()
    if re.search(r"\s", str(link)):
        raise SystemExit(f"ue-env: RUNNER_TEMP '{link.parent}' contains whitespace; CHAIN_PY must be space-free")
    executable = Path(sys.executable).resolve()
    # Windows: <root>/python.exe; elsewhere: <root>/bin/python3.
    folder = executable.parent if IS_WINDOWS else executable.parent.parent
    remove_link(link)
    if os.path.lexists(link):
        raise SystemExit(f"ue-env: could not remove stale {link}")
    if IS_WINDOWS:
        result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(folder)])
        if result.returncode != 0:
            raise SystemExit(f"ue-env: mklink /J {link} -> {folder} failed (exit {result.returncode})")
    else:
        os.symlink(folder, link, target_is_directory=True)
    python = (link / executable.relative_to(folder)).as_posix()
    if not re.fullmatch(r"([A-Za-z]:)?/\S+", python) or not Path(python).is_file():
        raise SystemExit(f"ue-env: CHAIN_PY '{python}' is not an absolute, space-free interpreter path")
    # Prove it RUNS with the same probe the print-fragment resolvers use (C-extension imports + 3.9+).
    probe = subprocess.run([python, "-c", "import sys, select, zlib, hashlib; sys.version_info >= (3, 9) or sys.exit(1); "
                            "print(sys.executable.replace(chr(92), chr(47)))"], capture_output=True, text=True)
    if probe.returncode != 0:
        raise SystemExit(f"ue-env: {python} is not a runnable Python 3.9+ (exit {probe.returncode})")
    print(f"ue-env: probe sys.executable={probe.stdout.strip()}", flush=True)
    return python


def resolve(version: str) -> None:
    root = engine_root(version)
    developer = developer_dir(root, version) if IS_MAC else None
    run_uat = root / f"Engine/Build/BatchFiles/RunUAT.{'bat' if IS_WINDOWS else 'sh'}"
    ue_build, editor_cmd = root / UE_BUILD, root / UE_EDITOR
    for required in (run_uat, ue_build, editor_cmd):
        if not required.is_file():
            raise SystemExit(f"ue-env: {required} not found")
    ca_bundle = None
    if not IS_WINDOWS and not os.environ.get("SSL_CERT_FILE"):
        ca_bundle = next((c for c in ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt") if os.path.isfile(c)), None)
    python = link_python()

    add_job_env("UE_ROOT", str(root))
    add_job_env("UE_PLATFORM", UE_PLATFORM)
    add_job_env("UE_RUNUAT", str(run_uat))
    add_job_env("UE_BUILD", str(ue_build))
    add_job_env("UE_EDITOR_CMD", str(editor_cmd))
    add_job_env("CHAIN_PY", python)
    if developer:
        add_job_env("DEVELOPER_DIR", str(developer))
    if ca_bundle:
        add_job_env("SSL_CERT_FILE", ca_bundle)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--version", help="the matrix engine version, <major>.<minor>")
    group.add_argument("--remove-link", action="store_true", help="remove only the CHAIN_PY link (best effort, never fails)")
    args = parser.parse_args()
    if args.remove_link:
        try:
            remove_link(link_path())
        except OSError as error:
            print(f"ue-env: could not remove {link_path()} ({error})")
        return 0
    if not re.fullmatch(r"\d+\.\d+", args.version):
        parser.error("--version must be <major>.<minor>")
    resolve(args.version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
