<b>[English](RUNTIME.md) | [中文](RUNTIME.zh-CN.md) | [日本語](RUNTIME.ja.md) | [Español](RUNTIME.es.md) | [فارسی](RUNTIME.fa.md) | [हिन्दी](RUNTIME.hi.md) | [العربية](RUNTIME.ar.md)</b>

# 运行时使用（游戏内）

默认情况下，Unreal-MCP 操控的是**编辑器**。它也可以在**正在运行的游戏内部**运行——PIE、Standalone 或打包的 **Development** 构建——让 AI 助手实时操控你的游戏。这是 [Unity-MCP 运行时（游戏内）支持](https://github.com/IvanMurzak/Unity-MCP#runtime-usage-in-game)在 Unreal 中的对应功能：相当于 Unity 的 `UnityMcpPluginRuntime.Initialize().Build().Connect()` 及其 `[AiTool]` 国际象棋机器人示例。

运行时入口是一个 `UGameInstanceSubsystem`：[`UUnrealMcpRuntimeSubsystem`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpRuntimeSubsystem.h)（位于插件的 `UnrealMcpRuntime` **运行时模块**中）。它会为每个 `UGameInstance` 自动实例化一次，但**绝不会自动连接**——连接始终是一次显式的、需主动启用的调用（见下文的[安全约定](#runtime-security-contract)）。

## 三种连接方式

三种方式最终都调用同一个 `UUnrealMcpRuntimeSubsystem::Connect(Host, Token, Mode, bAllowRemoteHost)`；连接模式默认为 **Custom**（由开发者提供的回环服务器）。

**1. 从 C++**（例如你的 `GameMode::BeginPlay`）：

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

`Get(WorldContext)` 是一个静态的 `BlueprintPure` 辅助函数，返回该上下文所属游戏实例的子系统（或 null）。如果任何安全关卡拒绝，`Connect` 会返回 `false`（不建立任何连接）——见下文。

**2. 从 Blueprint** — `Get Unreal MCP Runtime Subsystem`（`Get` 节点，支持 `WorldContext`）→ **`Connect`**（**Unreal MCP** 类别下的 `BlueprintCallable` 节点；`Token` / `Mode` / `bAllowRemoteHost` 是高级引脚）。配合 **`Disconnect`** 节点，以及用于查询状态的 **`Is Connected`** 纯节点使用。

**3. 从控制台**（方便 QA，无需重新编译）— 在子系统存活期间注册：

```
UnrealMcp.Connect <host> [token]
UnrealMcp.Disconnect
```

控制台方式始终使用回环 + Custom 模式。

## 你自己的游戏内工具、提示词和资源

游戏可以提供**自己的**玩法工具（如有需要，还可以提供提示词和资源），由 AI 实时操控——相当于 Unity 的 `WithToolsFromAssembly` / `[AiTool]` 国际象棋机器人。三者的编写方式与编辑器扩展完全相同（[EXTENSIONS.md](EXTENSIONS.md)），对于游戏模块只有两处不同：把它设为 **`Type=Runtime`**，并依赖 **`UnrealMcpRuntime`**（而不是 `UnrealMcpEditor`）。在模块启动时注册你的 provider，或使用子系统中易于发现的包装函数——每种各有一对注册/注销函数：

```cpp
if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
{
    Mcp->RegisterToolProvider(MyToolProvider);          // tools merge in; manifest re-pushed
    Mcp->RegisterPromptProvider(MyPromptProvider);      // prompts merge in; manifest re-pushed
    Mcp->RegisterResourceProvider(MyResourceProvider);  // resources merge in; manifest re-pushed
}
// ... before destroying the providers, call the matching Unregister*Provider(...) for each.
```

完整、可构建的示例是 [`samples/UnrealAIRuntimeSample/`](../samples/UnrealAIRuntimeSample)——一个 `Type=Runtime` 插件，它的 **`game-time-dilation`** 工具读取/设置实时世界的 `AWorldSettings::TimeDilation`（慢动作 / 快进），可在运行中的游戏里通过运行时 MCP 连接调用。（它演示的是工具路径；提示词和资源通过上面展示的同一套三个包装函数 API 注册，随附的核心类别就是可运行的参考。）作者侧的细节（约定、生命周期、顺序、隔离）见 [`docs/EXTENSIONS.md` → Runtime usage](EXTENSIONS.md#runtime-usage-in-game-extensions)。

## 运行时工具集与仅限编辑器的工具

运行时连接只提供**一个**内置工具：**`ping`**（存活探测）。`ping` 是一个[系统工具](TOOLS.zh-CN.md)，因此它在 `POST /api/system-tools/ping` 上应答，不会向 AI 智能体公布——也就是说，一个没有注册任何自有工具的打包游戏会呈现一个**空的**工具列表，这如实反映了它的情况。运行时 AI 智能体能做的一切都需要**自带**：通过上面的扩展总线（`RegisterToolProvider`）注册你自己的工具，它们就会正常出现。

所有引擎开发类别——RQL 数据层（`rql-query` / `rql-mutate` / `rql-schema`，作用于 Actor、组件、资源、关卡、控制台和编辑器状态）、所有截图工具，以及 Blueprint 编写、C++ 源码编辑与编译、`console-run-command`、`unreal-tool-list`、`python-execute` 和 Fab 工具——都**仅限编辑器**（即 [编辑器工具](TOOLS.zh-CN.md)）。它们操控的是编辑器，其中几个属于 RCE 级别（例如 `rql-mutate`、`console-run-command`、`python-execute`），因此默认不会编译进发布的游戏。打包的游戏中没有编辑器，所以运行时的内置能力有意只包括 `ping` + 你的游戏注册的工具。

## <a id="editor-only-exclude-from-packaged-games"></a>仅限编辑器 — 从打包的游戏中排除 Unreal-MCP

如果你只想在 **Unreal Editor 内使用 AI 工具**，并打算发布一个**完全不含 Unreal-MCP** 的打包游戏——没有运行时模块，也没有捆绑的 `.NET` 边车进程二进制文件——请在你的**使用方项目自己的 `.uproject`**（而不是插件的 `.uplugin`）中用 **`TargetDenyList`** 把插件限定在编辑器中：

```jsonc
// <YourProject>.uproject — "Plugins" array
{
    "Name": "UnrealMCP",
    "Enabled": true,
    "TargetDenyList": [ "Game", "Client", "Server" ]
}
```

UE 会遵循插件引用上的 `TargetDenyList` / `TargetAllowList`（`PluginReferenceDescriptor::IsEnabledForTarget`）：插件在 **Editor** 目标中保持启用，但会**从打包的 `Game` / `Client` / `Server` 构建中排除**——因此运行时基础模块和捆绑的 `unreal-mcp-bridge` 边车进程都不会被暂存或编译进你发布的产品。编辑器类型的模块（`UnrealMcpEditor`）无论如何都会从游戏构建中剥离；拒绝列表还会去掉 `UnrealMcpRuntime` 基础模块及其边车进程的 `RuntimeDependencies`（§12.5），实现**零编译占用**。

> **注意 — 不要创建直接的模块依赖。** `TargetDenyList` 只排除插件*自己的*模块。如果你的某个**游戏**模块在其 `*.Build.cs` 的 `PublicDependencyModuleNames` / `PrivateDependencyModuleNames` 中添加了 `UnrealMcpRuntime`（或任何 `UnrealMcp*` 模块），或 `#include` 了某个 `UnrealMcp*` 头文件，这个直接依赖会**覆盖拒绝列表**，UBT 仍会把运行时模块编译进你的游戏。纯粹仅限编辑器的项目不引用插件中的任何内容，因此天然保持干净——只有在你确实需要[运行时（游戏内）使用](#运行时使用游戏内)时，才添加模块依赖。

如果要**运行时（游戏内）**使用，则**不要**使用拒绝列表：`UnrealMcpRuntime` 基础模块 + 内置的 `ping` 会进入打包构建（§12.5），你再通过 `IUnrealMcpToolProvider` 注册自己的玩法工具——见[三种连接方式](#三种连接方式)和 [EXTENSIONS.md](EXTENSIONS.md)。两者互斥：仅限编辑器的项目使用拒绝列表，承载实时 MCP 连接的游戏不使用拒绝列表。

## <a id="runtime-security-contract"></a>安全约定

运行时连接就是**对正在运行的游戏的远程控制**（生成 Actor、写入任意反射属性、通过 `console-run-command` 设置任意 CVar、调用 `UFunction`）——如果在发布的产品中可以访问，就属于 RCE 级别。因此运行时能力由**五层缓解措施**锁定（[`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §12.8），全部在 `Connect()` 内部强制执行：

1. **仅限主动启用。** 子系统会自动实例化，但**绝不会自动连接**——加载时不会自动拨号，也没有基于配置资源的自动连接。连接只会通过一次显式的 `Connect()` 调用发生。
2. **总开关，默认关闭。** `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled`（Project Settings → Plugins → **Unreal MCP (Runtime)**）默认为 **`false`**。关闭时，所有 `Connect()` 都会被拒绝，也不会启动边车进程。它是一个 **Game** 配置项（`DefaultGame.ini`，节 `[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]`），因此会随打包构建一起带走，在那里关卡必须生效。打开它**不会**自动连接——仍需显式调用 `Connect()`。
3. **Shipping 关卡。** `*.Build.cs` 中的 `bUnrealMcpAllowShipping` 标志默认为 **`false`**（→ `UNREAL_MCP_ALLOW_SHIPPING=0`）；在 **Shipping** 构建中，除非有意把该标志编译进去，否则 `Connect()` 会记录日志并返回 `false`。Development / PIE 构建不受影响。
4. **默认仅限回环主机。** 除非调用方显式传入 `bAllowRemoteHost = true`，否则 `Connect()` 会拒绝任何非回环的 `Host`。默认设置让连接保持在 `localhost` / `127.0.0.0/8` / `::1` 上。
5. **回环 IPC + 一次性 stdin 令牌。** 插件 ⇄ 边车进程的 IPC 仅限回环，并使用一次性令牌进行认证，该令牌通过 **stdin 而非 argv** 传递，且绝不记录到日志（与编辑器的模型相同，ARCHITECTURE §1.4）。

此外，运行时路径**仅限桌面平台**（Win64 / Mac / Linux）：在主机或移动平台上无法启动 .NET `unreal-mcp-bridge` 边车进程，因此这些平台完全没有运行时 MCP 能力。

> **端到端操作手册。** 完整的操作演练——打开总开关、从 PIE 和打包的 Development 构建连接、通过实时连接调用工具——见 [`docs/RUNTIME-E2E.md`](RUNTIME-E2E.md)。其中确定性的部分（安全关卡、世界解析器切换、连接/断开/孤儿安全）由无界面的 Automation 规格锁定。
