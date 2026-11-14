<div align="center">

<img src="img/readme/hero.svg" alt="Unreal MCP — Claude, Cursor, Copilot या किसी भी MCP agent को Unreal Editor देखने और चलाने दें" width="100%"/>

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

**Unreal MCP** (Unreal के लिए AI Game Developer) किसी भी MCP-सक्षम AI agent को **Unreal Editor** से जोड़ता है। सीधी भाषा में बताइए, और agent actors spawn करता है, levels edit करता है, Blueprints बनाता और compile करता है, C++ edit और compile करता है, assets संभालता है और screenshots से अपने काम की जाँच करता है। यह [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) और [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) का Unreal वाला साथी है, और वही cloud backend ([ai-game.dev](https://ai-game.dev)) या आपका अपना [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) इस्तेमाल करता है।

<p align="center">
  <img src="img/readme/architecture.svg" alt="AI agent server से MCP में बात करता है; server SignalR के ज़रिए unreal-mcp-bridge sidecar तक संदेश पहुँचाता है, जो loopback TCP पर Unreal Editor में UnrealMCP C++ plugin तक पहुँचता है" width="100%"/>
</p>

Unreal का editor C++ में है, इसलिए .NET MCP host उसके साथ एक छोटे **sidecar** (`unreal-mcp-bridge`) के रूप में चलता है, जिसे plugin आपके लिए शुरू करता है, उस पर नज़र रखता है और ज़रूरत पर restart करता है। डिज़ाइन का विवरण: [docs/ARCHITECTURE.md](ARCHITECTURE.md)।

## विशेषताएँ

✅ **RQL data layer** — एक ही भाषा में actors, assets और levels को query करें और बदलें ([RQL](rql.md))\
✅ **Blueprints** — बनाएँ, edit करें और compile करें, ऐसी structured errors के साथ जिन्हें agent ठीक कर सके\
✅ **C++** — project source का ढाँचा बनाएँ, edit और compile करें (Live Coding या UBT)\
✅ **Visual feedback** — viewport, game-view, camera और अलग किए गए actor के screenshots\
✅ **Python** — पूरे `unreal` API के साथ editor Python चलाएँ\
✅ **Fab** — Fab में खोजें, अपनी library देखें, अपना खरीदा content जोड़ें\
✅ **Extensions** — GAS, Niagara, PCG, Landscape, Control Rig, Enhanced Input\
✅ **आपके अपने tools** — किसी भी plugin से tools, prompts और resources जोड़ें ([गाइड](EXTENSIONS.md))\
✅ **Runtime mode** — चलते हुए game को चलाने के लिए opt in करें ([docs/RUNTIME.md](RUNTIME.hi.md))\
✅ **Cloud या local** — `ai-game.dev` तुरंत तैयार, या self-hosted `gamedev-mcp-server`

8 families में 30 built-in tools; पूरी सूची [docs/TOOLS.md](TOOLS.hi.md) में है।

## आवश्यकताएँ

- **Unreal Engine 5.5 – 5.8** (release pipeline इनमें से हर एक को validate करती है) और एक **C++ project**: plugin आपके project में editor modules compile करता है।
- `unreal-mcp-cli` के लिए **Node.js 22.14+**।
- Windows, macOS या Linux। .NET install करने की ज़रूरत नहीं: sidecar plugin के अंदर ही आता है। (.NET 9 SDK सिर्फ़ bridge को source से build करने के लिए चाहिए।)

## जल्दी शुरू करें

<p align="center">
  <img src="img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, editor खोलें" width="100%"/>
</p>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` CLI version से मेल खाने वाली signed plugin source release download करता है। Editor में **Tools → AI Game Developer** खोलें, वहाँ connection, bridge का status और जुड़े हुए agents दिखते हैं। फिर अपने agent से कहें:

> *"एक कतार में तीन cubes spawn करो और उनके ऊपर एक point light लगाओ, फिर screenshot लो।"*

Install करने के दूसरे तरीके:

- **Fab / Epic Marketplace**: [Fab पर AI Game Developer](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)। Fab build में executables शामिल नहीं हो सकते, इसलिए वहाँ window का **Get bridge** button आपको एक बार sidecar install करने के चरण बताता है।
- **Manual**: [`UnrealMCP/`](../UnrealMCP/) को `<YourProject>/Plugins/UnrealMCP/` में copy करें। Source checkout में sidecar साथ नहीं आता: `unreal-mcp-cli bootstrap-local` चलाएँ या `UNREAL_MCP_BRIDGE_PATH` set करें।

Update करना (`unreal-mcp-cli update`), connection modes, environment variables और troubleshooting: [docs/CONFIGURATION.md](CONFIGURATION.hi.md)।

## उपयोग

ज़्यादातर समय आप बस अपने agent से बात करते हैं। CLI भी उन्हीं tools को सीधे call करता है, और कोई editor न चल रहा हो तो headless editor शुरू कर देता है:

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

सभी commands और options: [cli/README.md](../cli/docs/README.hi.md) या `unreal-mcp-cli commands`।

## Extensions

Extensions optional plugins हैं जो RQL को बढ़ाते हैं; ये अपना कोई tool नहीं जोड़ते।

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

| Extension | क्या जोड़ता है | ज़रूरतें |
| --- | --- | --- |
| [AI&nbsp;ControlRig](../extensions/UnrealAIControlRig/README.md) | Procedural animation rigs और उनकी Anim Blueprint wiring | `ControlRig`, `RigVM`, UE 5.7+ |
| [AI&nbsp;EnhancedInput](../extensions/UnrealAIEnhancedInput/README.md) | Input actions, mapping contexts, triggers और modifiers | `EnhancedInput`, UE 5.7+ |
| [AI&nbsp;GAS](../extensions/UnrealAIGAS/README.md) | Gameplay Effects, ability Blueprints और attribute sets | `GameplayAbilities`, `EnhancedInput`, `GameplayTagsEditor`, UE 5.7+ |
| [AI&nbsp;Landscape](../extensions/UnrealAILandscape/README.md) | Landscapes, oceans, lakes और rivers | `Water`, UE 5.7+ |
| [AI&nbsp;Niagara](../extensions/UnrealAINiagara/README.md) | Niagara systems, emitters, modules और collision | `Niagara`, `ChaosNiagara`, UE 5.7+ |
| [AI&nbsp;PCG](../extensions/UnrealAIPCG/README.md) | PCG graphs, volumes और generated scatters | `PCG`, UE 5.7+ |

Extensions आपके project में source से compile होते हैं (अगली बार editor खोलने पर, या `--build` के साथ तुरंत); installer उनके लिए ज़रूरी engine plugins enable कर देता है।

## Documentation

| पेज | इसमें क्या है |
|---|---|
| [docs/TOOLS.md](TOOLS.hi.md) | हर built-in और system tool, extensions, हर tool की visibility |
| [docs/rql.md](rql.md) | RQL query language: sources, members, mutations, play testing |
| [docs/CONFIGURATION.md](CONFIGURATION.hi.md) | पहली बार चलाना, update करना, environment variables, troubleshooting |
| [docs/EXTENSIONS.md](EXTENSIONS.md) | अपने खुद के RQL modules, tools, prompts और resources लिखें |
| [docs/RUNTIME.md](RUNTIME.hi.md) | Runtime (game के अंदर) mode, उसके security gates, shipped games से plugin को बाहर रखना |
| [cli/README.md](../cli/docs/README.hi.md) | `unreal-mcp-cli` command reference और exit codes |
| [docs/ARCHITECTURE.md](ARCHITECTURE.md) | आधिकारिक डिज़ाइन (IPC, registration, sidecar, UI, config) |
| [docs/RELEASING.md](RELEASING.md) | Maintainers के लिए: releases, CI runners, secrets |
| [docs/EXTENSION-MONOREPO.md](EXTENSION-MONOREPO.md) | Maintainers के लिए: extensions की testing और release |

## Repository की संरचना

| Path | यह क्या है |
|---|---|
| [`UnrealMCP/`](../UnrealMCP/) | Unreal plugin (C++): `UnrealMcpRuntime`, `UnrealMcpRql`, `UnrealMcpEditor` |
| [`bridge/`](../bridge/) | `unreal-mcp-bridge`, .NET 9 sidecar |
| [`cli/`](../cli/) | `unreal-mcp-cli`, npm package |
| [`extensions/`](../extensions/) | छह वैकल्पिक engine extensions, जिनकी अपनी releases हैं |
| [`samples/`](../samples/) | एक editor extension template और एक runtime (game के अंदर) tool sample |

## Links

- [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G) · [Issues](https://github.com/IvanMurzak/Unreal-MCP/issues)
- साथी projects: [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) · [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP)
- साझा हिस्से: [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) · [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) · [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet)
- [Model Context Protocol](https://modelcontextprotocol.io/)

## License

[Apache-2.0](../LICENSE) © Ivan Murzak
