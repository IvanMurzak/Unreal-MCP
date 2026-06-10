# unreal-cli

Cross-platform CLI tool for [Unreal-MCP](https://github.com/IvanMurzak/Unreal-MCP) — the Unreal
analog of [`unity-mcp-cli`](https://github.com/IvanMurzak/Unity-MCP/tree/main/cli) and
[`godot-cli`](https://github.com/IvanMurzak/Godot-MCP/tree/main/cli).

> **Status: pre-alpha scaffold.** The package is `private: true` until the publish gate.
> Only the `status` stub command exists today.

## Planned command set (docs/ARCHITECTURE.md §9.1)

`create-project`, `open`, `close`, `install-plugin`, `remove-plugin`, `configure`
(`UNREAL_MCP_*` env/.env), `setup-mcp`, `login` (device code), `status`, `wait-for-ready`,
`run-tool`, `run-system-tool`, `bootstrap-local`, `update`, `install-engine`
(`LauncherInstalled.dat` detection at minimum), `setup-skills`.

It also exports a side-effect-free library (`import { getStatus } from 'unreal-cli'`) whose
functions return a `{ kind: 'success' | 'failure' }` discriminated union and never throw past
the public boundary — same contract as the Unity/Godot CLIs.

## Develop

```bash
npm install
npm run build   # tsc -> dist/ (ESM)
npm test        # vitest
node bin/unreal-cli.js status
```
