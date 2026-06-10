# Writing an Unreal-MCP Extension

Unreal-MCP exposes the Unreal Editor to MCP-aware AI assistants as a set of **tools**. Any third-party
UE plugin can contribute its own tools through a small, public, modular-feature-based contract — no
fork, no link-time coupling, no load-order assumptions. This guide is the authoritative author
reference. The design rationale lives in [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §5.

A complete, buildable example accompanies this guide:
[`samples/UnrealAITemplate/`](../samples/UnrealAITemplate).

---

## The contract

Implement a single interface —
[`IUnrealMcpToolProvider`](../UnrealMCP/Source/UnrealMcpEditor/Public/IUnrealMcpToolProvider.h), the
**only Unreal-MCP-specific contract header** (to declare tools you also include `UnrealMcpToolRegistry.h`,
and to register the provider you include `Features/IModularFeatures.h` — see Steps 2–3 below):

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

## Reference example

[`samples/UnrealAITemplate/`](../samples/UnrealAITemplate) is a complete, buildable plugin implementing
everything above with a `hello-extension` tool. It also carries a compile-time switch
(`UNREAL_AI_TEMPLATE_INVALID_SCHEMA=1`) that emits an intentionally invalid tool so you can observe the
isolation behaviour first-hand.
