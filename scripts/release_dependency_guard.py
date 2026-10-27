#!/usr/bin/env python3
"""
Release dependency guard: refuse a release that would ship a dependency that is not a published release.

Between test waves the plugin's `main` may consume an UNRELEASED dependency: RQL from its main commit,
as the sidecar's `com.IvanMurzak.Rql 0.3.0-dev.<sha>` (restored from `bridge/prerelease-packages/`
through `bridge/Directory.Build.props`) and as the vendored C++ runtime in
`UnrealMCP/Source/ThirdParty/RQL/`. Fine for CI; a release cut in that state ships unreleased code
nobody can rebuild from published packages. This guard runs as the FIRST step of `release.yml` and
`release_extensions.yml` and fails the run (dry runs too) before anything is built or published.

What it checks (everything the core, extension and CLI releases ship):
  * every NuGet pin of the sidecar (`bridge/src/*.csproj`, the `Directory.Build.props` /
    `Directory.Packages.props` above it, MSBuild `$(Property)` versions resolved): a pre-release
    version (any `-` suffix) or a version not listed on nuget.org fails, and so does a local
    (non-URL) restore source (`RestoreAdditionalProjectSources`, `nuget.config`), e.g. `prerelease-packages`;
  * the vendored RQL C++ runtime: its README must record the published `com.IvanMurzak.Rql` release it
    was copied from (`com.IvanMurzak.Rql` <version>: not a pre-release, listed on nuget.org), that version must
    equal the sidecar's `com.IvanMurzak.Rql` pin (the C++ runtime executes the plans the sidecar's RQL
    parses), and its recorded commit must be the `<repository commit>` of that package's nuspec. The
    copied sources carry no version of their own, so the README is what is checked;
  * the vendored Code-Exec Python runtime (`_vendor/code_exec/VENDORED.md`): a published
    `com.IvanMurzak.CodeExec` release;
  * every extension's `minCoreVersion` (`extensions/*/extension.json`) and the npm dependencies of
    `unreal-mcp-cli` (`cli/package.json`): pre-release or non-registry specs fail.

Usage:
    python scripts/release_dependency_guard.py [--root <repo>]
Exit 0 = every pin is a published release; exit 1 = problems listed (file:line: message).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Optional

ROOT = Path(__file__).resolve().parents[1]
NUGET_INDEX_URL = "https://api.nuget.org/v3-flatcontainer/{id}/index.json"
NUGET_NUSPEC_URL = "https://api.nuget.org/v3-flatcontainer/{id}/{version}/{id}.nuspec"

# MSBuild item types that pin a NuGet package version.
PACKAGE_ITEMS = {"PackageReference", "PackageVersion", "GlobalPackageReference", "PackageDownload"}
# MSBuild properties that add NuGet restore sources.
RESTORE_SOURCE_PROPERTIES = {"RestoreSources", "RestoreAdditionalProjectSources", "RestoreFallbackFolders"}
SKIP_DIRS = {"bin", "obj", "node_modules", "publish", "prerelease-packages"}
PROPERTY_RE = re.compile(r"\$\(([A-Za-z_][A-Za-z0-9_.-]*)\)")
NPM_PRERELEASE_RE = re.compile(r"\d+\.\d+\.\d+-")
# A registry semver spec (`1.2.3`, `^1.2`, `~1`, `>=1 <2`, `v1.2.3`); dist-tags, paths, git and URLs are not.
NPM_REGISTRY_SPEC_RE = re.compile(r"^\s*[0-9^~<>=v]")


@dataclass(frozen=True)
class Pin:
    """One dependency version as written in a shipped file."""

    ecosystem: str  # "nuget": pre-release + nuget.org check; "semver": pre-release check only
    package: str
    version: str
    file: Path
    line: int


@dataclass(frozen=True)
class Problem:
    file: Path
    line: int
    message: str


class RegistryError(Exception):
    """nuget.org could not be queried (network / HTTP error other than 404)."""


class NuGetRegistry:
    """Listed versions per package id from the nuget.org flat container (cached; 404 = unknown package)."""

    def __init__(self, fetch: Optional[Callable[[str], Optional[bytes]]] = None) -> None:
        self._fetch = fetch or _http_get
        self._cache: dict[str, Optional[set[str]] | RegistryError] = {}

    def versions(self, package: str) -> Optional[set[str]]:
        key = package.lower()
        if key not in self._cache:
            try:
                body = self._fetch(NUGET_INDEX_URL.format(id=key))
                self._cache[key] = None if body is None else {
                    normalize_nuget_version(v) for v in json.loads(body.decode("utf-8")).get("versions", [])
                }
            except RegistryError as error:  # remembered: one retry cycle per package, not per pin
                self._cache[key] = error
        cached = self._cache[key]
        if isinstance(cached, RegistryError):
            raise cached
        return cached

    def source_commit(self, package: str, version: str) -> Optional[str]:
        """The `<repository commit>` a published package was packed from (None: no nuspec / no commit)."""
        key = package.lower()
        body = self._fetch(NUGET_NUSPEC_URL.format(id=key, version=normalize_nuget_version(version)))
        if body is None:
            return None
        repository = next((e for e in ET.fromstring(body).iter() if _local(e.tag) == "repository"), None)
        return None if repository is None else repository.get("commit")


def _http_get(url: str) -> Optional[bytes]:
    """GET with retries; None on 404; RegistryError when nuget.org stays unreachable."""
    last: Exception | None = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=30) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            last = error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            last = error
        if attempt < 2:
            time.sleep(2 ** attempt)
    raise RegistryError(f"{url}: {last}")


# --- versions ---------------------------------------------------------------------------------


def is_prerelease(version: str) -> bool:
    """SemVer/NuGet pre-release: any `-` suffix on the version (build metadata `+x` alone is not)."""
    return "-" in version.split("+", 1)[0]


def normalize_nuget_version(version: str) -> str:
    """NuGet's normalized form: `1.0` -> `1.0.0`, `1.0.0.0` -> `1.0.0`, leading zeros dropped, metadata dropped."""
    core, _, prerelease = version.split("+", 1)[0].strip().partition("-")
    parts = [str(int(p)) if p.isdigit() else p for p in core.split(".")]
    while len(parts) < 3:
        parts.append("0")
    if len(parts) == 4 and parts[3] == "0":
        parts.pop()
    normalized = ".".join(parts)
    return (normalized + "-" + prerelease if prerelease else normalized).lower()


