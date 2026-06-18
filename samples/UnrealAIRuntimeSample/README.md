# UnrealAIRuntimeSample

A minimal, standalone Unreal Engine plugin demonstrating the **Unreal-MCP RUNTIME (in-game) extension
contract** ([`IUnrealMcpToolProvider`](../../UnrealMCP/Source/UnrealMcpRuntime/Public/IUnrealMcpToolProvider.h)),
registered through `IModularFeatures` under the feature name `UnrealMcpToolProvider`. It contributes a
single `game-time-dilation` gameplay tool **callable in a running game** (PIE / Standalone / packaged
Development), proving the §12.9 runtime extension bus end-to-end. It is the runtime counterpart of the
editor-side [`samples/UnrealAITemplate`](../UnrealAITemplate) and the UE analog of Unity's `[AiTool]`
Chess-bot in-game example.

## What it shows

- A **`Type=Runtime`** game module (not editor) implementing `IUnrealMcpToolProvider` — so it compiles
  into a **packaged game** (the editor module is absent in a Game target).
- A tool that drives **real gameplay state on the live `UWorld`**: it reads and optionally sets
  `AWorldSettings::TimeDilation` (slow-motion / fast-forward).
- Registering / unregistering the provider as a modular feature in `StartupModule` / `ShutdownModule`,
  which makes Unreal-MCP rebuild the registry and **re-push the manifest** so a connected sidecar's
  `tools/list` gains/loses the tool at runtime.

## The `game-time-dilation` tool

| Param | Type | Required | Meaning |
| --- | --- | --- | --- |
| `value` | number | no | New global time dilation (> 0). Omit to **read** the current value without changing it. |

The result's structured payload is `{ "timeDilation": <effective value>, "wasSet": <bool>, "world": <name> }`.
When `value` is supplied, `AWorldSettings::SetTimeDilation` clamps it to
`[MinGlobalTimeDilation, MaxGlobalTimeDilation]`, so the **effective** (post-clamp) value is returned —
not the raw request.

## Layout

```
UnrealAIRuntimeSample.uplugin                                       # plugin descriptor (depends on UnrealMCP); module Type=Runtime
Source/UnrealAIRuntimeSample/UnrealAIRuntimeSample.Build.cs         # module rules (depends on UnrealMcpRuntime)
Source/UnrealAIRuntimeSample/Private/UnrealAIRuntimeSampleModule.cpp # provider + module
```

## Using it (in-game)

1. Copy or symlink this folder into a UE project's `Plugins/` directory **alongside the `UnrealMCP`
   plugin** (this sample's module depends on `UnrealMcpRuntime`).
2. Enable both plugins and build the **Game** target (or a packaged Development build).
3. In-game, opt the runtime MCP surface in (it is OFF by default — `docs/ARCHITECTURE.md` §12.8):
   - Enable the kill switch: **Project Settings → Plugins → Unreal MCP (Runtime) →
     `bRuntimeMcpEnabled`**.
   - Connect from `BeginPlay` (or the console): `UUnrealMcpRuntimeSubsystem::Get(this)->Connect("http://localhost:8080", "my-token")`,
     or the QA console command `UnrealMcp.Connect http://localhost:8080 my-token`.
4. With an MCP client connected through the sidecar, `game-time-dilation` appears in `tools/list` and,
   when executed, reads/sets the live world's time dilation.

## Registering a provider at runtime (ergonomic API)

Beyond module-startup registration (what this sample does), a game can register a provider explicitly
once the runtime subsystem is up — no direct `IModularFeatures` call needed:

```cpp
#include "UnrealMcpRuntimeSubsystem.h"
#include "IUnrealMcpToolProvider.h"

if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
    Mcp->RegisterToolProvider(MyProviderInstance);   // ... ->UnregisterToolProvider(MyProviderInstance) to remove

```

Both paths feed the same §5 extension manager, which rebuilds the registry and re-pushes the manifest.
Keep the provider instance alive for as long as it is registered.

See [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §12.9 and
[`docs/EXTENSIONS.md`](../../docs/EXTENSIONS.md).
