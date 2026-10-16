# EnhancedInput Tools

AI tools for Unreal's Enhanced Input system (list/inspect Input Actions & Mapping Contexts, create actions/contexts, add key mappings).

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 0.5.0 or later, installed in the same project.
- Engine plugins: `EnhancedInput`. The installer enables these dependencies.
- A C++ toolchain for your Unreal Engine installation. Extension releases contain source files and must be compiled for your project and engine.

## Install

With the Unreal Editor closed:

```sh
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin "<UEProject>"
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-enhanced-input "<UEProject>"
```

Replace `<UEProject>` with the project directory. The installer selects this extension's latest indexed version independently of the core and other extensions. Use `--version X.Y.Z` to select a published historical version, or `--build` to compile after installation. Otherwise rebuild the project before opening the editor.

The plugin is installed under `Plugins/UnrealAIEnhancedInput/`. Connect Unreal MCP to your AI client to use the tools below.

## Tools

| Tool | Description |
| --- | --- |
| `enhanced-input-list` | List every Enhanced Input asset in the project (Asset Registry; no asset loaded): Input Actions and Mapping Contexts. |
| `enhanced-input-get` | Inspect a single Enhanced Input asset (read-only): a UInputAction's value type, or a UInputMappingContext's key->action mappings. |
| `enhanced-input-action-create` | Create a UInputAction asset (value type Boolean/Axis1D/Axis2D/Axis3D) at a content path. |
| `enhanced-input-context-create` | Create a UInputMappingContext asset at a content path. |
| `enhanced-input-add-mapping` | Add a key -> action mapping to a UInputMappingContext. |

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the provider API.

Licensed under [Apache License 2.0](LICENSE).
