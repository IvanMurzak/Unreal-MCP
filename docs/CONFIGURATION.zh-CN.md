<b>[English](CONFIGURATION.md) | [中文](CONFIGURATION.zh-CN.md) | [日本語](CONFIGURATION.ja.md) | [Español](CONFIGURATION.es.md) | [فارسی](CONFIGURATION.fa.md) | [हिन्दी](CONFIGURATION.hi.md) | [العربية](CONFIGURATION.ar.md)</b>

# 配置、更新和故障排查

如何连接编辑器、保持插件为最新版本、用环境变量进行调整，以及修复常见问题。安装步骤见 [README](README.zh-CN.md)；这一切背后的设计见 [ARCHITECTURE.md](ARCHITECTURE.md)。

- [首次运行](#首次运行)
- [更新插件](#更新插件)
- [环境变量](#环境变量)
- [故障排查](#故障排查)

## 首次运行

1. 从编辑器的 **Tools** 菜单打开 **AI Game Developer** 主窗口（该标签页注册在 Tools 菜单类别下）。
2. **选择连接模式：**
   - **Cloud**（默认）— 连接到 [ai-game.dev](https://ai-game.dev)。点击 **Authorize**（授权）开始 OAuth **设备码流程**：窗口会显示一个验证 URL 和一个简短的用户码；打开该 URL，输入用户码并批准，编辑器即完成授权。使用 **Revoke**（撤销）清除已存储的云端令牌。
   - **Custom** — 连接到你运行的本地 `gamedev-mcp-server`（或任何兼容的服务器）。输入服务器 URL，并让你的 AI 客户端指向它。你可以自己运行服务器，也可以让编辑器启动和停止它（`UNREAL_MCP_START_SERVER`，仅限 Custom 模式 + http；见[环境变量](#环境变量)）。
3. **Connection** 部分显示一个状态圆点、一个状态标签和一个 Connect / Disconnect / Stop 按钮；bridge 状态显示为 `Running (restarts: N)` 或 `Stopped`。用它们确认边车进程正在运行。
4. 让你的 AI 客户端（Claude Code、Cursor、AI Game Developer 应用……）指向该服务器。**AI agents** 部分列出当前已连接的智能体；要写入 MCP 客户端配置，请使用 `unreal-mcp-cli setup-mcp`。

连接设置保存在 `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json`（每个 UE 模板都会在 gitignore 中忽略 `Saved/`，因此默认情况下令牌不会进入版本控制）。

> **固定的 MCP 客户端 URL。** `unreal-mcp-cli setup-mcp <agent>` 写入的 MCP 客户端配置指向**按项目固定**的云端 URL `<base>/mcp/p/<pin>`，因此即使你的账户操控多个项目，智能体也会路由到**这个**项目的编辑器。传入 `--no-pin` 则改为写入不带固定段的 `<base>/mcp` URL。固定段只是一个路由路径段——OAuth 资源仍是 `<base>/mcp`。

> **项目密钥。** 当本机已登录（`unreal-mcp-cli login`，或编辑器的 Sign in）时，Cloud 配置还会为每个智能体附带 `Authorization: Bearer agd_pk_…`——一个永不过期、可撤销、绑定到本项目固定段的密钥——因此任何智能体都不需要自己的 OAuth。该密钥缓存在 `~/.ai-game-dev/project-keys.json` 中并被复用。传入 `--oauth` 可得到仅含 URL 的配置（由智能体自己登录），传入 `--regenerate-key` 可生成新密钥并撤销旧密钥（编辑器的 AI agent 面板中有一个 **Regenerate key** 按钮）。未登录，或服务器无法生成密钥 ⇒ 使用仅含 URL 的配置。

## 更新插件

原地更新必须始终让你运行**新**代码。风险来自 UE 的增量编译器：如果插件源码发生变化（新的 `.cpp` 文件、新的模块），但旧的 `UnrealMCP/Intermediate/` 构建缓存仍然存在，UE 可能会基于过时的模块文件列表进行部分重新编译，悄无声息地让你停留在旧的/不完整的代码上。每种渠道的处理方式不同：

- **Fab / Epic Marketplace→ 自动。** Epic Games Launcher 会原地替换预编译的二进制文件；无需编译，也无需清理缓存。
- **`unreal-mcp-cli update` → 自动干净重建。** `update` 会重新复制插件源码，并且**默认**删除已安装插件过时的 `Intermediate/` 和 C++ `Binaries/`，让 UE 在下次启动编辑器时进行干净编译——无需手动操作。`Binaries/ThirdParty/UnrealMcpBridge/<rid>/` 下捆绑的边车 bridge 会保持不变：发布源码安装会从 `Source/ThirdParty/...` 刷新它，而仓库/开发安装在需要时会保留之前捆绑的副本。开发用的 **junction** 安装永远不会被清理（否则会清空你实时源码树的输出）。传入 `--no-clean` 可跳过缓存清理。

  ```bash
  unreal-mcp-cli update <YourProject>            # default: clean rebuild on version change
  unreal-mcp-cli update <YourProject> --force    # re-copy even when versions match
  unreal-mcp-cli update <YourProject> --no-clean # keep the existing build cache
  unreal-mcp-cli update <YourProject> --no-extensions # leave the installed extensions as they are
  unreal-mcp-cli update <YourProject> --no-self  # leave the CLI as it is
  unreal-mcp-cli update <YourProject> --check    # only report; exit code 2 when something is outdated
  ```

- **手动复制 → 自己清理缓存。** 如果你手动覆盖 `<YourProject>/Plugins/UnrealMCP/`，请**先关闭编辑器**，删除 `<YourProject>/Plugins/UnrealMCP/Intermediate/` 和 C++ `Binaries/`（如果存在捆绑的 bridge，请保留 `Binaries/ThirdParty/`），然后重新启动，让 UE 干净地重新编译。

## 环境变量

插件读取配置的优先级为 **进程环境变量 → `<Project>/.env` → 配置文件 → 内置默认值**。可识别的 `UNREAL_MCP_*` 变量（定义在 `UnrealMCP/Source/UnrealMcpRuntime/Private/Config/UnrealMcpConfig.cpp` 中；`UNREAL_MCP_CREDENTIALS_DIR` 由 bridge 读取，`UNREAL_MCP_SERVER_PATH` 由 CLI 读取）：

| 变量 | 用途 |
| --- | --- |
| `UNREAL_MCP_CONNECTION_MODE` | `Cloud` 或 `Custom`。**默认为 `Cloud`。** |
| `UNREAL_MCP_HOST` | 服务器 URL（Custom 模式），例如 `http://localhost:<port>` |
| `UNREAL_MCP_CLOUD_URL` | 云后端 URL（默认为 ai-game.dev） |
| `UNREAL_MCP_TOKEN` | 认证令牌（边车进程 IPC 令牌 / 服务器令牌）。**机密——切勿提交。** |
| `UNREAL_MCP_AUTH_OPTION` | `none` 或 `oauth`（本地服务器认证） |
| `UNREAL_MCP_KEEP_CONNECTED` | 保存/恢复连接状态 |
| `UNREAL_MCP_TOOLS` | 启用工具的覆盖设置（白名单；为空 = 不过滤） |
| `UNREAL_MCP_START_SERVER` | `true`：bridge 报告项目端口后，编辑器自己启动本地 `gamedev-mcp-server`（仅限 Custom 模式 + http；保存为 `startServer`），并在退出时停止它。`unreal-mcp-cli exec` / `open --headless` 会设置它。 |
| `UNREAL_MCP_TRANSPORT` | `stdio` 或 `http` |
| `UNREAL_MCP_LOG_LEVEL` | 日志详细程度 |
| `UNREAL_MCP_TOOL_TIMEOUT` | 一次工具调用在编辑器中可运行的时长，单位为毫秒（默认 `300000`；保存为 `toolTimeoutMs`）。运行较长的 `python-execute` 代码片段或资源操作时请调大它。编辑器会把该值加 10 秒作为 `tool-timeout` 传给它启动的本地服务器；较旧的服务器（McpPlugin 8.11.0 之前）无论此处如何设置，都会把工具调用限制在 5 分钟（[ARCHITECTURE §4](ARCHITECTURE.md)）。在编辑器开始执行之前就已超时或被取消的调用会被跳过，不会稍后再运行。 |
| `UNREAL_MCP_BRIDGE_PATH` | 边车进程二进制文件的路径——**开发/CI 覆盖**；优先于捆绑的二进制文件（§6）。打包发布版会自动找到捆绑的边车进程，因此最终用户永远不需要设置它；从源码构建时使用它。 |
| `UNREAL_MCP_CREDENTIALS_DIR` | bridge 读取（并刷新）的本机凭据存储的绝对路径文件夹，用来代替 `~/.ai-game-dev`，需在编辑器的进程环境中设置（不从 `.env` 读取）——是一个路径，绝不是令牌；相对路径会被记录到日志并忽略。用于启动器以另一个配置文件登录的开发/测试编辑器（AI Game Dev App 的本地开发环境）；对应 Godot 的 `GODOT_MCP_CREDENTIALS_DIR`。 |
| `UNREAL_MCP_SERVER_PATH` | 本地 `gamedev-mcp-server` 二进制文件的路径（由 `unreal-mcp-cli` 读取，而不是插件）——跳过服务器下载 + 版本检查（§6）。 |
| `AIGD_SESSION_IDLE_SECONDS` | 由 `unreal-mcp-cli` 在它启动的编辑器上设置：在这么多秒内没有工具调用（正在进行的调用、Play In Editor、资源编译和用户输入都算作活动）后，编辑器会保存有路径的未保存包并退出。只有同时设置了 `AIGD_SESSION_OWNER=cli` 和 nonce、且项目租约仍指向这个编辑器时才会生效；未设置 = 永不退出。 |
| `AIGD_SESSION_OWNER` / `AIGD_SESSION_NONCE` | `cli` 以及 `<project>/.ai-game-dev/session.json` 的 nonce：标记该编辑器由 CLI 会话启动。只有这样的编辑器才接受 `editor-quit`，并且在退出时，如果租约仍带有它的 nonce，就会删除该租约。 |

> **切勿提交 `.env`。** 项目根目录的 `.env` 可能包含 `UNREAL_MCP_TOKEN`，而 UE 项目模板不附带 `.gitignore`。`unreal-mcp-cli configure` 会把 `.env` 追加到目标项目的 `.gitignore`；本仓库自己的脚手架已经忽略了它。边车进程的 IPC 令牌通过 stdin（绝不通过 argv）传递，且绝不记录到日志。

## 故障排查

- **出现 `No connected clients. Retrying [1..10]`，随后本地服务器返回 HTTP 500。** 连接模式默认为 `Cloud`，因此边车进程拨号到了 ai-game.dev，而不是你的本地服务器。设置 `UNREAL_MCP_CONNECTION_MODE=Custom`（环境变量、`.env` 或 UI 开关）。
- **启动时没有 `[Unreal-MCP] plugin loaded` 这一行。** 编辑器模块加载失败——在 Output Log 中查找 `StartupModule` 错误或格式错误的 `.uplugin`。
- **通过 REST 直通调用时，工具返回 `'x' is required.`。** 发送 `-H "Content-Type: application/json"`；缺少它时，服务器会丢弃 JSON 请求体。
- **截图工具返回 `Screenshot tools are unavailable: this editor runs with the null RHI (-nullrhi)`。** 在 `-nullrhi` 下不会渲染任何内容。不带它重新启动编辑器：使用窗口模式，或使用能渲染截图的 `-RenderOffscreen` 无界面模式（使用 `unreal-mcp-cli` 时，去掉 `--nullrhi`）。
- **没有边车进程 / 边车进程不断重启。** 在 **Connection** 部分查看 bridge 状态（`Running (restarts: N)` / `Stopped`）。如果日志显示 `no sidecar binary resolved for rid <rid> …`，说明捆绑的二进制文件、下载的二进制文件和 `UNREAL_MCP_BRIDGE_PATH` 都没有找到。Fab 安装需要手动安装 bridge：**Get bridge** 会打开发布页面，把对应你平台的 zip 解压到 **Open bridge folder** 显示的文件夹中，然后点击 **Connect**（Fab 版本绝不会自行下载或启动程序）；CLI / 打包发布版会捆绑它，所以在那里出现这个问题意味着插件打包时没有包含你平台的 bridge（请重新安装正确的版本）；在从源码构建的版本中，设置 `UNREAL_MCP_BRIDGE_PATH`，或运行 `unreal-mcp-cli bootstrap-local` 从源码构建一个（[ARCHITECTURE](ARCHITECTURE.md) §6.3、§6.8）。
- **端口。** IPC 使用 `30000–39999` 范围内按项目确定的端口，冲突时向后探测（然后使用临时端口）；本地服务器使用 `20000–29999` 范围内由哈希确定的端口，不做探测——CLI 无需读取任何配置就能算出同一个端口号，服务器绑定的正是所请求的端口。具体端口号只是方便调试，并非必需。
- **日志。** 使用主窗口的 **Open log file** 操作、RQL 查询（`console | where Verbosity == "Error"`），或编辑器的 Output Log（`LogUnrealMcp` 类别）。
