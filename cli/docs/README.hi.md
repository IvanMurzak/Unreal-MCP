<div align="center">

<img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/readme/hero.svg" alt="Unreal MCP — किसी भी AI agent को Unreal Editor देखने और चलाने दें" width="100%"/>

<br/>

[![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Node.js](https://img.shields.io/badge/Node.js-22.14%2B-0E7490?logo=nodedotjs&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://nodejs.org/) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE) [![Docs](https://img.shields.io/badge/docs-ai--game.dev-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

  <p>
    <a href="https://claude.ai/download"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://visualstudio.microsoft.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/visual-studio-64.png" alt="Visual Studio" title="Visual Studio" height="36"></a>&nbsp;&nbsp;
    <a href="https://opencode.ai/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/open-code-64.png" alt="OpenCode" title="OpenCode" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/Kilo-Org/kilocode"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/kilo-code-64.png" alt="Kilo Code" title="Kilo Code" height="36"></a>
  </p>

  <b>[English](../README.md) | [中文](README.zh-CN.md) | [日本語](README.ja.md) | [Español](README.es.md) | [فارسی](README.fa.md) | [हिन्दी](README.hi.md) | [العربية](README.ar.md)</b>

</div>

**`unreal-mcp-cli`** [Unreal-MCP](../../docs/README.hi.md) का command-line tool है, और [`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) व [`godot-cli`](https://www.npmjs.com/package/godot-cli) का Unreal वाला साथी है। यह UnrealMCP plugin install करता है, आपको [ai-game.dev](https://ai-game.dev) में sign in कराता है, आपके AI agent को configure करता है, सही connection settings के साथ Unreal Editor खोलता है और terminal से plugin के tools call करता है।

<p align="center">
  <img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, editor खोलें" width="100%"/>
</p>

## विशेषताएँ

✅ **Plugin install** — UnrealMCP plugin को project के `Plugins/` में copy (या junction) करें\
✅ **Extensions** — वैकल्पिक engine extensions जोड़ें (GAS, Niagara, PCG, …)\
✅ **Sign in** — `ai-game.dev` में OAuth device-code login, हर machine पर एक बार\
✅ **Agents सेट करें** — Claude Code, Cursor, Copilot, Codex और दूसरों के लिए MCP configs\
✅ **खोलें और बंद करें** — editor window के साथ या headless खोलें; सिर्फ़ CLI से शुरू हुए बंद करें\
✅ **Tools call करें** — `rql`, `exec`, `run-tool`; ज़रूरत पड़ने पर headless editor शुरू होता है\
✅ **Build और test** — बिना editor के UBT से C++ compile करें और Automation tests चलाएँ\
✅ **Agent के अनुकूल** — `--format json`, स्थिर exit codes, एक `commands` manifest\
✅ **Cross-platform** — Windows, macOS और Linux

## जल्दी शुरू करें

[Node.js](https://nodejs.org/) 22.14 या उससे नया चाहिए।

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # plugin + bundled bridge into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` और `update` local `UnrealMCP/` checkout मौजूद हो तो उसी का इस्तेमाल करते हैं; वरना वे CLI version से मेल खाने वाली GitHub Release का plugin source asset download करते हैं और extract करने से पहले उसका signature (minisign / Ed25519) verify करते हैं। `--plugin-source <dir>` offline / CI / dev के लिए override है।

## उपयोग

```bash
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"
unreal-mcp-cli compile ./MyGame --strict
unreal-mcp-cli test ./MyGame --junit-output junit.xml
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

`rql`, `exec` और `run-tool` चल रहे editor को दोबारा इस्तेमाल करते हैं या headless editor शुरू करते हैं; `compile` और `test` बिना editor के चलते हैं।

## Commands

| क्षेत्र | Commands |
| --- | --- |
| Setup | `create-project` · `install-engine` · `install-plugin` · `bootstrap-local` |
| Packages | `install-extension` · `update` · `remove-plugin` |
| Account | `login` |
| AI agents | `setup-mcp` · `setup-skills` · `configure` |
| Editor | `open` · `close` · `wait-for-ready` · `status` · `dialog` |
| Tools | `tools` · `rql` · `run-tool` · `run-system-tool` · `exec` |
| Build, test | `compile` · `test` |
| Discovery | `commands` |

हर option, exit codes और JSON output: **[command reference](https://github.com/IvanMurzak/Unreal-MCP/blob/docs/readme-refresh/cli/docs/commands.md)**, या `unreal-mcp-cli <command> --help`।

## Library

Commands एक typed, side-effect-free library के रूप में भी उपलब्ध हैं: न argv parsing, न stdout, न process exit; हर result `kind: 'success' | 'failure'` होता है।

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

## Links

[Unreal-MCP](../../docs/README.hi.md) · [Tools](../../docs/TOOLS.hi.md) · [Configuration](../../docs/CONFIGURATION.hi.md) · [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G)

## License

[Apache-2.0](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)
