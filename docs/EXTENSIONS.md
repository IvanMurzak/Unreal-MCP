# Writing an Unreal-MCP Extension

Unreal-MCP exposes the Unreal Editor to MCP-aware AI assistants as a set of **tools**, **prompts**, and
**resources**. Any third-party UE plugin can contribute its own through a small, public,
modular-feature-based contract — no fork, no link-time coupling, no load-order assumptions. This guide is the
authoritative author reference. The tool design rationale lives in [`docs/ARCHITECTURE.md`](ARCHITECTURE.md)
§5; the prompt/resource design is in §A.

The tool contract is documented first and in full; **prompts** and **resources** follow the exact same
shape (provider interface + fluent registry builder + modular-feature registration) and are documented in
their own sections below — read the tool section first, then the deltas.

A complete, buildable example accompanies this guide:
[`samples/UnrealAITemplate/`](../samples/UnrealAITemplate).

---

## The contract

Implement a single interface —
[`IUnrealMcpToolProvider`](../UnrealMCP/Source/UnrealMcpRuntime/Public/IUnrealMcpToolProvider.h), the
**only Unreal-MCP-specific contract header** (to declare tools you also include `UnrealMcpToolRegistry.h`,
and to register the provider you include `Features/IModularFeatures.h` — see Steps 2–3 below). Both public
headers live in the plugin's **`UnrealMcpRuntime`** module (re-exported by `UnrealMcpEditor`), so the same
contract serves both editor and [runtime (in-game)](#runtime-usage-in-game-extensions) extensions:

```cpp
class IUnrealMcpToolProvider : public IModularFeature
{
public:
    static FName GetModularFeatureName();          // FName("UnrealMcpToolProvider")
    virtual FString GetExtensionId() const = 0;    // stable, unique, reverse-DNS — e.g. "com.foo.niagara-ai"
    virtual FText   GetDisplayName() const = 0;    // shown in the extensions UI
    virtual FString GetExtensionVersion() const = 0;
    virtual void    RegisterTools(FUnrealMcpToolRegistry& Registry) = 0;
};
```

| Member | Purpose |
| --- | --- |
| `GetExtensionId()` | Stable, **unique** identifier. Used as the deterministic sort key and stamped onto every tool the extension contributes (the manifest's `extensionId`). Reverse-DNS is recommended. |
| `GetDisplayName()` | Human-readable name for the extensions UI row. |
| `GetExtensionVersion()` | Free-form version string, independent of the Unreal-MCP plugin version. |
| `RegisterTools(Registry)` | Declare your tools into the registry (see below). Called on the game thread. |

---

## Step 1 — depend on the UnrealMCP plugin

In your module's `*.Build.cs`:

```csharp
PublicDependencyModuleNames.AddRange(new string[] { "Core" });

PrivateDependencyModuleNames.AddRange(new string[]
{
    "CoreUObject", "Engine", "Projects",
    "Json",            // the registry header includes Dom/JsonObject.h
    "UnrealMcpEditor", // the extension contract + tool registry
});
```

In your `*.uplugin`, declare the dependency so load order resolves:

```json
"Plugins": [ { "Name": "UnrealMCP", "Enabled": true } ]
```

## Step 2 — implement the provider

```cpp
#include "IUnrealMcpToolProvider.h"
#include "UnrealMcpToolRegistry.h"

class FMyExtensionProvider : public IUnrealMcpToolProvider
{
public:
    virtual FString GetExtensionId() const override      { return TEXT("com.foo.my-extension"); }
    virtual FText   GetDisplayName() const override      { return NSLOCTEXT("Foo", "Name", "My Extension"); }
    virtual FString GetExtensionVersion() const override { return TEXT("1.0.0"); }

    virtual void RegisterTools(FUnrealMcpToolRegistry& Registry) override
    {
        Registry.Tool(TEXT("hello-extension"))
            .Title(TEXT("Hello Extension"))
            .Description(TEXT("Returns a friendly greeting, optionally addressed to 'name'."))
            .ParamString(TEXT("name"), TEXT("Who to greet. Defaults to 'world'."))
            .ReadOnlyHint(true)
            .IdempotentHint(true)
            .Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpToolResult
            {
                const FString Name = Call.Has(TEXT("name")) ? Call.GetString(TEXT("name")) : TEXT("world");
                TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
                Structured->SetStringField(TEXT("greeting"), FString::Printf(TEXT("Hello, %s!"), *Name));
                return FUnrealMcpToolResult::Success(Structured->GetStringField(TEXT("greeting")), Structured);
            });
    }
};
```

### The tool builder

`Registry.Tool("tool-id")` returns a fluent builder. Available calls:

- `.Title(...)`, `.Description(...)` — human-/LLM-facing copy.
- `.ParamString / .ParamInt / .ParamNumber / .ParamBool / .ParamVector(name, desc, requirement)` —
  declare an input parameter. Pass `EUnrealMcpParamRequirement::Required` to mark it required.
- `.ReadOnlyHint / .DestructiveHint / .IdempotentHint / .OpenWorldHint(bool)` — MCP tool hints.
- `.Handle(lambda)` — bind the handler and commit the tool. **The handler runs on the game thread**
  (the dispatcher guarantees this, §4), so you may call editor/`UObject` APIs directly. Do not block
  on bridge state or pump modal UI.

> Do **not** call `.ExtensionId(...)` from an extension — Unreal-MCP stamps your `GetExtensionId()`
> onto every tool automatically.

Tool ids must be **kebab-case** (`^[a-z0-9]+(-[a-z0-9]+)*$`), matching the Unity/Godot convention.

## Step 3 — register the provider as a modular feature

```cpp
class FMyExtensionModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        Provider = MakeUnique<FMyExtensionProvider>();
        IModularFeatures::Get().RegisterModularFeature(
            IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
    }
    virtual void ShutdownModule() override
    {
        if (Provider.IsValid())
        {
            IModularFeatures::Get().UnregisterModularFeature(
                IUnrealMcpToolProvider::GetModularFeatureName(), Provider.Get());
            Provider.Reset();
        }
    }
private:
    TUniquePtr<FMyExtensionProvider> Provider;
};
IMPLEMENT_MODULE(FMyExtensionModule, MyExtension)
```

That is the whole contract. Keep the provider instance alive for as long as it is registered, and
always unregister in `ShutdownModule`.

---

## Lifecycle

- **Discovery on boot.** Unreal-MCP enumerates every registered `UnrealMcpToolProvider` modular feature
  and merges their tools before the sidecar's first handshake.
- **Late load / hot unload.** Unreal-MCP subscribes to `OnModularFeatureRegistered` /
  `OnModularFeatureUnregistered`. Registering or unregistering your feature at any time triggers a
  **registry rebuild** and a **manifest revision bump**; the sidecar diffs the new manifest and
  adds/removes the affected MCP tools automatically (§2.2). You do not push anything yourself.

## Ordering

- Providers are registered in **ascending `GetExtensionId()` order** (deterministic across runs).
- Within one provider, tools register in **declaration order**.

## Isolation semantics (important)

UE is built without C++ exceptions, so isolation is **descriptor-level**, not body-level:

- Each tool you declare is validated: a valid kebab-case **name**, a well-formed **schema** (every
  parameter has a non-empty name and a known type; no duplicate parameter names), and a **bound
  handler**.
- An **invalid** tool entry is **dropped** and the reason is recorded on your extension's record. Your
  other valid tools, and every other extension, are unaffected.
- A **duplicate tool name** across providers is **rejected** for the later-sorted provider (first-wins
  by the `ExtensionId` sort), again recorded on that extension's record.
- A **duplicate `GetExtensionId()`** across two providers is an authoring error: the first-registered
  provider keeps the id and the later one contributes **no tools**, with the conflict recorded on its
  record. An **empty** or **reserved (`core`)** id is likewise skipped with a recorded error.
- A crash **inside a tool body** is an editor crash like any other plugin code — that is outside the
  isolation contract. Validate inputs and fail gracefully (`FUnrealMcpToolResult::Error(...)`).

The errors recorded on an extension record surface as an error badge on its UI row (§7).

## Enable / disable

Each extension can be toggled by the user. A **disabled** extension contributes **no tools to the
manifest at all** (they are excluded entirely, not merely hidden). The disabled set is persisted across
editor sessions.

## Versioning

Extensions are additive and version-independent: contributing tools never changes the Unreal-MCP plugin
version, and your `GetExtensionVersion()` is your own.

---

## Custom prompts

Beyond tools, an extension can contribute **MCP prompts** — reusable, parameterized prompt templates the AI
agent can fetch via `prompts/get`. The contract is the exact prompt sibling of the tool contract: implement
[`IUnrealMcpPromptProvider`](../UnrealMCP/Source/UnrealMcpRuntime/Public/IUnrealMcpPromptProvider.h) and
declare prompts with the fluent
[`FUnrealMcpPromptRegistry`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpPromptRegistry.h) builder
(both headers in the `UnrealMcpRuntime` module). The architecture is in
[`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §A.

```cpp
class IUnrealMcpPromptProvider : public IModularFeature
{
public:
    static FName GetModularFeatureName();          // FName("UnrealMcpPromptProvider")
    virtual FString GetExtensionId() const = 0;    // stable, unique, reverse-DNS
    virtual FText   GetDisplayName() const = 0;
    virtual FString GetExtensionVersion() const = 0;
    virtual void    RegisterPrompts(FUnrealMcpPromptRegistry& Registry) = 0;
};
```

### The prompt builder

`Registry.Prompt("prompt-id")` returns a fluent builder. Available calls:

- `.Title(...)`, `.Description(...)` — human-/LLM-facing copy.
- `.Role(EUnrealMcpPromptRole::User | ::Assistant)` — the default author role of the rendered message(s).
- `.ParamString / .ParamInt / .ParamNumber / .ParamBool / .ParamVector(name, desc, requirement)` — declare a
  prompt argument. These are the **same** param helpers and `EUnrealMcpParamRequirement` as the tool builder
  (prompt args reuse the tool schema generation verbatim). `.Param(name, jsonType, desc, req, customSchema)`
  is the generic escape hatch.
- `.Handle(lambda)` — bind the handler and commit the prompt. The handler runs on the **game thread** and
  receives the same `FUnrealMcpToolCall` args surface (`Call.GetString(...)`, `Call.Has(...)`); it returns a
  `FUnrealMcpPromptResult`.

A prompt handler returns role-tagged messages. The common one-shot shape is the static helper:

```cpp
#include "IUnrealMcpPromptProvider.h"
#include "UnrealMcpPromptRegistry.h"

virtual void RegisterPrompts(FUnrealMcpPromptRegistry& Registry) override
{
    Registry.Prompt(TEXT("level-design-brief"))
        .Title(TEXT("Level Design Brief"))
        .Description(TEXT("Generate a level design brief from a single 'theme' argument."))
        .Role(EUnrealMcpPromptRole::User)
        .ParamString(TEXT("theme"), TEXT("The level theme (e.g. 'haunted forest')."),
                     EUnrealMcpParamRequirement::Required)
        .Handle([](const FUnrealMcpToolCall& Call) -> FUnrealMcpPromptResult
        {
            const FString Theme = Call.GetString(TEXT("theme"));
            if (Theme.IsEmpty())
                return FUnrealMcpPromptResult::Error(TEXT("theme is required."));

            const FString Text = FString::Printf(
                TEXT("Draft a level design brief for a \"%s\"-themed level. Cover layout, pacing, "
                     "key encounters, and mood."), *Theme);
            return FUnrealMcpPromptResult::Success(Text, EUnrealMcpPromptRole::User);
        });
}
```

This is the shipped core `level-design-brief` prompt
([`UnrealMCP/Source/UnrealMcpRuntime/Private/Prompts/UnrealMcpCorePrompts.cpp`](../UnrealMCP/Source/UnrealMcpRuntime/Private/Prompts/UnrealMcpCorePrompts.cpp)).
Register the provider as a modular feature exactly as for tools, but under
`IUnrealMcpPromptProvider::GetModularFeatureName()`:

```cpp
IModularFeatures::Get().RegisterModularFeature(
    IUnrealMcpPromptProvider::GetModularFeatureName(), PromptProvider.Get());
// ... and UnregisterModularFeature(...) in ShutdownModule.
```

Prompt ids are **kebab-case**, the same rule as tool ids. Do **not** call `.ExtensionId(...)` yourself — your
`GetExtensionId()` is stamped automatically. Lifecycle, ordering, isolation (descriptor-level: a duplicate
prompt name across providers is rejected first-wins; an invalid descriptor is dropped), and per-extension
enable/disable behave **identically** to the tool path described above.

---

## Custom resources

An extension can also contribute **MCP resources** — addressable, readable content the AI agent fetches via
`resources/read`. Implement
[`IUnrealMcpResourceProvider`](../UnrealMCP/Source/UnrealMcpRuntime/Public/IUnrealMcpResourceProvider.h) and
declare resources with the fluent
[`FUnrealMcpResourceRegistry`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpResourceRegistry.h)
builder.

```cpp
class IUnrealMcpResourceProvider : public IModularFeature
{
public:
    static FName GetModularFeatureName();          // FName("UnrealMcpResourceProvider")
    virtual FString GetExtensionId() const = 0;
    virtual FText   GetDisplayName() const = 0;
    virtual FString GetExtensionVersion() const = 0;
    virtual void    RegisterResources(FUnrealMcpResourceRegistry& Registry) = 0;
};
```

### The resource builder

`Registry.Resource("unreal://your/uri")` returns a fluent builder — the resource's **URI is its identity**
(the registry key and the MCP route). Available calls:

- `.Name(...)`, `.Description(...)` — human-/LLM-facing copy.
- `.MimeType(...)` — e.g. `"application/json"`, `"image/png"`.
- `.Read(lambda)` — bind the read handler and commit the resource. The handler runs on the **game thread**,
  receives the requested `FString Uri`, and returns a `FUnrealMcpResourceResult`.

A read returns one or more content blocks. Each block is **text XOR a base64 blob** + a mime type — use the
static helpers `FUnrealMcpResourceResult::Text(uri, text, mimeType)` or `::Blob(uri, base64, mimeType)` (and
`::MakeError(reason)` on failure):

```cpp
#include "IUnrealMcpResourceProvider.h"
#include "UnrealMcpResourceRegistry.h"

virtual void RegisterResources(FUnrealMcpResourceRegistry& Registry) override
{
    // A JSON (text) resource:
    Registry.Resource(TEXT("unreal://project/levels"))
        .Name(TEXT("Project Levels"))
        .Description(TEXT("A JSON snapshot of the active world and its levels."))
        .MimeType(TEXT("application/json"))
        .Read([](const FString& Uri) -> FUnrealMcpResourceResult
        {
            return FUnrealMcpResourceResult::Text(Uri, BuildLevelsJson(), TEXT("application/json"));
        });

    // A binary (blob) resource — carried as base64, like a screenshot image:
    Registry.Resource(TEXT("unreal://project/icon"))
        .Name(TEXT("Project Icon"))
        .Description(TEXT("A small PNG, returned as a base64 blob."))
        .MimeType(TEXT("image/png"))
        .Read([](const FString& Uri) -> FUnrealMcpResourceResult
        {
            const FString Base64 = FBase64::Encode(IconBytes, sizeof(IconBytes));
            return FUnrealMcpResourceResult::Blob(Uri, Base64, TEXT("image/png"));
        });
}
```

These are the shipped core resources
([`UnrealMCP/Source/UnrealMcpRuntime/Private/Resources/UnrealMcpCoreResources.cpp`](../UnrealMCP/Source/UnrealMcpRuntime/Private/Resources/UnrealMcpCoreResources.cpp)).
Register the provider under `IUnrealMcpResourceProvider::GetModularFeatureName()` (same modular-feature
pattern as tools and prompts). Lifecycle, ordering, isolation (a duplicate **URI** across providers is
rejected first-wins; an invalid descriptor is dropped), and per-extension enable/disable behave identically.

> **Scope note (MVP).** Only **static, fixed-URI** resources are supported today —
> templated / parameterized resource URIs are deferred (`docs/ARCHITECTURE.md` §A.1). Declare one resource per
> concrete URI.

> **Binary-content note.** A blob is base64 on the Unreal side and on the IPC wire (the C++ `Blob(...)`
> helper and the bridge `ProxyResource` round-trip it correctly). A known **upstream** encoding quirk in the
> shared GameDev-MCP-Server / MCP-Plugin-dotnet can re-emit blob bytes on the MCP wire to the client — text
> resources round-trip cleanly end-to-end; if a binary resource arrives mis-encoded at the client, that
> upstream issue is the cause, not the Unreal-MCP plugin or bridge.

---

## Runtime usage (in-game extensions)

Everything above also works **in a running game** (PIE, Standalone, packaged Development) — this is the
primary Unity-parity use case (`docs/ARCHITECTURE.md` §12.9): a game ships its own gameplay tools and an
AI assistant drives them live, the UE analog of Unity's `[AiTool]` Chess-bot example.

The contract is identical — the extension headers (`IUnrealMcpToolProvider.h`, `UnrealMcpToolRegistry.h`)
live in the Unreal-MCP plugin's **runtime module** `UnrealMcpRuntime`, and `IModularFeatures` is
runtime-available — with two differences for a game extension:

- **Make your module `Type=Runtime`** (not `Editor`) in its `.uplugin`, and depend on **`UnrealMcpRuntime`**
  (not `UnrealMcpEditor`) in your `*.Build.cs`, so it compiles into a packaged Game target where the editor
  module is absent.
- **The runtime MCP surface is opt-in and OFF by default** (`docs/ARCHITECTURE.md` §12.8). Your tools only
  become reachable after the game enables the kill switch (Project Settings → Plugins → Unreal MCP (Runtime)
  → `bRuntimeMcpEnabled`) and explicitly connects:
  `UUnrealMcpRuntimeSubsystem::Get(this)->Connect("http://localhost:8080", "my-token")` (or the QA console
  command `UnrealMcp.Connect <host> [token]`).

### Ergonomic registration via the runtime subsystem

Module-startup registration (Step 3) works in a game too, but the runtime subsystem also offers a
discoverable wrapper so you never have to touch `IModularFeatures` directly:

```cpp
#include "UnrealMcpRuntimeSubsystem.h"
#include "IUnrealMcpToolProvider.h"
#include "IUnrealMcpPromptProvider.h"
#include "IUnrealMcpResourceProvider.h"

if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
{
    Mcp->RegisterToolProvider(MyToolProvider);          // tools merge in; manifest re-pushed
    Mcp->RegisterPromptProvider(MyPromptProvider);      // prompts merge in; manifest re-pushed
    Mcp->RegisterResourceProvider(MyResourceProvider);  // resources merge in; manifest re-pushed
}
// ... later, before destroying the providers:
    Mcp->UnregisterToolProvider(MyToolProvider);
    Mcp->UnregisterPromptProvider(MyPromptProvider);
    Mcp->UnregisterResourceProvider(MyResourceProvider);
```

All three kinds feed the same kind-aware extension manager: registering or unregistering at any time rebuilds
the relevant registry and **re-pushes the manifest**, so a connected sidecar's `tools/list` / `prompts/list`
/ `resources/list` gains/loses your contributions automatically. The subsystem does **not** take ownership of
a provider — keep it alive while it is registered (typically a member of your game module or a
`UGameInstanceSubsystem`) and unregister before destroying it. The same provider object may implement more
than one contract (tools + prompts + resources) and be registered under each.

## Reference examples

- [`samples/UnrealAITemplate/`](../samples/UnrealAITemplate) — the **editor** extension sample: a complete,
  buildable `Type=Editor` plugin with a `hello-extension` tool. It also carries a compile-time switch
  (`UNREAL_AI_TEMPLATE_INVALID_SCHEMA=1`) that emits an intentionally invalid tool so you can observe the
  isolation behaviour first-hand.
- [`samples/UnrealAIRuntimeSample/`](../samples/UnrealAIRuntimeSample) — the **runtime (in-game)** extension
  sample: a `Type=Runtime` plugin whose `game-time-dilation` tool reads/sets the live `UWorld`'s
  `AWorldSettings::TimeDilation`, callable in a running game over a runtime MCP connection (§12.9).

---

## Installing an extension into a project

The supported way to install a published (or local) extension into a user's Unreal project is the
`unreal-mcp-cli install-extension` command (and the matching exported library function
`installExtension`, parity with `installPlugin`):

```bash
# From the extension's GitHub release (the default channel):
unreal-mcp-cli install-extension <extensionId> <project> --version <x.y.z>

# From a local copy — offline / CI / dev (mirrors godot-cli install-plugin --source):
unreal-mcp-cli install-extension <extensionId> <project> --source <plugin-dir>

# Compile now instead of on next editor open:
unreal-mcp-cli install-extension <extensionId> <project> --source <plugin-dir> --build
```

What it does, and why:

1. **Resolve** `<extensionId>` against the shared, engine-agnostic **extension catalog** (the typed
   `cli/src/utils/extensions-catalog.ts`, mirror of the future `extensions.catalog.json` —
   `{ schemaVersion, extensions[] }`; each descriptor carries `extensionId`, `pluginName`, `repo`,
   `version`, `minCoreVersion`, `enginePlugins`, `tools`). An unpublished extension installs anyway via
   `--source` (the descriptor is synthesized from the source `.uplugin`).
2. **Place** the plugin into `<project>/Plugins/<pluginName>/` (build-cache / VCS subtrees excluded;
   staged-then-swapped so a failed install never corrupts an existing one).
3. **Enable** the extension **and its gating engine plugins** — e.g. an extension that uses Niagara
   declares `"Plugins": [ { "Name": "Niagara", "Enabled": true } ]` in its own `.uplugin`; the installer
   reads that set (∪ the catalog `enginePlugins` hint) and enables every entry, plus the extension
   itself, in the project's `.uproject` `Plugins[]` array.
4. **Compile.** A Unreal-MCP extension is shipped as **C++ source** and compiled by UnrealBuildTool. The
   default strategy is **source-ship + compile-on-next-editor-open** (`install-extension` reports
   `rebuildRequired: true`); `--build` opts into an eager UBT compile against the resolved engine. We
   ship source rather than precompiled-per-UE-version binaries because UE plugin binaries are tied to an
   exact engine version + build config + platform and are ABI-unstable across engine minors — the same
   reason the core `install-plugin` ships source and excludes the stale `Intermediate/`/`Binaries/` cache.

The install is **idempotent** (a re-run at the same version that is already enabled writes nothing) and
the install-source resolver is **pluggable** — a future **Fab** (Epic marketplace) channel slots in as a
new source `kind` without changing the command or library API. See
[`cli/README.md` § Extensions](../cli/README.md#extensions) for the full option list.
