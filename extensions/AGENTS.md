# extensions/

- Each `UnrealAI<Feature>/` is an optional UE plugin: its own `.uplugin`, module, `extension.json`, tests and license.
- Each contributes an RQL module (`IUnrealMcpRqlModule`, docs/rql.md), not MCP tools: sources, an operations class
  (`Private/UnrealAI<Feature>RqlApi.*`, UFUNCTIONs), computed members and a guide; the plugin README lists them (`extension.json` `tools` stays empty).
- `migration-sources.json` records the original public repo and exact imported commit; preserve provenance.
- New first-party plugins go here, never as nested git submodules. Keep IDs/plugin names stable.
- Source handlers/specs live under each plugin's `Source/`; live checks are declarative `Tests/e2e/tools/*.e2e.json`
  (format: `docs/EXTENSION-MONOREPO.md`, runner `scripts/run_extension_e2e.py`).
- Version source: `.uplugin` `VersionName`; bump with `scripts/extension_release.py bump --plugin ... --version ...`.
- Independent tags/ZIPs and the online index: `docs/EXTENSION-MONOREPO.md`.
- Add test expectations to `tests/automation-inventory.json`; never lower them merely to pass a run.