def nuget_versions_in(spec: str) -> tuple[list[str], Optional[str]]:
    """The concrete versions named by a NuGet version spec (`1.2.3`, `[1.2.3]`, `[1.0,2.0)`), or an error."""
    spec = spec.strip()
    if not spec:
        return [], "empty version"
    if "*" in spec:
        return [], f"floating version '{spec}' is not a pin"
    if spec[0] in "[(":
        if spec[-1] not in "])":
            return [], f"malformed version range '{spec}'"
        lower, _, upper = (b.strip() for b in spec[1:-1].partition(","))
        if "," not in spec:  # `[1.2.3]`: exactly that version
            return [lower], None
        # The lower bound is what restore picks; an exclusive upper bound need not exist (yet).
        return [b for b in (lower, upper if spec[-1] == "]" else "") if b], None
    return [spec], None


# --- MSBuild ----------------------------------------------------------------------------------


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def line_of(text: str, *needles: str) -> int:
    """1-based line of the first line containing every needle (0 when none does)."""
    for number, line in enumerate(text.splitlines(), 1):
        if all(n in line for n in needles):
            return number
    return 0


def rel(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def ancestors(start: Path, root: Path) -> list[Path]:
    """`start` and its parents up to and including `root`, outermost first."""
    root = root.resolve()
    chain = []
    current = start.resolve()
    while True:
        chain.append(current)
        if current == root or current.parent == current:
            break
        current = current.parent
    return list(reversed(chain))


def msbuild_chain(project: Path, root: Path) -> list[Path]:
    """The `Directory.*.props/targets` above a project (outermost first), the project, and the
    `.props`/`.targets`/`.nuspec` files packed from its folder."""
    files: list[Path] = []
    for directory in ancestors(project.parent, root):
        for name in ("Directory.Build.props", "Directory.Packages.props", "Directory.Build.targets"):
            if (directory / name).is_file():
                files.append(directory / name)
    files.append(project)
    for path in walk(project.parent):
        if path.suffix in (".props", ".targets", ".nuspec") and path not in files:
            files.append(path)
    return files


def walk(directory: Path) -> Iterable[Path]:
    """Files of a project's folder, not descending into build output or another project's folder."""
    for entry in sorted(directory.iterdir()):
        if entry.is_dir():
            if entry.name not in SKIP_DIRS and not entry.name.startswith(".") and not any(entry.glob("*.csproj")):
                yield from walk(entry)
        else:
            yield entry


def read_properties(files: list[Path]) -> dict[str, list[str]]:
    """Every static `<PropertyGroup>` definition over the chain (Conditions are not evaluated, so a
    property set more than once keeps every value it may take)."""
    properties: dict[str, list[str]] = {}
    for path in files:
        try:
            tree = ET.parse(path)
        except ET.ParseError:
            continue
        for group in tree.getroot().iter():
            if _local(group.tag) != "PropertyGroup":
                continue
            for prop in group:
                if isinstance(prop.tag, str) and prop.text is not None:
                    values = properties.setdefault(_local(prop.tag), [])
                    if prop.text.strip() not in values:
                        values.append(prop.text.strip())
    return properties


def resolve(value: str, properties: dict[str, list[str]], file: Path) -> list[str]:
    """Every expansion of the `$(Name)`s in `value` over the static definitions; an unknown name stays
    as `$(Name)` (the caller reports it)."""
    builtins = {"MSBuildThisFileDirectory": str(file.parent) + os.sep, "MSBuildProjectDirectory": str(file.parent)}
    results: set[str] = set()
    pending = [(value, 0)]
    while pending:
        current, depth = pending.pop()
        match = PROPERTY_RE.search(current)
        name = match.group(1) if match else ""
        options = [builtins[name]] if name in builtins else properties.get(name)
        if not match or not options or depth >= 16:
            results.add(current)
            continue
        pending += [(current[:match.start()] + option + current[match.end():], depth + 1) for option in options]
    return sorted(results)


def is_url(source: str) -> bool:
    return re.match(r"^[a-z][a-z0-9+.-]*://", source, re.IGNORECASE) is not None and not source.lower().startswith("file:")


def item_version(element: ET.Element) -> Optional[str]:
    """The version an item pins; `VersionOverride` wins over `Version` (central package management)."""
    for name in ("VersionOverride", "Version", "version"):
        if element.get(name) is not None:
            return element.get(name)
    for name in ("VersionOverride", "Version"):
        child = next((c for c in element if isinstance(c.tag, str) and _local(c.tag) == name), None)
        if child is not None and child.text:
            return child.text.strip()
    return None


def scan_msbuild(projects: list[Path], root: Path) -> tuple[list[Pin], list[Problem]]:
    pins: list[Pin] = []
    problems: list[Problem] = []
    for project in (p.resolve() for p in projects):
        chain = msbuild_chain(project, root)
        properties = read_properties(chain)
        for path in chain:  # a shared props file is read per project: its `$(X)` may differ per project
            text = path.read_text(encoding="utf-8-sig")
            try:
                document = ET.fromstring(text)
            except ET.ParseError as error:
                problems.append(Problem(path, 1, f"cannot parse MSBuild/nuspec XML: {error}"))
                continue
            for element in document.iter():
                if not isinstance(element.tag, str):
                    continue
                name = _local(element.tag)
                if name in PACKAGE_ITEMS or (name == "dependency" and path.suffix == ".nuspec"):
                    package = element.get("Include") or element.get("Update") or element.get("id")
                    raw = item_version(element)
                    if not package or raw is None:
                        continue  # a version-less reference takes its pin from a PackageVersion scanned here too
                    line = line_of(text, f'"{package}"', raw) or line_of(text, package, raw)
                    for version in resolve(raw, properties, path):
                        if PROPERTY_RE.search(version):
                            problems.append(Problem(path, line, f"{package}: cannot resolve version '{raw}'"))
                            continue
                        versions, error = nuget_versions_in(version)
                        if error:
                            problems.append(Problem(path, line, f"{package}: {error}"))
                        pins.extend(Pin("nuget", package, v, path, line) for v in versions)
                elif name in RESTORE_SOURCE_PROPERTIES and element.text:
                    for entry in element.text.split(";"):
                        entry = entry.strip()
                        if not entry or entry == f"$({name})":
                            continue
                        if not all(is_url(source) for source in resolve(entry, properties, path)):
                            problems.append(Problem(path, line_of(text, f"<{name}"),
                                                    f"{name} adds the local NuGet feed '{entry}' "
                                                    f"(a release must restore from nuget.org only; delete the "
                                                    f"pre-release feed when the pins return to published versions)"))
    return pins, problems


def scan_nuget_configs(projects: list[Path], root: Path) -> list[Problem]:
    problems: list[Problem] = []
    directories = {d for project in projects for d in ancestors(project.parent, root)}
    for directory in sorted(directories):
        for entry in directory.iterdir():
            if entry.is_file() and entry.name.lower() == "nuget.config":
                text = entry.read_text(encoding="utf-8-sig")
                try:
                    tree = ET.fromstring(text)
                except ET.ParseError as error:
                    problems.append(Problem(entry, 1, f"cannot parse nuget.config: {error}"))
                    continue
                for sources in tree.iter("packageSources"):
                    for add in sources.iter("add"):
                        value = add.get("value", "")
                        if value and not is_url(value):
                            problems.append(Problem(entry, line_of(text, value),
                                                    f"package source '{add.get('key')}' is the local feed '{value}'"))
    return problems


# --- other pin sources ------------------------------------------------------------------------


def scan_package_json(path: Path) -> tuple[list[Pin], list[Problem]]:
    """`dependencies` / `optionalDependencies` (what installs with the package; dev dependencies do not)."""
    text = path.read_text(encoding="utf-8-sig")
    data = json.loads(text)
    pins: list[Pin] = []
    problems: list[Problem] = []
    for section in ("dependencies", "optionalDependencies"):
        for package, spec in (data.get(section) or {}).items():
            line = line_of(text, f'"{package}"')
            if not isinstance(spec, str) or not NPM_REGISTRY_SPEC_RE.match(spec):
                problems.append(Problem(path, line, f"{package}: '{spec}' is not a registry version"))
            else:
                pins.append(Pin("semver", package, spec, path, line))
    return pins, problems


def check_pins(pins: list[Pin], registry: NuGetRegistry) -> list[Problem]:
    problems: list[Problem] = []
    for pin in pins:
        prerelease = is_prerelease(pin.version) if pin.ecosystem == "nuget" else bool(NPM_PRERELEASE_RE.search(pin.version))
        if prerelease:
            problems.append(Problem(pin.file, pin.line, f"{pin.package} {pin.version} is a pre-release version"))
            continue
        if pin.ecosystem != "nuget":
            continue
        try:
            listed = registry.versions(pin.package)
        except RegistryError as error:
            problems.append(Problem(pin.file, pin.line, f"{pin.package} {pin.version}: cannot verify on nuget.org ({error})"))
            continue
        if listed is None:
            problems.append(Problem(pin.file, pin.line, f"{pin.package} is not a package on nuget.org"))
        elif normalize_nuget_version(pin.version) not in listed:
            problems.append(Problem(pin.file, pin.line, f"{pin.package} {pin.version} is not published on nuget.org"))
    return problems


def require(path: Path, problems: list[Problem]) -> bool:
    """A shipped file the guard expects; missing = the guard is out of date, which must fail too."""
    if path.is_file():
        return True
    problems.append(Problem(path, 1, f"expected file is missing (update {Path(__file__).name})"))
    return False


# --- this repository --------------------------------------------------------------------------

RQL_PACKAGE = "com.IvanMurzak.Rql"
RQL_VENDORED = Path("UnrealMCP/Source/ThirdParty/RQL/README.md")
CODE_EXEC_VENDORED = Path("UnrealMCP/Content/Python/unreal_mcp/_vendor/code_exec/VENDORED.md")
RQL_COMMIT_RE = re.compile(r"commit\s+`([0-9a-fA-F]{7,40})`")
RQL_MARKER_RE = re.compile(r"`com\.IvanMurzak\.Rql`\s*`?\s*([0-9][0-9A-Za-z.+-]*[0-9A-Za-z])")
VENDORED_RELEASE_RE = re.compile(r"release\s+\*\*([^*\s]+)\*\*")


def scan_vendored(path: Path, package: str, marker: re.Pattern[str] = VENDORED_RELEASE_RE) -> tuple[list[Pin], list[Problem]]:
    """A vendored source copy records the package release it came from (`marker` captures the version)."""
    text = path.read_text(encoding="utf-8-sig")
    match = marker.search(text)
    if not match:
        return [], [Problem(path, 1, f"vendored {package}: no release version marker (copy it from a published release)")]
    return [Pin("nuget", package, match.group(1), path, text.count("\n", 0, match.start()) + 1)], []


def collect(root: Path) -> tuple[list[Pin], list[Problem]]:
    """Every dependency pin Unreal-MCP's core, extension and CLI releases ship."""
    problems: list[Problem] = []
    projects = sorted(root.glob("bridge/src/*.csproj"))
    if not projects:
        problems.append(Problem(root / "bridge" / "src", 1, f"no sidecar csproj found (update {Path(__file__).name})"))
    pins, msbuild_problems = scan_msbuild(projects, root)
    problems += msbuild_problems + scan_nuget_configs(projects, root)
    bridge_rql = {p.version for p in pins if p.package.lower() == RQL_PACKAGE.lower()}
    if require(root / RQL_VENDORED, problems):
        rql_pins, rql_problems = scan_vendored(root / RQL_VENDORED, RQL_PACKAGE, RQL_MARKER_RE)
        pins += rql_pins
        problems += rql_problems
        for pin in rql_pins:
            if bridge_rql and pin.version not in bridge_rql:
                problems.append(Problem(pin.file, pin.line, f"vendored RQL C++ runtime is {RQL_PACKAGE} {pin.version} "
                                        f"but the sidecar pins {', '.join(sorted(bridge_rql))}"))
    if require(root / CODE_EXEC_VENDORED, problems):
        code_exec_pins, code_exec_problems = scan_vendored(root / CODE_EXEC_VENDORED, "com.IvanMurzak.CodeExec")
        pins += code_exec_pins
        problems += code_exec_problems
    for descriptor in sorted(root.glob("extensions/*/extension.json")):
        text = descriptor.read_text(encoding="utf-8-sig")
        minimum = json.loads(text).get("minCoreVersion")
        if minimum:
            pins.append(Pin("semver", "Unreal-MCP core (minCoreVersion)", minimum, descriptor, line_of(text, '"minCoreVersion"')))
    cli_package = root / "cli" / "package.json"
    if require(cli_package, problems):
        npm_pins, npm_problems = scan_package_json(cli_package)
        pins += npm_pins
        problems += npm_problems
    if not any(p.ecosystem == "nuget" for p in pins):
        problems.append(Problem(root / "bridge", 1, f"no NuGet pins found (update {Path(__file__).name})"))
    return pins, problems


def check_vendored_rql_commit(root: Path, pins: list[Pin], registry: NuGetRegistry) -> list[Problem]:
    """The README's commit must be the one the published package was packed from: a re-vendor from an
    unreleased commit that keeps the old version marker would otherwise pass."""
    readme = root / RQL_VENDORED
    marker = next((p for p in pins if p.file == readme and p.package == RQL_PACKAGE), None)
    if marker is None or is_prerelease(marker.version):
        return []  # no marker / a pre-release marker is already reported
    text = readme.read_text(encoding="utf-8-sig")
    commit = RQL_COMMIT_RE.search(text)
    if not commit:
        return [Problem(readme, marker.line, "vendored RQL C++ runtime: no source commit (commit `<sha>`) recorded")]
    try:
        published = registry.source_commit(RQL_PACKAGE, marker.version)
    except (RegistryError, ET.ParseError) as error:
        return [Problem(readme, marker.line, f"{RQL_PACKAGE} {marker.version}: cannot read the nuspec on nuget.org ({error})")]
    if published is None:
        return []  # an unpublished version is reported by check_pins; a nuspec without a commit cannot be compared
    if not published.lower().startswith(commit.group(1).lower()):
        return [Problem(readme, text.count("\n", 0, commit.start()) + 1,
                        f"vendored RQL C++ runtime is commit {commit.group(1)} but {RQL_PACKAGE} {marker.version} "
                        f"was packed from {published[:12]} (copy the sources of that release)")]
    return []


def run(root: Path, registry: NuGetRegistry) -> tuple[list[Pin], list[Problem]]:
    pins, problems = collect(root)
    pins = list(dict.fromkeys(pins))
    problems += check_pins(pins, registry) + check_vendored_rql_commit(root, pins, registry)
    unique = sorted(set(problems), key=lambda p: (rel(p.file, root), p.line, p.message))
    return pins, unique


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    pins, problems = run(root, NuGetRegistry())
    files = sorted({rel(p.file, root) for p in pins})
    print(f"Release dependency guard: {len(pins)} pins in {len(files)} shipped files")
    for name in files:
        print(f"  {name}")
    if not problems:
        print("OK: every dependency pin is a published release.")
        return 0
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"
    print(f"\nFAIL: {len(problems)} problem(s); this release would ship dependencies users cannot restore:")
    for problem in problems:
        location, line = rel(problem.file, root), problem.line or 1
        print(f"  {location}:{line}: {problem.message}")
        if annotate:
            print(f"::error file={location},line={line}::{problem.message}")
    print("\nSwitch every pin back to a published release (and delete local pre-release feeds) before releasing.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
