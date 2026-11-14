<div align="center">

<img src="img/readme/hero.svg" alt="Unreal MCP — 让 Claude、Cursor、Copilot 或任何 MCP 智能体检查并操控 Unreal Editor" width="100%"/>

<br/>

[![Release](https://img.shields.io/github/v/release/IvanMurzak/Unreal-MCP?label=release&color=2F6F9F&logo=github&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/releases) [![Fab](https://img.shields.io/badge/Fab-AI%20Game%20Developer-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) [![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.5%E2%80%935.8-0E7490?logo=unrealengine&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.unrealengine.com/)<br/>[![Release build](https://img.shields.io/github/actions/workflow/status/IvanMurzak/Unreal-MCP/release.yml?label=release%20build&logo=github&color=15803D&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/actions/workflows/release.yml) [![Docs](https://img.shields.io/badge/docs-ai--game.dev-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev) [![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://discord.gg/cfbdMZX99G) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](../LICENSE) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

  <p>
    <a href="https://claude.ai/download"><img src="img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://visualstudio.microsoft.com/"><img src="img/mcp-clients/visual-studio-64.png" alt="Visual Studio" title="Visual Studio" height="36"></a>&nbsp;&nbsp;
    <a href="https://opencode.ai/"><img src="img/mcp-clients/open-code-64.png" alt="OpenCode" title="OpenCode" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/Kilo-Org/kilocode"><img src="img/mcp-clients/kilo-code-64.png" alt="Kilo Code" title="Kilo Code" height="36"></a>
  </p>

  <b>[English](../README.md) | [中文](README.zh-CN.md) | [日本語](README.ja.md) | [Español](README.es.md) | [فارسی](README.fa.md) | [हिन्दी](README.hi.md) | [العربية](README.ar.md)</b>

</div>

**Unreal MCP**（AI Game Developer for Unreal）把任何支持 MCP 的 AI 智能体连接到 **Unreal Editor**。用自然语言提出需求，智能体就会生成 Actor、编辑关卡、创建并编译 Blueprint、编辑并编译 C++、管理资源，并用截图检查自己的成果。它是 [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) 和 [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) 的 Unreal 版姊妹项目，使用同一个云后端（[ai-game.dev](https://ai-game.dev)），也可以使用你自己的 [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server)。

<p align="center">
  <img src="img/readme/architecture.svg" alt="AI 智能体通过 MCP 与服务器通信；服务器通过 SignalR 转发给 unreal-mcp-bridge 边车进程，边车再通过回环 TCP 连接 Unreal Editor 中的 UnrealMCP C++ 插件" width="100%"/>
</p>

Unreal 编辑器是 C++ 编写的，所以 .NET MCP 宿主作为一个小型**边车进程**（`unreal-mcp-bridge`）在它旁边运行，由插件负责启动、监控和重启。设计细节：[docs/ARCHITECTURE.md](ARCHITECTURE.md)。

## 功能

✅ **RQL 数据层** — 用一种语言查询和修改 Actor、资源和关卡（[RQL](rql.md)）\
✅ **Blueprint** — 创建、编辑和编译，返回智能体能据此修复的结构化错误\
✅ **C++** — 生成骨架、编辑并编译项目源码（Live Coding 或 UBT）\
✅ **视觉反馈** — 视口、游戏视图、相机和单独 Actor 的截图\
✅ **Python** — 运行编辑器 Python，可使用完整的 `unreal` API\
✅ **Fab** — 搜索 Fab、列出你的库、添加已拥有的内容\
✅ **扩展** — GAS、Niagara、PCG、Landscape、Control Rig、Enhanced Input\
✅ **自定义工具** — 从任意插件添加工具、提示词和资源（[指南](EXTENSIONS.md)）\
✅ **运行时模式** — 可选启用，操控正在运行的游戏（[docs/RUNTIME.md](RUNTIME.zh-CN.md)）\
✅ **云端或本地** — 开箱即用的 `ai-game.dev`，或自托管的 `gamedev-mcp-server`

共 8 个类别、30 个内置工具；完整列表见 [docs/TOOLS.md](TOOLS.zh-CN.md)。

## 系统要求

- **Unreal Engine 5.5 – 5.8**（发布流水线会逐一验证）以及一个 **C++ 项目**：插件会在你的项目中编译编辑器模块。
- `unreal-mcp-cli` 需要 **Node.js 22.14+**。
- Windows、macOS 或 Linux。无需安装 .NET：边车进程随插件一起提供。（只有从源码构建 bridge 时才需要 .NET 9 SDK。）

## 快速开始

<p align="center">
  <img src="img/readme/terminal.svg" alt="终端：npm install -g unreal-mcp-cli、install-plugin、login、setup-mcp claude-code，然后打开编辑器" width="100%"/>
</p>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` 会下载与 CLI 版本对应的已签名插件源码发布包。在编辑器中打开 **Tools → AI Game Developer**，即可查看连接、bridge 状态和已连接的智能体。然后对你的智能体说：

> *“在一排放三个立方体，在它们上方放一盏点光源，然后截一张图。”*

其他安装方式：

- **Fab / Epic Marketplace**：[Fab 上的 AI Game Developer](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)。Fab 版本不能捆绑可执行文件，因此在那里由窗口中的 **Get bridge**（获取 bridge）按钮引导你完成一次边车进程的安装。
- **手动安装**：把 [`UnrealMCP/`](../UnrealMCP/) 复制到 `<YourProject>/Plugins/UnrealMCP/`。源码检出不包含捆绑的边车进程：运行 `unreal-mcp-cli bootstrap-local` 或设置 `UNREAL_MCP_BRIDGE_PATH`。

更新（`unreal-mcp-cli update`）、连接模式、环境变量和故障排查：[docs/CONFIGURATION.md](CONFIGURATION.zh-CN.md)。

## 使用

大多数时候你只需要和智能体对话。CLI 也能直接调用同样的工具，没有正在运行的编辑器时会启动一个无界面编辑器：

```bash
# Query the level with RQL
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'

# Change it: set / call goes to rql-mutate (add --dry-run to preview)
unreal-mcp-cli rql -p ./MyGame 'editor | call SpawnActor("PointLight")'

# Run editor Python
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"

# List every tool with its inputs, build C++ without the editor, run Automation tests
unreal-mcp-cli tools -p ./MyGame
unreal-mcp-cli compile ./MyGame
unreal-mcp-cli test ./MyGame
```

全部命令和选项：[cli/README.md](../cli/docs/README.zh-CN.md) 或 `unreal-mcp-cli commands`。

## 扩展

扩展是可选插件，用于扩展 RQL；它们本身不添加任何工具。

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

| 扩展 | 新增 | 依赖 |
| --- | --- | --- |
| [AI&nbsp;ControlRig](../extensions/UnrealAIControlRig/README.md) | 程序化动画绑定及其 Anim Blueprint 连接 | `ControlRig`, `RigVM`, UE 5.7+ |
| [AI&nbsp;EnhancedInput](../extensions/UnrealAIEnhancedInput/README.md) | 输入操作、映射上下文、触发器和修饰器 | `EnhancedInput`, UE 5.7+ |
| [AI&nbsp;GAS](../extensions/UnrealAIGAS/README.md) | Gameplay Effect、能力 Blueprint 和属性集 | `GameplayAbilities`, `EnhancedInput`, `GameplayTagsEditor`, UE 5.7+ |
| [AI&nbsp;Landscape](../extensions/UnrealAILandscape/README.md) | 地形、海洋、湖泊和河流 | `Water`, UE 5.7+ |
| [AI&nbsp;Niagara](../extensions/UnrealAINiagara/README.md) | Niagara 系统、发射器、模块和碰撞 | `Niagara`, `ChaosNiagara`, UE 5.7+ |
| [AI&nbsp;PCG](../extensions/UnrealAIPCG/README.md) | PCG 图、体积和生成的散布 | `PCG`, UE 5.7+ |

扩展在你的项目中从源码编译（下次打开编辑器时，或使用 `--build` 立即编译）；安装程序会启用它们需要的引擎插件。

## 文档

| 页面 | 内容 |
|---|---|
| [docs/TOOLS.md](TOOLS.zh-CN.md) | 所有内置工具和系统工具、扩展、按工具设置可见性 |
| [docs/rql.md](rql.md) | RQL 查询语言：数据源、成员、修改操作、试玩测试 |
| [docs/CONFIGURATION.md](CONFIGURATION.zh-CN.md) | 首次运行、更新、环境变量、故障排查 |
| [docs/EXTENSIONS.md](EXTENSIONS.md) | 编写你自己的 RQL 模块、工具、提示词和资源 |
| [docs/RUNTIME.md](RUNTIME.zh-CN.md) | 运行时（游戏内）模式、它的安全关卡、让插件不进入发布的游戏 |
| [cli/README.md](../cli/docs/README.zh-CN.md) | `unreal-mcp-cli` 命令参考和退出码 |
| [docs/ARCHITECTURE.md](ARCHITECTURE.md) | 权威设计文档（IPC、注册、边车进程、UI、配置） |
| [docs/RELEASING.md](RELEASING.md) | 维护者：发布、CI 运行器、密钥 |
| [docs/EXTENSION-MONOREPO.md](EXTENSION-MONOREPO.md) | 维护者：测试和发布扩展 |

## 仓库结构

| 路径 | 内容 |
|---|---|
| [`UnrealMCP/`](../UnrealMCP/) | Unreal 插件（C++）：`UnrealMcpRuntime`、`UnrealMcpRql`、`UnrealMcpEditor` |
| [`bridge/`](../bridge/) | `unreal-mcp-bridge`，.NET 9 边车进程 |
| [`cli/`](../cli/) | `unreal-mcp-cli`，npm 包 |
| [`extensions/`](../extensions/) | 六个可选引擎扩展，各自独立发布 |
| [`samples/`](../samples/) | 一个编辑器扩展模板和一个运行时（游戏内）工具示例 |

## 链接

- [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G) · [Issues](https://github.com/IvanMurzak/Unreal-MCP/issues)
- 姊妹项目：[Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) · [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP)
- 共享组件：[GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) · [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) · [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet)
- [Model Context Protocol](https://modelcontextprotocol.io/)

## 许可证

[Apache-2.0](../LICENSE) © Ivan Murzak
