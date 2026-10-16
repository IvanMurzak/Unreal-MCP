# ControlRig Tools

AI tools for Unreal's Control Rig (list/inspect rig blueprints, controls, bones, and actor components).

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.5.0 or later, installed in the same project.
- Engine plugins: `ControlRig`. The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-control-rig "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAIControlRig/`. Connect Unreal MCP to your AI client to use the tools below.

## Tools

| Tool | Description |
| --- | --- |
| `control-rig-list` | Lists every Control Rig blueprint (UControlRigBlueprint) asset in the project via the Asset Registry, without loading any of them. Optionally filter by a content-path prefix. Returns { count, controlRigs:[{ name, path }] }. |
| `control-rig-get` | Inspects a single Control Rig blueprint asset (read-only) and reports its rig element hierarchy. Returns { path, name, boneCount, nullCount, controlCount, curveCount, elementCount, elements:[{ name, type }] }. |
| `control-rig-list-controls` | Lists the control elements of a Control Rig blueprint (read-only), each with its control type (e.g. Float, Transform, Bool). Returns { path, controlCount, controls:[{ name, controlType }] }. |
| `control-rig-list-bones` | Lists the bone elements of a Control Rig blueprint (read-only), each with its immediate parent bone (empty for a root). Returns { path, boneCount, bones:[{ name, parent }] }. |
| `control-rig-get-component` | Inspects the Control Rig component of an actor in the active editor world (read-only). Looks the actor up by its label. Returns { actorName, componentName, controlRigClass } or a defensive error if the actor or component is not found. |

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the provider API.

Licensed under [Apache License 2.0](LICENSE).
