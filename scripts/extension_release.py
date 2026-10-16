"""Independent extension versions/releases. No core/CLI/bridge publication.

Run validate/package locally; publish only from the gated release_extensions workflow.
The shared extensions-index release is NOT GitHub's latest release (reserved for core).
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPO = "IvanMurzak/Unreal-MCP"
INDEX_TAG = "extensions-index"
VERSION = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\Z")
SLUGS = {"UnrealAINiagara": "niagara", "UnrealAIEnhancedInput": "enhanced-input",
         "UnrealAIPCG": "pcg", "UnrealAIGAS": "gas", "UnrealAILandscape": "landscape",
         "UnrealAIControlRig": "control-rig"}


def components() -> dict[str, Path]:
    found = {p.stem: p.parent for p in sorted((ROOT / "extensions").glob("*/*.uplugin"))}
    for metadata in (ROOT / "extensions").glob("*/extension.json"):
        if metadata.parent.name not in found or found[metadata.parent.name] != metadata.parent:
            raise ValueError(f"Extension metadata has no matching top-level plugin: {metadata}")
    return found


def slug(plugin: str) -> str:
    return SLUGS.get(plugin, re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "-", plugin.removeprefix("UnrealAI")).lower())


def read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path: Path, value: dict) -> None:
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temp.replace(path)


def validate(plugin: str) -> dict:
    folder = components()[plugin]
    descriptor = read(folder / f"{plugin}.uplugin")
    metadata = read(folder / "extension.json")
    version = descriptor["VersionName"]
    if not VERSION.fullmatch(version) or metadata["version"] != version or metadata["pluginName"] != plugin:
        raise ValueError(f"Version/identity drift in {plugin}")
    providers = "\n".join(p.read_text(encoding="utf-8-sig") for p in (folder / "Source").rglob("*.cpp"))
    if not re.search(r'GetExtensionVersion\(\).*?TEXT\("' + re.escape(version) + r'"\)', providers):
        raise ValueError(f"Provider version drift in {plugin}")
    if not list((folder / "Tests/e2e/tools").glob("*.e2e.ps1")):
        raise ValueError(f"No E2E checks for {plugin}")
    ids = [t["name"] for t in metadata["tools"]]
    if len(ids) != len(set(ids)):
        raise ValueError(f"Duplicate tool names in {plugin}")
    return metadata


def bump(plugin: str, version: str) -> None:
    metadata = validate(plugin)
    if not VERSION.fullmatch(version) or tuple(map(int, version.split('.'))) <= tuple(map(int, metadata['version'].split('.'))):
        raise ValueError("New version must be a greater stable semver")
    folder = components()[plugin]
    for path in (folder / f"{plugin}.uplugin", folder / "extension.json"):
        data = read(path)
        key = "VersionName" if path.suffix == ".uplugin" else "version"
        data[key] = version
        if key == "VersionName":
            data["Version"] = int(data.get("Version", 0)) + 1
        write(path, data)
    for path in (folder / "Source").rglob("*.cpp"):
        text = path.read_text(encoding="utf-8-sig")
        updated = re.sub(r'(GetExtensionVersion\(\).*?TEXT\(")' + re.escape(metadata['version']) + r'("\))',
                         lambda m: m[1] + version + m[2], text)
        if updated != text:
            path.write_text(updated, encoding="utf-8")
    validate(plugin)


def package(plugin: str, output: Path) -> Path:
    metadata = validate(plugin)
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"{plugin}-{metadata['version']}.zip"
    folder = components()[plugin]
    # Deterministic, source-only package. No sibling extension or build cache can leak in.
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(folder.rglob("*")):
            rel = path.relative_to(folder)
            if not path.is_file() or rel.parts[0] not in {"Source", "Content", "Config", "Resources", f"{plugin}.uplugin", "LICENSE", "README.md"}:
                continue
            if any(part in {"Binaries", "Intermediate", ".git", "Saved"} for part in rel.parts):
                continue
            info = zipfile.ZipInfo(f"{plugin}/{rel.as_posix()}", date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            bundle.writestr(info, path.read_bytes())
    return archive


def update_index(index: dict, metadata: dict, tag: str, checksum: str) -> dict:
    """Keep all historic versions and every untouched extension. Never roll latest backwards."""
    result = json.loads(json.dumps(index))
    if result.get("schemaVersion") != 1 or not isinstance(result.get("extensions"), list):
        raise ValueError("Unsupported index schema")
    entry = next((e for e in result["extensions"] if e["extensionId"] == metadata["extensionId"]), None)
    if entry and entry["pluginName"] != metadata["pluginName"]:
        raise ValueError("Cannot change an existing extension's plugin identity")
    if entry is None:
        entry = {**metadata, "releases": {}}
        result["extensions"].append(entry)
    version = metadata["version"]
    release = {"repo": REPO, "tag": tag, "sha256": checksum,
               "minCoreVersion": metadata["minCoreVersion"], "enginePlugins": metadata["enginePlugins"]}
    existing = entry["releases"].get(version)
    if existing and existing["repo"] == REPO and existing != release:
        raise ValueError("A published component version is immutable; bump its version")
    entry["releases"][version] = release
    if tuple(map(int, version.split('.'))) >= tuple(map(int, entry["version"].split('.'))):
        history = entry["releases"]
        entry.update(metadata)
        entry.update(repo=REPO, releases=history)
    return result


def gh(*args: str) -> str:
    return subprocess.check_output(["gh", *args], text=True).strip()


def publish(plugin: str, archive: Path, *, bootstrap: bool = False) -> None:
    """Caller serializes ALL index writes with workflow concurrency, including reruns."""
    metadata = validate(plugin)
    tag = f"{slug(plugin)}-v{metadata['version']}"
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    sha = gh("api", f"repos/{REPO}/commits/HEAD", "--jq", ".sha")
    local_sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if sha != local_sha:
        raise ValueError("Publish requires the current default-branch commit")
    with tempfile.TemporaryDirectory(prefix="extension-release-", dir=archive.parent) as temporary:
        work = Path(temporary)
        index_path = work / "extensions-index.json"
        if bootstrap:
            # A batch may have created the index before a later component failed.
            # On a full workflow retry, continue from that live index, never reset it.
            exists = subprocess.run(["gh", "release", "view", INDEX_TAG, "--repo", REPO], capture_output=True)
            if exists.returncode == 0:
                bootstrap = False
            else:
                detail = exists.stderr.decode('utf-8', errors='replace')
                if 'release not found' not in detail.lower() and 'HTTP 404' not in detail:
                    raise ValueError('Cannot determine whether the live index exists: ' + detail[:300])
        if bootstrap:
            index = read(ROOT / "extensions-index.json")
        else:
            gh("release", "download", INDEX_TAG, "--repo", REPO, "--pattern", "extensions-index.json", "--dir", str(work))
            index = read(index_path)
        updated = update_index(index, metadata, tag, checksum)
        tags = json.loads(gh("api", f"repos/{REPO}/releases?per_page=100"))
        release = next((r for r in tags if r["tag_name"] == tag), None)
        if release is None:
            gh("release", "create", tag, "--repo", REPO, "--target", local_sha, "--draft", "--latest=false",
               "--title", f"{plugin} {metadata['version']}", "--notes",
               f"Source-only {plugin} release. Requires Unreal-MCP >= {metadata['minCoreVersion']}.", str(archive))
        elif release['draft']:
            # A failed draft upload is resumable; published artifacts are never replaced.
            gh("release", "upload", tag, str(archive), "--repo", REPO, "--clobber")
        # Verify the actual uploaded bytes, including on retries. Never overwrite published ZIPs.
        gh("release", "download", tag, "--repo", REPO, "--pattern", archive.name, "--dir", str(work))
        if hashlib.sha256((work / archive.name).read_bytes()).hexdigest() != checksum:
            raise ValueError("Uploaded archive differs; use a new component version")
        gh("release", "edit", tag, "--repo", REPO, "--draft=false", "--latest=false")
        write(index_path, updated)
        if bootstrap:
            gh("release", "create", INDEX_TAG, "--repo", REPO, "--target", local_sha, "--latest=false",
               "--title", "Extension download index", "--notes", "Machine-readable stable extension versions.", str(index_path))
        else:
            gh("release", "upload", INDEX_TAG, str(index_path), "--repo", REPO, "--clobber")
        verify = work / "verify"
        verify.mkdir()
        gh("release", "download", INDEX_TAG, "--repo", REPO, "--pattern", index_path.name, "--dir", str(verify))
        if read(verify / index_path.name) != updated:
            raise ValueError("Published index verification failed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["validate", "bump", "package", "publish"])
    parser.add_argument("--plugin", choices=list(components()))
    parser.add_argument("--version")
    parser.add_argument("--output", type=Path, default=ROOT / ".artifacts/releases")
    parser.add_argument("--bootstrap-index", action="store_true")
    args = parser.parse_args()
    if args.action == "validate":
        for plugin in components():
            print(plugin, validate(plugin)["version"])
        return
    if not args.plugin:
        parser.error("--plugin is required")
    if args.action == "bump":
        bump(args.plugin, args.version or "")
    else:
        archive = package(args.plugin, args.output)
        print(archive)
        if args.action == "publish":
            import os
            if os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("GITHUB_REF") != "refs/heads/main":
                parser.error("Publication is only permitted in the gated workflow on main")
            publish(args.plugin, archive, bootstrap=args.bootstrap_index)


if __name__ == "__main__":
    main()
