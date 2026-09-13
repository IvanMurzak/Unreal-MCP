#!/usr/bin/env python3
"""
chain_leg_checks.py — Unreal-MCP's own companions to the vendored chain_feed.py.

NOT vendored. `chain_feed.py` next to this file is byte-identical to ai-game-dev-software
`.scripts/chain/leg/chain_feed.py` and must never be edited here, so the two checks this repo's
chain legs need beyond `chain_feed.py record` live in this file and import it.

  pin-drift        Export CHAIN_PIN_DRIFT: one line naming every committed pin the chain override
                   replaced in this job (a csproj PackageReference, the cli/package.json
                   dependency, cli/src/lib/server-version.ts SERVER_VERSION). The workflow passes
                   it to `record --warn`. NEVER fails the job: an error becomes a named warning.
  assets-identity  Prove that EVERY listed project's obj/project.assets.json resolved the lock's
                   ws version of each NuGet package the override forced. `record` proves only the
                   RECIPES pins (bridge/src); the xUnit project restores on its own. Exit 3 on any
                   mismatch, 2 on a refusal, 0 on an ordinary run (no lock).

Both take the SAME `--job` / `--edges` as the job's `apply` and `record`.
Stdlib only: runs on hosted python3 and on UE's bundled Python 3.11.
"""

import argparse
import json
import re
import sys
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import chain_feed as cf  # noqa: E402  (vendored sibling)

NODE = "unreal-mcp"
SERVER_VERSION_FILE = "cli/src/lib/server-version.ts"
NPM_DEP_KEYS = ("dependencies", "devDependencies", "optionalDependencies", "peerDependencies")
#: The warning is substituted into a `run:` line through `${{ env.CHAIN_PIN_DRIFT }}` under both
#: bash and pwsh, so it may carry no quote, `$` or backtick. (`@` is literal inside double quotes in
#: both shells and names every scoped npm package.)
_UNSAFE = re.compile(r"[^A-Za-z0-9 ._:;,/()+=<>\[\]~^@-]")
MAX_WARNING_CHARS = 900


def build_context(args):
    namespace = types.SimpleNamespace(
        node=NODE, job=args.job, lock=None, lock_file=None, checkout=None, runner_temp=None,
        edges=args.edges,
    )
    return cf.build_context(namespace)


def csproj_pins(path):
    text = Path(path).read_text(encoding="utf-8-sig")
    return {
        m.group(1): m.group(2)
        for m in re.finditer(r'<PackageReference\s+Include="([^"]+)"\s+Version="([^"]+)"', text)
    }


def drift_notes(ctx):
    edges, _ = cf.scoped_edges(ctx)
    notes = []

    nuget = [edge for edge in edges if edge["mode"] == "nuget"]
    if nuget:
        wanted = {row["package_id"]: row["ws_version"] for row in cf.nuget_rows_for(ctx, nuget)}
        pins = []
        for edge in nuget:
            for pin in cf.pins_in_scope(edge, ctx.scope):
                if pin not in pins:
                    pins.append(pin)
        for pin in pins:
            committed = csproj_pins(ctx.checkout / pin)
            for package_id in sorted(wanted):
                if package_id in committed and committed[package_id] != wanted[package_id]:
                    notes.append("%s pins %s %s -> %s" % (pin, package_id, committed[package_id], wanted[package_id]))

    for edge in edges:
        want = ctx.ws_version(edge["node"])
        if edge["mode"] == "npm":
            package_id = cf.RECIPES[edge["node"]]["artifacts"][edge["artifact"]]["id"]
            manifest = Path(edge.get("path") or ".") / "package.json"
            data = json.loads((ctx.checkout / manifest).read_text(encoding="utf-8-sig"))
            spec = next((data[key][package_id] for key in NPM_DEP_KEYS if package_id in (data.get(key) or {})), None)
            if spec is not None and spec != want:
                notes.append("%s pins %s %s -> %s" % (manifest.as_posix(), package_id, spec, want))
        elif edge["mode"] == "server-binary":
            text = (ctx.checkout / SERVER_VERSION_FILE).read_text(encoding="utf-8")
            match = re.search(r"SERVER_VERSION\s*=\s*['\"]([^'\"]+)['\"]", text)
            committed = match.group(1) if match else "unreadable"
            if committed != want:
                notes.append("%s SERVER_VERSION %s -> %s via %s" % (SERVER_VERSION_FILE, committed, want, edge.get("env")))
    return notes


