# Landscape Tools

AI tools for Unreal's Landscape + Water systems (inspect landscapes, water bodies & zones).

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.5.0 or later, installed in the same project.
- Engine plugins: `Water`. The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-landscape "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAILandscape/`. Connect Unreal MCP to your AI client to use the tools below.

## Tools

| Tool | Description |
| --- | --- |
| `landscape-list-actors` | Lists every Landscape actor / proxy in the active editor world (read-only), reporting class, whether it is the primary ALandscape (vs a streaming proxy), and component count. |
| `landscape-get-actor` | Inspects a single Landscape actor by name (read-only): section layout (componentSizeQuads / subsectionSizeQuads / numSubsections), component count, landscape material, and paint layers. |
| `landscape-list-water-bodies` | Lists every Water body actor in the active editor world (read-only), reporting its type (River / Lake / Ocean / Custom) and spline-point count. |
| `landscape-get-water-body` | Inspects a single Water body by name (read-only): type, spline-point count, water material, and owning water zone. |
| `landscape-list-water-zones` | Lists every Water zone actor in the active editor world (read-only), reporting world location and 2D zone extent. |

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the provider API.

Licensed under [Apache License 2.0](LICENSE).
