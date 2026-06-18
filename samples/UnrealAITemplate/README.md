# UnrealAITemplate

A minimal, standalone Unreal Engine plugin demonstrating the **Unreal-MCP extensions contract**
([`IUnrealMcpToolProvider`](../../UnrealMCP/Source/UnrealMcpEditor/Public/IUnrealMcpToolProvider.h)),
registered through `IModularFeatures` under the feature name `UnrealMcpToolProvider`. It contributes a
single `hello-extension` tool, proving the contract end-to-end, and doubles as the working example in
the [extension-author guide](../../docs/EXTENSIONS.md) and the error-isolation test fixture.

## What it shows

- Implementing `IUnrealMcpToolProvider` (`GetExtensionId` / `GetDisplayName` / `GetExtensionVersion` /
  `RegisterTools`).
- Declaring a tool with the fluent `FUnrealMcpToolRegistry` builder.
- Registering / unregistering the provider as a modular feature in the module's
  `StartupModule` / `ShutdownModule`.

## Layout

```
UnrealAITemplate.uplugin                              # plugin descriptor (depends on the UnrealMCP plugin)
Source/UnrealAITemplate/UnrealAITemplate.Build.cs     # module rules (depends on UnrealMcpEditor)
Source/UnrealAITemplate/Private/UnrealAITemplateModule.cpp   # provider + module
```

## Using it

1. Copy or symlink this folder into a UE project's `Plugins/` directory **alongside the `UnrealMCP`
   plugin** (the sample's module depends on `UnrealMcpEditor`).
2. Enable both plugins and build the editor target.
3. With an MCP client connected through the sidecar, `hello-extension` appears in the tool list and
   returns a greeting.

## Isolation fixture

Define `UNREAL_AI_TEMPLATE_INVALID_SCHEMA=1` (e.g. in the module's `Build.cs` via
`PublicDefinitions.Add("UNREAL_AI_TEMPLATE_INVALID_SCHEMA=1")`) to make the provider additionally emit
an **invalid** tool (an empty-named parameter — a malformed schema). Unreal-MCP drops only the invalid
entry and records the failure on this extension's record; `hello-extension` stays registered. This is
the §5 descriptor-validation isolation guarantee in action.

## Prompts & resources

This sample focuses on the **tool** contract. Prompts and resources use the exact sibling contracts —
`IUnrealMcpPromptProvider` + `FUnrealMcpPromptRegistry` and `IUnrealMcpResourceProvider` +
`FUnrealMcpResourceRegistry` (same `UnrealMcpRuntime` module, same modular-feature registration, same
isolation rules). For runnable prompt/resource examples, see the shipped **core** families
[`UnrealMcpCorePrompts.cpp`](../../UnrealMCP/Source/UnrealMcpRuntime/Private/Prompts/UnrealMcpCorePrompts.cpp)
(`level-design-brief`) and
[`UnrealMcpCoreResources.cpp`](../../UnrealMCP/Source/UnrealMcpRuntime/Private/Resources/UnrealMcpCoreResources.cpp)
(`unreal://project/levels` + `unreal://project/icon`), and the authoring sections in
[`docs/EXTENSIONS.md`](../../docs/EXTENSIONS.md).

See [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §5 + §A and [`docs/EXTENSIONS.md`](../../docs/EXTENSIONS.md).
