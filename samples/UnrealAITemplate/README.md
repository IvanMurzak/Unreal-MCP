# UnrealAITemplate

A minimal, standalone Unreal Engine plugin demonstrating an **Unreal-MCP extension**: an RQL module
([`IUnrealMcpRqlModule`](../../UnrealMCP/Source/UnrealMcpRql/Public/IUnrealMcpRqlModule.h)) registered through
`IModularFeatures` under the feature name `UnrealMcpRqlModule`. Extensions do not add tools: agents reach the editor
through RQL (`rql-query` / `rql-mutate` / `rql-schema`, [`docs/rql.md`](../../docs/rql.md)), and a module adds what
reflection does not show. It doubles as the working example of the [extension-author guide](../../docs/EXTENSIONS.md).

## What it shows

- Implementing `IUnrealMcpRqlModule` (`GetRqlModuleName` / `GetExtensionId` / `GetExtensionVersion` / `ConfigureRql`);
  the version is the .uplugin VersionName (`UnrealMcpRql::PluginVersion`).
- An operations class (`UUnrealAITemplateRqlApi`): `AddApiSource` makes every UFUNCTION callable with `call` and its
  class default object the `hello` source; failures use `UnrealMcpRql::Failed`.
- A computed member of a narrow class (`AddJsonMember` on static mesh components) and a guide for the `unreal-rql`
  skill (`AddGuide`).
- Registering the module as a modular feature for the editor module's lifetime: `TUnrealMcpRqlEditorModule`.

```
hello | call Greet("Ada")
hello | call Relabel("Cube", "Crate")
actors | get Components | where $ is StaticMeshComponent | where MaterialCount > 1 | select Owner.Label MaterialCount
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
