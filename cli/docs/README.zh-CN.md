<div align="center">

<img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/readme/hero.svg" alt="Unreal MCP — 让任何 AI 智能体检查并操控 Unreal Editor" width="100%"/>

<br/>

[![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Node.js](https://img.shields.io/badge/Node.js-22.14%2B-0E7490?logo=nodedotjs&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://nodejs.org/) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE) [![Docs](https://img.shields.io/badge/docs-ai--game.dev-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

  <p>
    <a href="https://claude.ai/download"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://visualstudio.microsoft.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/visual-studio-64.png" alt="Visual Studio" title="Visual Studio" height="36"></a>&nbsp;&nbsp;
    <a href="https://opencode.ai/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/open-code-64.png" alt="OpenCode" title="OpenCode" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/Kilo-Org/kilocode"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/kilo-code-64.png" alt="Kilo Code" title="Kilo Code" height="36"></a>
  </p>

  <b>[English](../README.md) | [中文](README.zh-CN.md) | [日本語](README.ja.md) | [Español](README.es.md) | [فارسی](README.fa.md) | [हिन्दी](README.hi.md) | [العربية](README.ar.md)</b>

</div>

**`unreal-mcp-cli`** 是 [Unreal-MCP](../../docs/README.zh-CN.md) 的命令行工具，是 [`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) 和 [`godot-cli`](https://www.npmjs.com/package/godot-cli) 的 Unreal 版姊妹工具。它负责安装 UnrealMCP 插件、让你登录 [ai-game.dev](https://ai-game.dev)、配置你的 AI 智能体、用正确的连接设置打开 Unreal Editor，并在终端中调用插件的工具。

<p align="center">
  <img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/readme/terminal.svg" alt="终端：npm install -g unreal-mcp-cli、install-plugin、login、setup-mcp claude-code，然后打开编辑器" width="100%"/>
</p>

## 功能

✅ **安装插件** — 把 UnrealMCP 插件复制（或以 junction 链接）到项目的 `Plugins/`\
✅ **扩展** — 添加可选的引擎扩展（GAS、Niagara、PCG……）\
✅ **登录** — 通过 OAuth 设备码登录 `ai-game.dev`，每台机器一次\
✅ **配置智能体** — 为 Claude Code、Cursor、Copilot、Codex 等写入 MCP 配置\
✅ **打开与关闭** — 以窗口或无界面方式打开编辑器；只关闭由 CLI 启动的编辑器\
✅ **调用工具** — `rql`、`exec`、`run-tool`；按需启动无界面编辑器\
✅ **构建与测试** — 无需编辑器即可用 UBT 编译 C++ 并运行 Automation 测试\
✅ **对智能体友好** — `--format json`、稳定的退出码、`commands` 清单\
✅ **跨平台** — Windows、macOS 和 Linux

## 快速开始

需要 [Node.js](https://nodejs.org/) 22.14 或更高版本。

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # plugin + bundled bridge into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

如果本地有 `UnrealMCP/` 检出，`install-plugin` 和 `update` 会直接使用它；否则它们会下载与 CLI 版本对应的 GitHub Release 中的插件源码资产，并在解压前校验其签名（minisign / Ed25519）。`--plugin-source <dir>` 用于离线 / CI / 开发场景的覆盖。

## 使用

```bash
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"
unreal-mcp-cli compile ./MyGame --strict
unreal-mcp-cli test ./MyGame --junit-output junit.xml
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

`rql`、`exec` 和 `run-tool` 会复用正在运行的编辑器，或启动一个无界面编辑器；`compile` 和 `test` 无需编辑器即可运行。

## 命令

| 领域 | 命令 |
| --- | --- |
| 安装设置 | `create-project` · `install-engine` · `install-plugin` · `bootstrap-local` |
| 包 | `install-extension` · `update` · `remove-plugin` |
| 账户 | `login` |
| AI 智能体 | `setup-mcp` · `setup-skills` · `configure` |
| 编辑器 | `open` · `close` · `wait-for-ready` · `status` · `dialog` |
| 工具 | `tools` · `rql` · `run-tool` · `run-system-tool` · `exec` |
| 构建、测试 | `compile` · `test` |
| 发现 | `commands` |

所有选项、退出码和 JSON 输出：**[命令参考](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/commands.md)**（英文），或 `unreal-mcp-cli <command> --help`。

## 库

这些命令也是一个有类型、无副作用的库：不解析 argv、不写 stdout、不退出进程；每个结果都是 `kind: 'success' | 'failure'`。

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

## 链接

[Unreal-MCP](../../docs/README.zh-CN.md) · [工具](../../docs/TOOLS.zh-CN.md) · [配置](../../docs/CONFIGURATION.zh-CN.md) · [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G)

## 许可证

[Apache-2.0](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)
