# Niagara Tools

AI tools for Unreal's Niagara VFX system (list/inspect systems, spawn components).

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.5.0 or later, installed in the same project.
- Engine plugins: `Niagara`. The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-niagara "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAINiagara/`. Connect Unreal MCP to your AI client to use the tools below.

## Tools

| Tool | Description |
| --- | --- |
| `niagara-list-systems` | List every Niagara system asset in the project (Asset Registry; no asset loaded). |
| `niagara-get-system` | Inspect a single Niagara system (read-only): emitter count + per-emitter name/enabled. |
| `niagara-spawn-component` | Spawn a Niagara component for a system into the editor world at a location. |

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the provider API.

Licensed under [Apache License 2.0](LICENSE).
