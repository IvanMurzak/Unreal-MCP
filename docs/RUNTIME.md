# Runtime usage (in-game)

By default Unreal-MCP drives the **editor**. It can also run **inside a running game** — PIE, Standalone, or a packaged **Development** build — so an AI assistant can drive your game live. This is the Unreal counterpart of [Unity-MCP's runtime (in-game) support](https://github.com/IvanMurzak/Unity-MCP#runtime-usage-in-game): the UE analog of Unity's `UnityMcpPluginRuntime.Initialize().Build().Connect()` and its `[AiTool]` Chess-bot sample.

The runtime entry point is a `UGameInstanceSubsystem`, [`UUnrealMcpRuntimeSubsystem`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpRuntimeSubsystem.h) (in the plugin's `UnrealMcpRuntime` **runtime module**). It is auto-instantiated once per `UGameInstance` but **never auto-connects** — a connection is always an explicit, opt-in call (see [the security contract](#runtime-security-contract) below).

## Connect three ways

All three reach the same `UUnrealMcpRuntimeSubsystem::Connect(Host, Token, Mode, bAllowRemoteHost)`; the connection mode defaults to **Custom** (a developer-supplied loopback server).

**1. From C++** (e.g. your `GameMode::BeginPlay`):

```cpp
#include "UnrealMcpRuntimeSubsystem.h"

void AMyGameMode::BeginPlay()
{
    Super::BeginPlay();

    if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
        Mcp->Connect(TEXT("http://localhost:8080"), TEXT("my-token")); // Custom mode, loopback
    // ... and, when you are done:
    //  Mcp->Disconnect();
}
```

`Get(WorldContext)` is a static `BlueprintPure` helper that returns the subsystem for the context's game instance (or null). `Connect` returns `false` (and connects nothing) if any security gate rejects — see below.

**2. From Blueprint** — `Get Unreal MCP Runtime Subsystem` (the `Get` node, `WorldContext`-aware) → **`Connect`** (a `BlueprintCallable` node under the **Unreal MCP** category; `Token` / `Mode` / `bAllowRemoteHost` are advanced pins). Pair it with the **`Disconnect`** node and the **`Is Connected`** pure node for status.

**3. From the console** (QA convenience, no recompile) — registered while the subsystem is alive:

```
UnrealMcp.Connect <host> [token]
UnrealMcp.Disconnect
```

The console path always uses loopback + Custom mode.

## Your own in-game tools, prompts & resources

A game ships its **own** gameplay tools (and, if useful, prompts and resources) and the AI drives them live — the UE analog of Unity's `WithToolsFromAssembly` / `[AiTool]` Chess-bot. You author all three exactly as for an editor extension ([EXTENSIONS.md](EXTENSIONS.md)), with two changes for a game module: make it **`Type=Runtime`** and depend on **`UnrealMcpRuntime`** (not `UnrealMcpEditor`). Register your providers at module startup, or use the subsystem's discoverable wrappers — one register/unregister pair per kind:

```cpp
if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
{
    Mcp->RegisterToolProvider(MyToolProvider);          // tools merge in; manifest re-pushed
    Mcp->RegisterPromptProvider(MyPromptProvider);      // prompts merge in; manifest re-pushed
    Mcp->RegisterResourceProvider(MyResourceProvider);  // resources merge in; manifest re-pushed
}
// ... before destroying the providers, call the matching Unregister*Provider(...) for each.
```

The complete, buildable example is [`samples/UnrealAIRuntimeSample/`](../samples/UnrealAIRuntimeSample) — a `Type=Runtime` plugin whose **`game-time-dilation`** tool reads/sets the live world's `AWorldSettings::TimeDilation` (slow-motion / fast-forward), callable in a running game over a runtime MCP connection. (It demonstrates the tool path; prompts and resources register through the same three-wrapper API shown above and the shipped core families are the runnable reference.) The author-side details (the contract, lifecycle, ordering, isolation) are in [`docs/EXTENSIONS.md` → Runtime usage](EXTENSIONS.md#runtime-usage-in-game-extensions).

## Runtime tool set vs editor-only

A runtime connection ships exactly **one** built-in tool: **`ping`** (a liveness probe). `ping` is a [system tool](TOOLS.md), so it answers at `POST /api/system-tools/ping` and is not advertised to an AI agent — meaning a packaged game that registers nothing of its own presents an **empty** tool list, which is an honest description of it. Everything a runtime AI agent can do is **bring-your-own**: register your own tools via the extension bus above (`RegisterToolProvider`) and they appear normally.

All of the engine-development families — the RQL data layer (`rql-query` / `rql-mutate` / `rql-schema` over actors, components, assets, levels, the console and editor state), every screenshot tool, plus Blueprint authoring, C++ source edit & compile, `console-run-command`, `unreal-tool-list`, `python-execute` and the Fab tools — are **editor-only** (the [30 editor tools](TOOLS.md)). They drive the editor and several are RCE-class (e.g. `rql-mutate`, `console-run-command`, `python-execute`), so they are not compiled into a shipped game by default. There is no editor in a packaged game, so the runtime built-in surface is intentionally just `ping` + whatever tools your game registers.

## <a id="editor-only-exclude-from-packaged-games"></a>Editor-only — exclude Unreal-MCP from packaged games

If you only want Unreal-MCP for **AI tooling inside the Unreal Editor** and intend to ship a packaged game with **zero Unreal-MCP footprint** — no runtime module, no bundled `.NET` sidecar binary — pin the plugin to the editor with a **`TargetDenyList`** in your **consumer project's own `.uproject`** (not the plugin's `.uplugin`):

```jsonc
// <YourProject>.uproject — "Plugins" array
{
    "Name": "UnrealMCP",
    "Enabled": true,
    "TargetDenyList": [ "Game", "Client", "Server" ]
}
```

UE honours `TargetDenyList` / `TargetAllowList` on a plugin reference (`PluginReferenceDescriptor::IsEnabledForTarget`): the plugin stays active in the **Editor** target, but is **excluded from packaged `Game` / `Client` / `Server` builds** — so neither the runtime infra module nor the bundled `unreal-mcp-bridge` sidecar is staged or compiled into your shipped product. Editor-type modules (`UnrealMcpEditor`) are stripped from a game build regardless; the deny-list also drops the `UnrealMcpRuntime` infra module and its sidecar `RuntimeDependencies` (§12.5), giving you a **zero compiled footprint**.

> **Caveat — do not create a direct module dependency.** A `TargetDenyList` only excludes the plugin's *own* modules. If one of your **game** modules adds `UnrealMcpRuntime` (or any `UnrealMcp*` module) to its `*.Build.cs` `PublicDependencyModuleNames` / `PrivateDependencyModuleNames`, or `#include`s an `UnrealMcp*` header, that direct dependency **overrides the deny-list** and UBT compiles the runtime module into your game anyway. A pure editor-only project references nothing from the plugin, so it stays clean by construction — only opt into a module dependency when you actually want [runtime (in-game) usage](#runtime-usage-in-game).

For **runtime (in-game)** usage instead, leave the deny-list **off**: the `UnrealMcpRuntime` infra module + the `ping` built-in ship into the packaged build (§12.5), and you register your own gameplay tools via `IUnrealMcpToolProvider` — see [Connect three ways](#connect-three-ways) and [EXTENSIONS.md](EXTENSIONS.md). The two are mutually exclusive: deny-list for an editor-only project, no deny-list for a game that hosts a live MCP connection.

## <a id="runtime-security-contract"></a>Security contract

A runtime connection is **remote control of a running game** (spawning actors, writing any reflected property, arbitrary CVars via `console-run-command`, calling `UFunction`s) — RCE-class if it were reachable in a shipped product. The runtime surface is therefore locked down by **five layered mitigations** ([`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §12.8), all enforced inside `Connect()`:

1. **Opt-in only.** The subsystem auto-instantiates but **never auto-connects** — there is no auto-dial on load and no config-asset auto-connect. A connection only ever happens through an explicit `Connect()` call.
2. **Kill switch, default OFF.** `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled` (Project Settings → Plugins → **Unreal MCP (Runtime)**) defaults to **`false`**. While it is off, every `Connect()` is rejected and no sidecar is spawned. It is a **Game** config setting (`DefaultGame.ini`, section `[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]`), so it travels into the packaged build where the gate must take effect. Turning it on does **not** auto-connect — an explicit `Connect()` is still required.
3. **Shipping gate.** The `bUnrealMcpAllowShipping` `*.Build.cs` flag defaults to **`false`** (→ `UNREAL_MCP_ALLOW_SHIPPING=0`); in a **Shipping** build `Connect()` logs and returns `false` unless that flag was deliberately compiled in. Development / PIE builds are unaffected.
4. **Loopback-host default.** `Connect()` rejects any non-loopback `Host` unless the caller explicitly passes `bAllowRemoteHost = true`. The default keeps the connection on `localhost` / `127.0.0.0/8` / `::1`.
5. **Loopback IPC + one-shot stdin token.** The plugin ⇄ sidecar IPC is loopback-only and authenticated with a one-shot token delivered over **stdin, never argv**, and never logged (same model as the editor, ARCHITECTURE §1.4).

Additionally, the runtime path is **Desktop-only** (Win64 / Mac / Linux): spawning the .NET `unreal-mcp-bridge` sidecar process is not possible on console or mobile platforms, so those platforms have no runtime MCP surface at all.

> **End-to-end runbook.** The full operator walkthrough — enabling the kill switch, connecting from PIE and a packaged Development build, and exercising tools over the live connection — is in [`docs/RUNTIME-E2E.md`](RUNTIME-E2E.md). The deterministic half (security gates, world-resolver switching, connect/disconnect/orphan-safety) is locked by headless Automation specs.
