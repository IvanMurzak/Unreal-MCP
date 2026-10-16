# PCG Tools

AI tools for Unreal's Procedural Content Generation (PCG) framework (list/inspect graphs, add & inspect PCG components, trigger generation).

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.5.0 or later, installed in the same project.
- Engine plugins: `PCG`. The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-pcg "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAIPCG/`. Connect Unreal MCP to your AI client to use the tools below.

## Tools

| Tool | Description |
| --- | --- |
| `pcg-list-graphs` | Lists every PCG graph (UPCGGraph) asset in the project via the Asset Registry, without loading any of them. Optionally filter by a content-path prefix. |
| `pcg-get-graph` | Inspects a single PCG graph asset (read-only) and reports its nodes. |
| `pcg-add-component` | Adds a UPCGComponent to a named actor in the active editor world, optionally bound to a PCG graph asset. |
| `pcg-get-component` | Inspects the UPCGComponent on a named actor in the active editor world (read-only): its bound graph and generation state. |
| `pcg-generate` | Triggers PCG generation (refresh) on the UPCGComponent of a named actor in the active editor world. |

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the provider API.

Licensed under [Apache License 2.0](LICENSE).
