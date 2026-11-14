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

**स्थिति:** [latest release](https://github.com/IvanMurzak/Unreal-MCP/releases/latest) (plugin, sidecar और CLI का version एक ही होता है) · Unreal Engine 5.5–5.8 · sidecar Windows x64, macOS (Intel और Apple silicon) और Linux x64 के लिए build होता है · हर release Windows (UE 5.5–5.8) और macOS (UE 5.7, 5.8) पर test होता है · [Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) और [GitHub Releases](https://github.com/IvanMurzak/Unreal-MCP/releases) पर उपलब्ध।

## विशेषताएँ

✅ **C++ लिखना** — classes बनाएँ, source edit करें, compile करें; errors file, line और severity के साथ लौटते हैं\
✅ **Blueprints** — बनाएँ, edit करें और compile करें, ऐसे structured errors के साथ जिन्हें agent ठीक कर सके\
✅ **RQL data layer** — actors, assets और levels को एक ही भाषा में query करें और बदलें ([RQL](rql.md))\
✅ **Visual feedback** — viewport, game-view, camera और isolated-actor screenshots\
✅ **Python** — पूरे `unreal` API के साथ editor Python चलाएँ\
✅ **Fab** — Fab में खोजें, अपनी library देखें, अपना खरीदा content जोड़ें\
✅ **Extensions** — GAS, Niagara, PCG, Landscape, Control Rig, Enhanced Input\
✅ **आपके अपने tools** — किसी भी plugin से tools, prompts और resources जोड़ें ([guide](EXTENSIONS.md))\
✅ **Runtime mode** — चाहें तो चलते हुए game को control करें ([docs/RUNTIME.md](RUNTIME.hi.md))\
✅ **Cloud या local** — तैयार `ai-game.dev`, या self-hosted `gamedev-mcp-server`

**Coverage:** Unreal Editor में आप जो कुछ कर सकते हैं, लगभग वह सब agent भी कर सकता है। Tool groups: RQL data layer, Blueprints, C++ source, screenshots, Fab, console, editor Python और tool list, साथ में extensions। सभी tools: [docs/TOOLS.md](TOOLS.hi.md)।

**Efficiency:** tools design से ही compact हैं, इसलिए agent को हर task के लिए कम calls और कम tokens चाहिए, और वह तेज़ और सस्ते में काम करता है। RQL data layer के लिए यह RQL के अपने evaluation में मापा गया है (एक sandbox game world, वही tasks, model और grader): पारंपरिक find / dump-as-JSON / set reflection tools की तुलना में RQL को हर task में लगभग 9–18× कम tool calls और 17–19× कम tokens लगे ([results](https://github.com/IvanMurzak/RQL/blob/main/RESULTS.md#33-rql-vs-conventional-reflection-tools))।

### आप क्या कह सकते हैं

| अपने agent से कहें | वह कौन-से tools इस्तेमाल करता है |
| --- | --- |
| "C++ में एक `AHealthPickup` actor class जोड़ो, compile करो और errors ठीक करो" | `source-create-class`, `source-update`, `source-compile` |
| "Overlap पर खुलने वाला door Blueprint बनाओ, compile करो, level में रखो" | `blueprint-*` |
| "इस level में पेड़ गिनो और हर point light को 1 m ऊपर करो" | `rql-query`, `rql-mutate` |
| "Player camera से screenshot लो और lighting जाँचो" | `screenshot-*` |
| "Fab पर एक free rock pack ढूँढो और project में जोड़ो" | `fab-search`, `fab-add` |
| "चुने हुए assets को Python script से एक साथ rename करो" | `python-execute` |

**C++ एक first-class loop है।** `source-compile` Live Coding चालू होने पर उससे, वरना UnrealBuildTool से build करता है, और हर diagnostic उसकी file, line और severity के साथ लौटाता है, इसलिए agent build log पढ़े बिना errors ठीक कर लेता है। जब किसी बदलाव के लिए full build और editor restart चाहिए (नई classes, बदली हुई `UCLASS` / `UPROPERTY` / `UFUNCTION` declarations), तो वह यह बताता है। File operations `<Project>/Source/` के अंदर ही रहते हैं। Terminal से `unreal-mcp-cli compile` बिना editor के `file:line:col` diagnostics print करता है।

**RQL editor से ही सीखा जाता है:** `rql-schema` हर source, type और member की सूची देता है, और `unreal-rql` skill recipes के साथ syntax सिखाती है (`unreal-mcp-cli setup-skills` उसे आपके agent के लिए लिखता है)।

## आवश्यकताएँ

- **Unreal Engine 5.5 – 5.8** (release pipeline हर एक को validate करती है)।
- **एक C++ project**, क्योंकि plugin source के रूप में आता है और Unreal आपके platform के C++ toolchain से आपके project के लिए उसके modules compile करता है। Blueprint-only project एक कदम में C++ project बन जाता है: editor में **Tools → New C++ Class…** से कोई भी class बनाएँ। नए project के लिए `unreal-mcp-cli create-project ./MyGame` एक minimal C++ project बनाता है।
- **.NET install की ज़रूरत नहीं।** MCP host एक self-contained .NET program (`unreal-mcp-bridge`) है जो आपके platform के लिए plugin के अंदर bundled है; plugin उसे start और restart करता है। (Fab build में कोई executable नहीं होता; उसका **Get bridge** button bridge को एक बार install करता है।) सिर्फ़ bridge को source से build करने के लिए .NET 9 SDK चाहिए।
- **Node.js 22.14+**, सिर्फ़ `unreal-mcp-cli` के लिए (एक npm package; 22.14 उसका minimum Node version है)।
- **Windows या macOS** (हर release दोनों पर test होता है)। **Linux x64:** sidecar build और ship होता है, लेकिन Linux पर कोई release test नहीं चलता।

## जल्दी शुरू करें

**1. Plugin install करें**, इनमें से एक तरीके से:

- **[Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)** (terminal की ज़रूरत नहीं): **AI Game Developer** को अपने engine में जोड़ें, **Edit → Plugins** में enable करें, अपना project खोलें।
- **दो commands** (Node.js 22.14+):

```bash
npx unreal-mcp-cli install-plugin ./MyGame   # plugin + bundled bridge into MyGame/Plugins
npx unreal-mcp-cli open ./MyGame             # build if needed and open the editor
```

**2. Editor में connect करें:** **Tools → AI Game Developer** खोलें।

- सिर्फ़ Fab install: एक बार **Get bridge** पर click करें (Fab build में executables नहीं होते, इसलिए यह `unreal-mcp-bridge` sidecar install करता है)।
- **Cloud** (default): **Authorize** पर click करें और browser में sign-in approve करें। **Local**: **Custom** पर switch करें और **MCP server** के पास **Start** पर click करें; editor checksum-verified `gamedev-mcp-server` download करता है (Fab install में यह server हाथ से install करें: log download का पता बताता है)।
- **AI agent** के नीचे अपना agent चुनें (Claude Code, Cursor, Codex, …) और **Configure** पर click करें।

**3. अपने agent से कहें:** *"एक पंक्ति में तीन cubes और उनके ऊपर एक point light बनाओ, फिर एक screenshot लो।"*

Local या Cloud? दोनों पूरी तरह supported हैं; बाद में बदल सकते हैं। **Cloud default है** क्योंकि इसमें कोई port खोलना नहीं पड़ता और दूसरे programs के साथ port conflicts नहीं होते; यह connection free है। **Local** बिना account या internet के चलता है, offline और air-gapped machines के लिए।

| | **Local** | **Cloud** |
| --- | --- | --- |
| Account | कोई नहीं | ai-game.dev sign-in |
| MCP server | आपकी machine पर `gamedev-mcp-server`, editor या agent द्वारा started | ai-game.dev relay |
| Traffic | `localhost` पर ही रहता है | ai-game.dev से होकर जाता है ([वह क्या log करता है](#what-the-cloud-relay-sees)) |
| Auth | loopback पर कोई नहीं, या एक shared token (एक flag) | OAuth या per-project key |
| किसके लिए अच्छा | offline और air-gapped काम, CI, private projects | कोई खुला port या port conflict नहीं; कई projects और machines के लिए एक account |

<details>
<summary><b>सब कुछ terminal से</b> (CLI: Cloud, Local, token auth)</summary>

<p align="center">
  <img src="img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, editor खोलें" width="100%"/>
</p>

**Cloud** (plugin का default mode):

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

**Local** (बिना account):

```bash
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin ./MyGame
unreal-mcp-cli configure -p ./MyGame --mode Custom                 # plugin: local server
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --transport stdio # agent starts it
unreal-mcp-cli open ./MyGame
```

`setup-mcp --transport stdio` checksum-verified `gamedev-mcp-server` release को project में download करता है और agent को उस पर point करता है। दोनों तरफ़ `localhost` पर एक ही per-project port इस्तेमाल होता है।

**Token auth चालू करें (local):** `configure` line में flags जोड़ें, फिर `setup-mcp` दोबारा चलाएँ; इसके बाद plugin और server दोनों secret माँगते हैं।

```bash
unreal-mcp-cli configure -p ./MyGame --mode Custom --auth required --token <secret>
```

Local में default रूप से auth क्यों नहीं: server सिर्फ़ loopback पर bind होता है और non-local browser origins से आई requests ठुकरा देता है, इसलिए सिर्फ़ आपकी अपनी machine के programs उस तक पहुँच सकते हैं। जब आप उस machine के हर program या user पर भरोसा न करें, तब token इस्तेमाल करें। Secret project की `.env` (जिसे `configure` gitignore में जोड़ता है) और agent की config file में जाता है, इसलिए उस file को version control से बाहर रखें।

</details>

<a id="what-the-cloud-relay-sees"></a>**Cloud relay क्या देखता है।** Cloud mode में tool calls ai-game.dev के MCP relay से होकर जाते हैं। Relay stateless है: वह tool calls को आपके AI client और editor के बीच route करता है, उनके content को न inspect करता है न store करता है, और आपकी conversations, source code, project files, assets या scene data collect नहीं करता। वह MCP event metadata record करता है (event type, timing, status, session identifiers, tool, prompt या resource का नाम, और error details); request logs और MCP events एक rolling 90-day window के भीतर इस्तेमाल होते हैं। Details: [Privacy Policy](https://ai-game.dev/legal/privacy)।

`install-plugin` CLI version से मेल खाने वाला signed plugin source release download करता है।

Install करने के दूसरे तरीके:

- **Fab-format zip**: [Fab listing](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) के अलावा, हर GitHub release में executables के बिना एक Fab-format build (`unreal-mcp-plugin-fab-<version>.zip`) भी होता है; दोनों में से किसी के साथ, window का **Get bridge** button sidecar को एक बार install करने में मदद करता है।
- **Manual**: [`UnrealMCP/`](../UnrealMCP/) को `<YourProject>/Plugins/UnrealMCP/` में copy करें। Source checkout में bundled sidecar नहीं होता: `unreal-mcp-cli bootstrap-local` चलाएँ या `UNREAL_MCP_BRIDGE_PATH` set करें।

Update करना (`unreal-mcp-cli update`), connection modes, environment variables और troubleshooting: [docs/CONFIGURATION.md](CONFIGURATION.hi.md)।

### Agent setup

`unreal-mcp-cli setup-mcp <agent> -p ./MyGame` आपके लिए config लिखता है (`setup-mcp --list` सभी supported agents दिखाता है)। वह क्या लिखता है:

<details>
<summary><b>Claude Code</b>: <code>.mcp.json</code></summary>

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

`<pin>` इस project के editor तक route करता है। Machine signed in हो तो header इस project की key होता है; `--oauth` सिर्फ़ URL वाला config लिखता है और agent खुद sign in करता है। `--transport stdio` के साथ entry local server चलाती है: `"command"` `<project>/Intermediate/UnrealMCP/server/` के नीचे `gamedev-mcp-server` है, `"args"` `port=<port>`, `client-transport=stdio` और `authorization=none` set करते हैं।

</details>

<details>
<summary><b>Cursor</b>: <code>.cursor/mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp cursor -p ./MyGame
```

Claude Code जैसी ही `mcpServers` → `unreal-mcp` entry: URL के साथ `"type": "http"`, या local server के लिए `command` और `args` के साथ `"type": "stdio"`।

</details>

<details>
<summary><b>Codex</b>: <code>.codex/config.toml</code></summary>

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

Project key होने पर section में `Authorization` header वाला `http_headers` भी होता है।

</details>

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

## Security

- **Plugin ↔ sidecar:** plugin एक loopback TCP port पर सुनता है; sidecar stdin से भेजे गए one-shot token के साथ उससे जुड़ता है (कभी command line पर नहीं, कभी log में नहीं)।
- **Local server:** `gamedev-mcp-server` default रूप से loopback पर bind होता है और जिस request का `Origin` allowed नहीं, उसे `403` देता है (DNS-rebinding से बचाव)। Loopback पर plugin उसे बिना authentication start करता है, जब तक आप token न चुनें (`UNREAL_MCP_AUTH_OPTION`, `UNREAL_MCP_TOKEN`)। Server options: [hosting और authentication](https://github.com/IvanMurzak/GameDev-MCP-Server/blob/main/docs/hosting.md)।
- **Cloud:** agent OAuth से sign in करता है या per-project key इस्तेमाल करता है, जिसे आप `setup-mcp --regenerate-key` से बदल सकते हैं। Relay stateless है और tool calls का content store नहीं करता; वह MCP event metadata record करता है ([details](#what-the-cloud-relay-sees))।
- **Tools क्या कर सकते हैं:** `<Project>/Source/` के नीचे files edit करना, console commands और editor Python चलाना, कोई भी reflected property बदलना।
- **उन पर नियंत्रण:**
  - **Whitelist** (hard limit): `UNREAL_MCP_TOOLS` सिर्फ़ सूची वाले tools देता है; कोई और tool न list होता है, न call हो सकता है।
  - **Hide** (soft): **MCP Tools** window का **Visible** checkbox tool को `tools/list` से हटाता है, लेकिन नाम से call करने वाला client उसे फिर भी चला सकता है।
  - **सीमित बदलाव:** C++ file tools `<Project>/Source/` के अंदर रहते हैं; एक `rql-mutate` call all-or-nothing है, एक undo step, `dry_run` से preview हो सकता है, और 200 से ज़्यादा targets पर मना कर दिया जाता है जब तक call `max_targets` न बढ़ाए (1000 तक); `python-execute` `timeout` के बाद रुक जाता है (default 30 s); हर tool call की time limit है (`UNREAL_MCP_TOOL_TIMEOUT`, default 5 min)।
  - **AI के लिए नहीं:** system tools (`ping`, skill generation, `editor-quit`) `tools/list` में नहीं होते, और `editor-quit` तब तक मना करता है जब तक CLI session का nonce match न करे।
  - Details: [docs/TOOLS.md](TOOLS.hi.md)।
- **Runtime mode default रूप से बंद है:** game कभी अपने-आप connect नहीं होता। इसके लिए `bRuntimeMcpEnabled` setting (default बंद) और एक explicit `Connect()` call चाहिए, यह सिर्फ़ loopback host स्वीकार करता है जब तक आप remote host की अनुमति न दें, और Shipping builds में मना हो जाता है जब तक compile में शामिल न किया जाए। `TargetDenyList` से plugin को packaged games से बाहर रखें ([docs/RUNTIME.md](RUNTIME.hi.md))।

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

**Game developers के लिए free, commercial games समेत।** Unreal MCP [Apache-2.0](../LICENSE) © Ivan Murzak है। दो bundled components, RQL और Code-Exec, [PolyForm Shield 1.0.0](https://polyformproject.org/licenses/shield/1.0.0) license इस्तेमाल करते हैं, जो सिर्फ़ उन companies को रोकता है जो game development के लिए competing AI tools बनाती हैं; आप अपने games बनाने, ship करने और बेचने के लिए यह सब इस्तेमाल कर सकते हैं। Notices: [`UnrealMCP/ThirdPartyNotices.txt`](../UnrealMCP/ThirdPartyNotices.txt)।
