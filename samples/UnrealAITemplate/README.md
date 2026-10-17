# UnrealAITemplate

A minimal, standalone Unreal Engine plugin demonstrating an **Unreal-MCP extension**: an RQL module
([`IUnrealMcpRqlModule`](../../UnrealMCP/Source/UnrealMcpRql/Public/IUnrealMcpRqlModule.h)) registered through
`IModularFeatures` under the feature name `UnrealMcpRqlModule`. Extensions do not add tools: agents reach the editor
through RQL (`rql-query` / `rql-mutate` / `rql-schema`, [`docs/rql.md`](../../docs/rql.md)), and a module adds what
reflection does not show. It doubles as the working example of the [extension-author guide](../../docs/EXTENSIONS.md).

## What it shows

- Implementing `IUnrealMcpRqlModule` (`GetRqlModuleName` / `GetExtensionId` / `GetExtensionVersion` / `ConfigureRql`).
- An operations class (`UUnrealAITemplateRqlApi`): every UFUNCTION becomes callable with `call` once the module calls
  `AddApiClass`; its class default object is the `hello` source (`AddObjectSource`).
- A computed member on every actor (`AddJsonMember`) and a guide for the `unreal-rql` skill (`AddGuide`).
- Registering / unregistering the module as a modular feature in `StartupModule` / `ShutdownModule`.

```
hello | call Greet("Ada")
hello | call Relabel("Cube", "Crate")
actors | where TagCount > 0 | select Label Tags TagCount
```

## Layout

```
UnrealAITemplate.uplugin                                       # plugin descriptor (depends on the UnrealMCP plugin)
Source/UnrealAITemplate/UnrealAITemplate.Build.cs              # module rules (depends on UnrealMcpRql)
Source/UnrealAITemplate/Private/UnrealAITemplateModule.cpp     # the RQL module + the editor module
Source/UnrealAITemplate/Private/UnrealAITemplateRqlApi.h/.cpp  # the operations class (UFUNCTIONs)
```

## Using it

1. Copy or symlink this folder into a UE project's `Plugins/` directory **alongside the `UnrealMCP` plugin**.
2. Enable both plugins and build the editor target.
3. With an MCP client connected, `rql-schema` lists the `hello` source and `rql-mutate` runs `hello | call Greet("Ada")`;
   `unreal-skill-generate` writes the guide into the `unreal-rql` skill.

## Custom tools, prompts and resources

A dedicated MCP tool is still possible for what is not editor data (the same contracts the runtime sample uses):
`IUnrealMcpToolProvider` + `FUnrealMcpToolRegistry`, and the sibling `IUnrealMcpPromptProvider` /
`IUnrealMcpResourceProvider` (module `UnrealMcpRuntime`, same modular-feature registration, same isolation rules). See
[`samples/UnrealAIRuntimeSample/`](../UnrealAIRuntimeSample) and [`docs/EXTENSIONS.md`](../../docs/EXTENSIONS.md).
