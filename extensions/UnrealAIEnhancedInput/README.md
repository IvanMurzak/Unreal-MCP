# EnhancedInput RQL module

RQL module for Unreal's Enhanced Input system: query Input Actions and Mapping Contexts with their key mappings, create actions and contexts, map keys (with modifiers such as Negate and Swizzle).

An optional editor plugin in the [Unreal MCP monorepository](../../README.md), with its own module, version and release archive.

## Requirements

- Unreal Engine 5.7 or later (the shared extension suite verifies UE 5.7 and 5.8).
- Unreal MCP 1.0.0 or later (the RQL data layer), installed in the same project.
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

The plugin is installed under `Plugins/UnrealAIEnhancedInput/`. Connect Unreal MCP to your AI client to use the RQL module below.

## RQL module

The extension adds its feature to the editor's RQL data layer (`rql-query` / `rql-mutate` / `rql-schema`, [docs/rql.md](../../docs/rql.md)) instead of separate tools; its guide is part of the `unreal-rql` skill.

| Kind | Names |
| --- | --- |
| Sources | `inputActions`, `inputContexts`, `enhancedInput` |
| Operations (`call`) | `CreateAction`, `CreateContext`, `AddMapping`, `RemoveMapping`, `AddActionEvent`, `AddMappingContextOnBeginPlay` |
| Members | `InputMappingContext.KeyMappings` |

```
inputContexts | select Name KeyMappings
enhancedInput | call CreateAction("/Game/Input/IA_Jump", "Boolean")
enhancedInput | call AddMapping("/Game/Input/IMC_Default", "/Game/Input/IA_Jump", "SpaceBar")
[["W", "Swizzle"], ["S", "Swizzle, Negate"], ["A", "Negate"], ["D", ""]] | call enhancedInput.AddMapping("/Game/Input/IMC_Default", "/Game/Input/IA_Move", $[0], $[1])
enhancedInput | call AddMappingContextOnBeginPlay("/Game/BP_Hero", "/Game/Input/IMC_Default")
enhancedInput | call AddActionEvent("/Game/BP_Hero", "/Game/Input/IA_Jump")      wire its Triggered pin with ref(node) | call Connect(...)
```

`AddActionEvent` places the Blueprint editor's Enhanced Input action event (Triggered, Started, Ongoing, Canceled,
Completed, ActionValue) in an actor Blueprint's event graph; `AddMappingContextOnBeginPlay` adds Event BeginPlay -> Get
Player Controller -> Enhanced Input Local Player Subsystem -> Add Mapping Context ahead of what BeginPlay already runs.
The event node comes from the EnhancedInput plugin's `InputBlueprintNodes` module, a dependency of this extension only.

## Releases and development

Each extension has its own versioned source ZIP in [Unreal MCP Releases](https://github.com/IvanMurzak/Unreal-MCP/releases). The shared release index records the latest version, historical releases and archive checksums; unchanged extensions are not copied into other component releases.

See the [monorepository and test guide](../../docs/EXTENSION-MONOREPO.md) for independent publication, isolated compilation and the combined one-editor test suite. See [extension development](../../docs/EXTENSIONS.md) for the RQL module API.

Licensed under [Apache License 2.0](LICENSE).