def cmd_pin_drift(args):
    try:
        ctx = build_context(args)
        if ctx is None:
            return 0
        notes = drift_notes(ctx)
        text = ("pin drift, the chain override replaced committed pins: " + "; ".join(notes)) if notes else ""
    except cf.Refusal as exc:
        text = "pin drift not computed: %s" % exc
    except Exception as exc:  # a caveat must never be the reason a leg goes red
        text = "pin drift not computed: %s: %s" % (type(exc).__name__, exc)
    text = _UNSAFE.sub("?", text)[:MAX_WARNING_CHARS]
    cf.log("chain: CHAIN_PIN_DRIFT=%s" % (text or "(no drift)"))
    cf._write_github_env([("CHAIN_PIN_DRIFT", text)])
    return 0


def cmd_assets_identity(args):
    try:
        ctx = build_context(args)
    except cf.Refusal as exc:
        cf.log("chain: refused: %s" % exc)
        return 2
    if ctx is None:
        return 0
    edges, _ = cf.scoped_edges(ctx)
    nuget = [edge for edge in edges if edge["mode"] == "nuget"]
    rows = cf.nuget_rows_for(ctx, nuget) if nuget else []
    if not rows:
        cf.log("chain: FAIL the override forced no NuGet package in this job's scope; nothing to prove")
        return 3
    # The packages the node's own `consumes:` names MUST resolve at ws in every project. A sibling
    # artifact of the same producer (McpPlugin.Server) is forced too but may be unreferenced.
    required = {cf.RECIPES[edge["node"]]["artifacts"][edge["artifact"]]["id"] for edge in nuget}
    failures = 0
    for project in args.project:
        assets = (ctx.checkout / project).parent / "obj" / "project.assets.json"
        if not assets.is_file():
            cf.log("chain: FAIL %s: no obj/project.assets.json (nothing restored)" % project)
            failures += 1
            continue
        libraries = cf._library_versions(json.loads(assets.read_text(encoding="utf-8-sig")))
        for row in rows:
            package_id, want = row["package_id"], row["ws_version"]
            got = libraries.get(package_id)
            if got is None and package_id not in required:
                cf.log("chain: n/a  %s %s not referenced (sibling artifact)" % (project, package_id))
                continue
            ok = got == want
            failures += 0 if ok else 1
            cf.log("chain: %s %s %s expected=%s resolved=%s" % ("OK  " if ok else "FAIL", project, package_id, want, got))
    cf.log("chain: assets identity %s (%d failing)" % ("OK" if not failures else "FAILED", failures))
    return 3 if failures else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip())
    sub = parser.add_subparsers(dest="command", required=True)
    for name, handler, help_text in (
        ("pin-drift", cmd_pin_drift, "export CHAIN_PIN_DRIFT for `record --warn` (never fails)"),
        ("assets-identity", cmd_assets_identity, "prove every listed project resolved the ws packages"),
    ):
        p = sub.add_parser(name, help=help_text)
        p.set_defaults(handler=handler)
        p.add_argument("--job", required=True, help="the workflow job key (same as apply/record)")
        p.add_argument("--edges", action="append", default=[], metavar="SELECTOR",
                       help="the SAME --edges values the job passes to apply and record")
        if name == "assets-identity":
            p.add_argument("--project", action="append", required=True,
                           help="a csproj path relative to the checkout (repeatable)")
    args = parser.parse_args(argv)
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
