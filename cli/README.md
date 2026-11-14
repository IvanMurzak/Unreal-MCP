<div align="center">

<img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/readme/hero.svg" alt="Unreal MCP — let any AI agent inspect and drive the Unreal Editor" width="100%"/>

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

  <b>[English](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/README.md) | [中文](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/README.zh-CN.md) | [日本語](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/README.ja.md) | [Español](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/README.es.md) | [فارسی](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/README.fa.md) | [हिन्दी](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/README.hi.md) | [العربية](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/README.ar.md)</b>

</div>

**`unreal-mcp-cli`** is the command-line tool of [Unreal-MCP](https://github.com/IvanMurzak/Unreal-MCP), the Unreal sibling of [`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) and [`godot-cli`](https://www.npmjs.com/package/godot-cli). It installs the UnrealMCP plugin, signs you in to [ai-game.dev](https://ai-game.dev), configures your AI agent, opens the Unreal Editor with the right connection settings and calls the plugin's tools from the terminal.

<p align="center">
  <img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, open the editor" width="100%"/>
</p>

## Features

✅ **Install plugin** — copy (or junction) the UnrealMCP plugin into a project's `Plugins/`\
✅ **Extensions** — add optional engine extensions (GAS, Niagara, PCG, …)\
✅ **Sign in** — OAuth device-code login to `ai-game.dev`, once per machine\
✅ **Set up agents** — MCP configs for Claude Code, Cursor, Copilot, Codex and more\
✅ **Open & close** — open the editor, windowed or headless; close only CLI-started ones\
✅ **Call tools** — `rql`, `exec`, `run-tool`; a headless editor starts on demand\
✅ **Build & test** — compile C++ with UBT and run Automation tests without an editor\
✅ **Agent-friendly** — `--format json`, stable exit codes, a `commands` manifest\
✅ **Cross-platform** — Windows, macOS and Linux

## Quick start

Requires [Node.js](https://nodejs.org/) 22.14 or later.

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # plugin + bundled bridge into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` and `update` use a local `UnrealMCP/` checkout when present; otherwise they download the plugin source asset of the GitHub Release that matches the CLI version and verify its signature (minisign / Ed25519) before extracting it. `--plugin-source <dir>` is the offline / CI / dev override.

## Usage

```bash
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"
unreal-mcp-cli compile ./MyGame --strict
unreal-mcp-cli test ./MyGame --junit-output junit.xml
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

`rql`, `exec` and `run-tool` reuse a running editor or start a headless one; `compile` and `test` run without an editor.

## Commands

| Area | Commands |
| --- | --- |
| Setup | `create-project` · `install-engine` · `install-plugin` · `bootstrap-local` |
| Packages | `install-extension` · `update` · `remove-plugin` |
| Account | `login` |
| AI agents | `setup-mcp` · `setup-skills` · `configure` |
| Editor | `open` · `close` · `wait-for-ready` · `status` · `dialog` |
| Tools | `tools` · `rql` · `run-tool` · `run-system-tool` · `exec` |
| Build, test | `compile` · `test` |
| Discovery | `commands` |

Every option, exit codes and JSON output: **[command reference](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/commands.md)**, or `unreal-mcp-cli <command> --help`.

## Library

The commands are also a typed, side-effect-free library: no argv parsing, no stdout, no process exit; every result is `kind: 'success' | 'failure'`.

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

## Links

[Unreal-MCP](https://github.com/IvanMurzak/Unreal-MCP) · [Tools](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/TOOLS.md) · [Configuration](https://github.com/IvanMurzak/Unreal-MCP/blob/main/docs/CONFIGURATION.md) · [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G)

## License

[Apache-2.0](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)
