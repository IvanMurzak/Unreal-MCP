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

**状态：**[最新版本](https://github.com/IvanMurzak/Unreal-MCP/releases/latest)（插件、sidecar 和 CLI 共用同一版本号）· Unreal Engine 5.5–5.8 · sidecar 构建覆盖 Windows x64、macOS（Intel 与 Apple 芯片）和 Linux x64 · 每个版本都在 Windows（UE 5.5–5.8）和 macOS（UE 5.7、5.8）上测试 · 可从 [Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) 和 [GitHub Releases](https://github.com/IvanMurzak/Unreal-MCP/releases) 获取。

## 功能

✅ **编写 C++** — 生成类、编辑源码、编译；错误会带着文件、行号和严重级别返回\
✅ **Blueprints** — 创建、编辑和编译，返回智能体可以修复的结构化错误\
✅ **RQL 数据层** — 用一种语言查询和修改 Actor、资源和关卡（[RQL](rql.md)）\
✅ **可视化反馈** — 视口、游戏视图、摄像机和单独 Actor 的截图\
✅ **Python** — 运行编辑器 Python，可使用完整的 `unreal` API\
✅ **Fab** — 搜索 Fab、列出你的资源库、添加你拥有的内容\
✅ **扩展** — GAS、Niagara、PCG、Landscape、Control Rig、Enhanced Input\
✅ **你自己的工具** — 从任何插件添加工具、提示词和资源（[指南](EXTENSIONS.md)）\
✅ **运行时模式** — 可选开启，用于驱动正在运行的游戏（[docs/RUNTIME.md](RUNTIME.zh-CN.md)）\
✅ **云端或本地** — 开箱即用的 `ai-game.dev`，或自托管的 `gamedev-mcp-server`

**覆盖面：**你在 Unreal 编辑器中能做的事，智能体几乎都能做。工具分组：RQL 数据层、Blueprints、C++ 源码、截图、Fab、控制台、编辑器 Python 和工具列表，以及各扩展。全部工具：[docs/TOOLS.md](TOOLS.zh-CN.md)。

**效率：**这些工具按紧凑原则设计，智能体完成每个任务所需的调用和 token 更少，因此更快、更省钱。对 RQL 数据层，这一点在 RQL 自己的评测中做过测量（一个沙盒游戏世界，相同的任务、模型和评分器）：与传统的查找 / 导出为 JSON / 赋值式反射工具相比，RQL 每个任务的工具调用约少 9–18 倍，token 约少 17–19 倍（[结果](https://github.com/IvanMurzak/RQL/blob/main/RESULTS.md#33-rql-vs-conventional-reflection-tools)）。

### 你可以这样问

| 对你的智能体说 | 它使用的工具 |
| --- | --- |
| “用 C++ 添加一个 `AHealthPickup` Actor 类，编译并修复所有错误” | `source-create-class`、`source-update`、`source-compile` |
| “做一个重叠时打开的门 Blueprint，编译后放进关卡” | `blueprint-*` |
| “数一数这个关卡里的树，并把所有点光源上移 1 米” | `rql-query`、`rql-mutate` |
| “从玩家摄像机截一张图，检查一下光照” | `screenshot-*` |
| “在 Fab 上找一个免费的岩石资源包并添加到项目中” | `fab-search`、`fab-add` |
| “用 Python 脚本批量重命名选中的资源” | `python-execute` |

**C++ 是一等公民的工作循环。**`source-compile` 在 Live Coding 启用时用它编译，否则用 UnrealBuildTool，并返回每条诊断的文件、行号和严重级别，智能体无需读构建日志就能修复错误。当改动需要完整构建并重启编辑器时（新增类、修改了 `UCLASS` / `UPROPERTY` / `UFUNCTION` 声明），它会明确提示。文件操作限定在 `<Project>/Source/` 内。在终端中，`unreal-mcp-cli compile` 无需编辑器即可输出 `file:line:col` 格式的诊断。

**RQL 从编辑器本身学习：**`rql-schema` 列出每个数据源、类型和成员，`unreal-rql` skill 用示例讲解语法（`unreal-mcp-cli setup-skills` 会为你的智能体写入它）。

## 系统要求

- **Unreal Engine 5.5 – 5.8**（发布流水线会逐一验证）。
- **C++ 项目**，因为插件以源码形式发布，Unreal 会用你平台的 C++ 工具链为你的项目编译它的模块。纯 Blueprint 项目一步即可变成 C++ 项目：在编辑器中选择 **Tools → New C++ Class…** 并创建任意类。新项目可用 `unreal-mcp-cli create-project ./MyGame` 生成一个最小的 C++ 项目。Fab 版本无需 C++ 项目：Epic Games Launcher 会安装预编译好的版本。
- **无需安装 .NET。**MCP 主机是一个自包含的 .NET 程序（`unreal-mcp-bridge`），按你的平台打包在插件内；插件负责启动和重启它。（Fab 版本不含可执行文件：它的 **Get bridge** 按钮会打开发布页面，你只需把 bridge 解压到插件的 bridge 文件夹一次。）只有从源码构建 bridge 才需要 .NET 9 SDK。
- **Node.js 22.14+**，仅 `unreal-mcp-cli` 需要（它是 npm 包，22.14 是其最低 Node 版本）。
- **Windows 或 macOS**（每个版本都在两者上测试）。**Linux x64：**sidecar 会构建并发布，但没有任何发布测试在 Linux 上运行。

## 快速开始

**1. 安装插件**，任选其一：

- **[Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)**（无需终端）：把 **AI Game Developer** 添加到你的引擎，在 **Edit → Plugins** 中启用，然后打开项目。
- **两条命令**（Node.js 22.14+）：

```bash
npx unreal-mcp-cli install-plugin ./MyGame   # plugin + bundled bridge into MyGame/Plugins
npx unreal-mcp-cli open ./MyGame             # build if needed and open the editor
```

**2. 在编辑器中连接：**打开 **Tools → AI Game Developer**。

- 仅限 Fab 安装：需要安装一次 `unreal-mcp-bridge` sidecar（Fab 版本不含可执行文件）：点击 **Get bridge** 打开发布页面，把对应平台的 zip 解压到 **Open bridge folder** 显示的文件夹，然后点击 **Connect**。
- **Cloud**（默认）：点击 **Authorize**，并在浏览器中批准登录。**本地**：切换到 **Custom**，点击 **MCP server** 旁的 **Start**；编辑器会下载经过校验和验证的 `gamedev-mcp-server`（Fab 安装需手动安装该服务器：日志会给出下载地址）。
- 在 **AI agent** 下选择你的智能体（Claude Code、Cursor、Codex……），点击 **Configure**。

**3. 向智能体提问：***“在一排放三个立方体，在它们上方放一盏点光源，然后截一张图。”*

本地还是 Cloud？两者都完整支持，之后也可以随时切换。**默认使用 Cloud**，因为它无需开放端口，也不会与其他程序发生端口冲突；该连接免费。**本地**模式无需账号或互联网，适合离线和物理隔离（air-gapped）的机器。

| | **本地** | **Cloud** |
| --- | --- | --- |
| 账号 | 不需要 | 登录 ai-game.dev |
| MCP 服务器 | 你机器上的 `gamedev-mcp-server`，由编辑器或智能体启动 | ai-game.dev 中继 |
| 流量 | 留在 `localhost` | 经过 ai-game.dev（[它记录什么](#what-the-cloud-relay-sees)） |
| 认证 | loopback 上无需认证，或一个共享令牌（一个参数） | OAuth 或按项目的密钥 |
| 适合 | 离线和隔离环境、CI、私有项目 | 无需开放端口、没有端口冲突；一个账号用于多个项目和机器 |

<details>
<summary><b>全部在终端完成</b>（CLI：Cloud、本地、令牌认证）</summary>

<p align="center">
  <img src="img/readme/terminal.svg" alt="终端：npm install -g unreal-mcp-cli、install-plugin、login、setup-mcp claude-code、打开编辑器" width="100%"/>
</p>

**Cloud**（插件的默认模式）：

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

**本地**（无需账号）：

```bash
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin ./MyGame
unreal-mcp-cli configure -p ./MyGame --mode Custom                 # plugin: local server
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --transport stdio # agent starts it
unreal-mcp-cli open ./MyGame
```

`setup-mcp --transport stdio` 会把经过校验和验证的 `gamedev-mcp-server` 版本下载到项目中，并让智能体指向它。双方使用 `localhost` 上同一个按项目分配的端口。

**开启令牌认证（本地）：**在 `configure` 命令后加上这些参数，再运行一次 `setup-mcp`；此后插件和服务器都要求该密钥。

```bash
unreal-mcp-cli configure -p ./MyGame --mode Custom --auth required --token <secret>
```

为什么本地默认不认证：服务器只绑定 loopback，并拒绝来自非本地浏览器源的请求，所以只有你本机上的程序能访问它。如果你不信任这台机器上的所有程序或用户，就使用令牌。密钥会写入项目的 `.env`（`configure` 会把它加入 gitignore）和智能体的配置文件，请让该文件远离版本控制。

</details>

<a id="what-the-cloud-relay-sees"></a>**云端中继能看到什么。**在 Cloud 模式下，工具调用经过 ai-game.dev 的 MCP 中继。该中继是无状态的：它在你的 AI 客户端和编辑器之间转发工具调用，不检查也不保存其内容，也不收集你的对话、源代码、项目文件、资源或场景数据。它会记录请求元数据（客户端 IP 地址、时间戳和耗时、状态码和请求路径、工具名称、请求和响应大小）以及 MCP 事件元数据（事件类型、时间、状态、会话标识、所涉及的工具/提示词/资源名称以及错误详情）。请求日志和 MCP 事件主要在滚动的 90 天窗口内使用；隐私政策不保证在固定期限后自动删除。详情：[隐私政策](https://ai-game.dev/legal/privacy)。

`install-plugin` 会下载与 CLI 版本相匹配的已签名插件源码发布包。

其他安装方式：

- **Fab 格式 zip**：除了 [Fab 商品页](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)，每个 GitHub 版本还附带一个不含可执行文件的 Fab 格式构建（`unreal-mcp-plugin-fab-<version>.zip`）；无论用哪一个，窗口中的 **Get bridge** 按钮都会引导你一次性安装 sidecar。
- **手动**：把 [`UnrealMCP/`](../UnrealMCP/) 复制到 `<YourProject>/Plugins/UnrealMCP/`。源码检出不含捆绑的 sidecar：运行 `unreal-mcp-cli bootstrap-local` 或设置 `UNREAL_MCP_BRIDGE_PATH`。

更新（`unreal-mcp-cli update`）、连接模式、环境变量和故障排查：[docs/CONFIGURATION.md](CONFIGURATION.zh-CN.md)。

### 智能体配置

`unreal-mcp-cli setup-mcp <agent> -p ./MyGame` 会替你写入配置（`setup-mcp --list` 列出所有支持的智能体）。它写入的内容：

<details>
<summary><b>Claude Code</b>：<code>.mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp claude-code -p ./MyGame
```

```json
{
  "mcpServers": {
    "unreal-mcp": {
      "type": "http",
      "url": "https://ai-game.dev/mcp/p/<pin>",
      "headers": { "Authorization": "Bearer agd_pk_…" }
    }
  }
}
```

`<pin>` 会路由到这个项目的编辑器。机器已登录时，请求头是这个项目的密钥；`--oauth` 只写入 URL，由智能体自行登录。使用 `--transport stdio` 时，该条目改为运行本地服务器：`"command"` 是 `<project>/Intermediate/UnrealMCP/server/` 下的 `gamedev-mcp-server`，`"args"` 设置 `port=<port>`、`client-transport=stdio` 和 `authorization=none`。

</details>

<details>
<summary><b>Cursor</b>：<code>.cursor/mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp cursor -p ./MyGame
```

与 Claude Code 相同的 `mcpServers` → `unreal-mcp` 条目：`"type": "http"` 加 URL，或 `"type": "stdio"` 加本地服务器的 `command` 和 `args`。

</details>

<details>
<summary><b>Codex</b>：<code>.codex/config.toml</code></summary>

```bash
unreal-mcp-cli setup-mcp codex -p ./MyGame
```

```toml
[mcp_servers.unreal-mcp]
enabled = true
url = "https://ai-game.dev/mcp/p/<pin>"
tool_timeout_sec = 300
startup_timeout_sec = 30
```

使用项目密钥时，该节还会带上含 `Authorization` 请求头的 `http_headers`。

</details>

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

全部命令和选项：[cli/docs/commands.md](../cli/docs/commands.md) 或 `unreal-mcp-cli commands`。

## 安全

- **插件 ↔ sidecar：**插件监听一个 loopback TCP 端口；sidecar 用通过 stdin 传入的一次性令牌连接它（从不出现在命令行中，也从不写入日志）。
- **本地服务器：**`gamedev-mcp-server` 默认只绑定 loopback，对 `Origin` 不被允许的请求返回 `403`（防御 DNS 重绑定）。在 loopback 上，除非你选择令牌（`UNREAL_MCP_AUTH_OPTION`、`UNREAL_MCP_TOKEN`），插件会以无认证方式启动它。服务器选项：[托管与认证](https://github.com/IvanMurzak/GameDev-MCP-Server/blob/main/docs/hosting.md)。
- **Cloud：**智能体通过 OAuth 登录，或使用按项目的密钥，可用 `setup-mcp --regenerate-key` 更换。中继是无状态的，不保存工具调用内容；它会记录请求和 MCP 事件元数据（[详情](#what-the-cloud-relay-sees)）。
- **工具能做什么：**编辑 `<Project>/Source/` 下的文件，运行控制台命令和编辑器 Python，修改任何反射属性。
- **对工具的控制：**
  - **白名单**（硬限制）：`UNREAL_MCP_TOOLS` 只提供列出的工具；其他工具既不会列出，也无法调用。
  - **隐藏**（软限制）：**MCP Tools** 窗口中的 **Visible** 复选框会把工具从 `tools/list` 中移除，但按名称调用它的客户端仍能运行它。
  - **有边界的修改：**C++ 文件工具限定在 `<Project>/Source/` 内；一次 `rql-mutate` 调用要么全部成功要么全部不生效，只占一个撤销步骤，可用 `dry_run` 预览，目标超过 200 个时会被拒绝，除非调用调高 `max_targets`（最多 1000）；`python-execute` 在 `timeout` 后停止（默认 30 秒）；每次工具调用都有时间上限（`UNREAL_MCP_TOOL_TIMEOUT`，默认 5 分钟）。
  - **不开放给 AI：**系统工具（`ping`、skill 生成、`editor-quit`）不出现在 `tools/list` 中，且 `editor-quit` 只有在 CLI 会话的 nonce 匹配时才会执行。
  - 详情：[docs/TOOLS.md](TOOLS.zh-CN.md)。
- **运行时模式默认关闭：**游戏永远不会自行连接。它需要 `bRuntimeMcpEnabled` 设置（默认关闭）和一次显式的 `Connect()` 调用，除非你允许远程主机，否则只接受 loopback 主机，并且在 Shipping 构建中会被拒绝，除非编译时包含它。用 `TargetDenyList` 把插件排除在打包的游戏之外（[docs/RUNTIME.md](RUNTIME.zh-CN.md)）。

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
| [cli/docs/commands.md](../cli/docs/commands.md) | `unreal-mcp-cli` 命令参考和退出码 |
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

**游戏开发者可免费使用，包括商业游戏。**Unreal MCP 采用 [Apache-2.0](../LICENSE) © Ivan Murzak。两个捆绑组件 RQL 和 Code-Exec 采用 [PolyForm Shield 1.0.0](https://polyformproject.org/licenses/shield/1.0.0) 许可证，它只限制开发与之竞争的游戏开发 AI 工具的公司；你可以用它们全部来制作、发布和销售你的游戏。声明：[`UnrealMCP/ThirdPartyNotices.txt`](../UnrealMCP/ThirdPartyNotices.txt)。
