<b>[English](CONFIGURATION.md) | [中文](CONFIGURATION.zh-CN.md) | [日本語](CONFIGURATION.ja.md) | [Español](CONFIGURATION.es.md) | [فارسی](CONFIGURATION.fa.md) | [हिन्दी](CONFIGURATION.hi.md) | [العربية](CONFIGURATION.ar.md)</b>

# Configuration, updates और troubleshooting

Editor को कैसे connect करें, plugin को कैसे up to date रखें, environment variables से उसे कैसे tune करें, और आम समस्याएँ कैसे ठीक करें। Install के steps [README](README.hi.md) में हैं; इस सबके पीछे का डिज़ाइन [ARCHITECTURE.md](ARCHITECTURE.md) में है।

- [पहली बार चलाना](#पहली-बार-चलाना)
- [Plugin को update करना](#plugin-को-update-करना)
- [Environment variables](#environment-variables)
- [Troubleshooting](#troubleshooting)

## पहली बार चलाना

1. Editor के **Tools** menu से **AI Game Developer** main window खोलें (tab, Tools menu category के तहत registered है)।
2. **Connection mode चुनें:**
   - **Cloud** (default) — [ai-game.dev](https://ai-game.dev) से connect होता है। OAuth **device-code flow** शुरू करने के लिए **Authorize** पर click करें: window एक verification URL और एक छोटा user code दिखाती है; URL खोलें, code डालें, approve करें, और editor authorization पूरा कर लेता है। Save किया हुआ cloud token हटाने के लिए **Revoke** इस्तेमाल करें।
   - **Custom** — आपके द्वारा चलाए गए local `gamedev-mcp-server` (या किसी भी compatible server) से connect होता है। Server URL डालें और अपने AI client को उस पर point करें। Server खुद चलाएँ, या editor को उसे start और stop करने दें (`UNREAL_MCP_START_SERVER`, सिर्फ़ Custom mode + http; देखें [Environment variables](#environment-variables))।
3. **Connection** section में एक status dot, एक status label और एक Connect / Disconnect / Stop बटन होता है; bridge status `Running (restarts: N)` या `Stopped` दिखाता है। इनसे पक्का करें कि sidecar चल रहा है।
4. अपने AI client (Claude Code, Cursor, AI Game Developer app, …) को server पर point करें। **AI agents** section अभी जुड़े हुए agents की सूची दिखाता है; MCP client config लिखने के लिए `unreal-mcp-cli setup-mcp` इस्तेमाल करें।

Connection settings `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` में save होती हैं (`Saved/` हर UE template में gitignored है, इसलिए default रूप से tokens कभी VCS में नहीं पहुँचते)।

> **Pinned MCP client URL।** `unreal-mcp-cli setup-mcp <agent>` ऐसा MCP client config लिखता है जो **project-pinned** cloud URL `<base>/mcp/p/<pin>` पर point करता है, ताकि agent **इसी** project के editor तक पहुँचे, तब भी जब आपका account कई editors चलाता हो। इसकी जगह सादा `<base>/mcp` URL लिखने के लिए `--no-pin` दें। Pin सिर्फ़ routing path segment है — OAuth resource `<base>/mcp` ही रहता है।

> **Project key।** जब यह machine signed in हो (`unreal-mcp-cli login`, या editor का Sign in), तो Cloud config में हर agent के लिए `Authorization: Bearer agd_pk_…` भी होता है — इस project के pin से बँधी, कभी expire न होने वाली, revoke की जा सकने वाली key — ताकि किसी agent को अपना OAuth न करना पड़े। Key `~/.ai-game-dev/project-keys.json` में cache होती है और दोबारा इस्तेमाल होती है। सिर्फ़ URL वाले config के लिए `--oauth` दें (agent खुद sign in करता है), या नई key बनाने और पुरानी revoke करने के लिए `--regenerate-key` (editor के AI agent panel में **Regenerate key** बटन है)। Signed in नहीं हैं, या server key नहीं बना सकता ⇒ सिर्फ़ URL वाला config।

## Plugin को update करना

Update के बाद हमेशा **नया** code ही चलना चाहिए। खतरा UE के incremental compiler से है: अगर plugin source बदल जाए (नई `.cpp` files, नया module) लेकिन पुराना `UnrealMCP/Intermediate/` build cache बचा रहे, तो UE पुरानी module file-list के आधार पर आंशिक recompile कर सकता है और चुपचाप आपको पुराने/अधूरे code पर छोड़ सकता है। हर channel इसे अलग तरह से संभालता है:

- **Fab / Epic Marketplace (listing live होने के बाद) → अपने-आप।** Epic Games Launcher precompiled binaries को उसी जगह बदल देता है; न कुछ compile करना, न कोई cache साफ़ करना।
- **`unreal-mcp-cli update` → अपने-आप clean rebuild।** `update` plugin source दोबारा copy करता है और **default रूप से** installed plugin का पुराना `Intermediate/` और C++ `Binaries/` delete कर देता है, ताकि अगली बार editor launch होने पर UE clean compile करे — कोई manual step नहीं। `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` के तहत bundled sidecar bridge जस का तस रहता है: release-source installs उसे `Source/ThirdParty/...` से refresh करते हैं, जबकि repo/dev installs ज़रूरत पड़ने पर पहले से bundled copy बचाए रखते हैं। Dev **junction** installs कभी साफ़ नहीं किए जाते (इससे आपके live source tree के outputs मिट जाते)। Cache मिटाने से बचने के लिए `--no-clean` दें।

  ```bash
  unreal-mcp-cli update <YourProject>            # default: clean rebuild on version change
  unreal-mcp-cli update <YourProject> --force    # re-copy even when versions match
  unreal-mcp-cli update <YourProject> --no-clean # keep the existing build cache
  unreal-mcp-cli update <YourProject> --no-extensions # leave the installed extensions as they are
  unreal-mcp-cli update <YourProject> --no-self  # leave the CLI as it is
  unreal-mcp-cli update <YourProject> --check    # only report; exit code 2 when something is outdated
  ```

- **Manual copy → cache खुद साफ़ करें।** अगर आप `<YourProject>/Plugins/UnrealMCP/` को हाथ से overwrite करते हैं, तो **पहले editor बंद करें**, `<YourProject>/Plugins/UnrealMCP/Intermediate/` और C++ `Binaries/` delete करें (bundled bridge हो तो `Binaries/ThirdParty/` रखें), फिर दोबारा launch करें ताकि UE साफ़ तरीके से recompile करे।

## Environment variables

Plugin configuration को इस प्राथमिकता से पढ़ता है: **process env → `<Project>/.env` → config file → built-in defaults**। पहचाने जाने वाले `UNREAL_MCP_*` variables (`UnrealMCP/Source/UnrealMcpRuntime/Private/Config/UnrealMcpConfig.cpp` में परिभाषित; `UNREAL_MCP_CREDENTIALS_DIR` को bridge पढ़ता है, `UNREAL_MCP_SERVER_PATH` को CLI):

| Variable | उद्देश्य |
| --- | --- |
| `UNREAL_MCP_CONNECTION_MODE` | `Cloud` या `Custom`। **Default `Cloud` है।** |
| `UNREAL_MCP_HOST` | Server URL (Custom mode), जैसे `http://localhost:<port>` |
| `UNREAL_MCP_CLOUD_URL` | Cloud backend URL (default ai-game.dev) |
| `UNREAL_MCP_TOKEN` | Auth token (sidecar IPC token / server token)। **Secret — कभी commit न करें।** |
| `UNREAL_MCP_AUTH_OPTION` | `none` या `oauth` (local server auth) |
| `UNREAL_MCP_KEEP_CONNECTED` | Connected state को save/restore करता है |
| `UNREAL_MCP_TOOLS` | Enabled-tools override (whitelist; खाली = कोई filter नहीं) |
| `UNREAL_MCP_START_SERVER` | `true`: bridge के project का port बताते ही editor local `gamedev-mcp-server` खुद start करता है (सिर्फ़ Custom mode + http; `startServer` के रूप में save होता है) और exit पर उसे stop करता है। `unreal-mcp-cli exec` / `open --headless` इसे set करते हैं। |
| `UNREAL_MCP_TRANSPORT` | `stdio` या `http` |
| `UNREAL_MCP_LOG_LEVEL` | Log verbosity |
| `UNREAL_MCP_TOOL_TIMEOUT` | Editor में एक tool call कितनी देर चल सकता है, milliseconds में (default `300000`; `toolTimeoutMs` के रूप में save होता है)। लंबे `python-execute` snippets या assets के काम के लिए इसे बढ़ाएँ। Editor इसमें 10 s जोड़कर अपने start किए local server को `tool-timeout` के रूप में देता है; पुराना server (McpPlugin 8.11.0 से पहले) इस value के बावजूद tool calls को 5 min तक सीमित रखता है ([ARCHITECTURE §4](ARCHITECTURE.md))। जो call editor के शुरू करने से पहले time out या cancel हो जाए, उसे छोड़ दिया जाता है, बाद में नहीं चलाया जाता। |
| `UNREAL_MCP_BRIDGE_PATH` | Sidecar binary का path — **dev/CI override**; bundled binary पर भारी पड़ता है (§6)। Packaged release bundled sidecar को अपने-आप ढूँढ लेती है, इसलिए end users इसे कभी set नहीं करते; from-source builds इसे इस्तेमाल करते हैं। |
| `UNREAL_MCP_CREDENTIALS_DIR` | Machine credential store का absolute folder, जिसे bridge `~/.ai-game-dev` की जगह पढ़ता (और refresh करता) है; editor के process environment में set होता है (`.env` से नहीं पढ़ा जाता) — यह path है, token कभी नहीं; relative value log होकर ignore हो जाती है। उन development/test editors के लिए जिनका launcher किसी दूसरे profile से sign in करता है (AI Game Dev App का local dev stand); Godot के `GODOT_MCP_CREDENTIALS_DIR` का जुड़वाँ। |
| `UNREAL_MCP_SERVER_PATH` | Local `gamedev-mcp-server` binary का path (इसे `unreal-mcp-cli` पढ़ता है, plugin नहीं) — server download + version check छोड़ देता है (§6)। |
| `AIGD_SESSION_IDLE_SECONDS` | `unreal-mcp-cli` इसे अपने start किए editor पर set करता है: इतने seconds तक कोई tool call न हो (चल रहे calls, play-in-editor, asset compilation और user input को activity माना जाता है) तो editor path वाले dirty packages save करके बंद हो जाता है। सिर्फ़ `AIGD_SESSION_OWNER=cli` और nonce के साथ ही सक्रिय होता है, और सिर्फ़ तब तक जब तक project lease में यही editor दर्ज हो; unset = कभी नहीं। |
| `AIGD_SESSION_OWNER` / `AIGD_SESSION_NONCE` | `cli` और `<project>/.ai-game-dev/session.json` का nonce: बताते हैं कि editor किसी CLI session ने start किया है। सिर्फ़ ऐसा editor `editor-quit` स्वीकार करता है, और exit पर lease हटा देता है अगर उसमें अब भी उसका nonce हो। |

> **`.env` कभी commit न करें।** Project-root की `.env` में `UNREAL_MCP_TOKEN` हो सकता है, और UE project templates कोई `.gitignore` नहीं देते। `unreal-mcp-cli configure` target project की `.gitignore` में `.env` जोड़ देता है; इस repo का अपना scaffold पहले से इसे gitignore करता है। Sidecar IPC token stdin से जाता है (argv से कभी नहीं) और कभी log नहीं होता।

## Troubleshooting

- **Local server से `No connected clients. Retrying [1..10]` और फिर HTTP 500।** Connection mode default रूप से `Cloud` रहा, इसलिए sidecar ने आपके local server की जगह ai-game.dev को dial किया। `UNREAL_MCP_CONNECTION_MODE=Custom` set करें (env, `.env`, या UI toggle)।
- **Boot पर `[Unreal-MCP] plugin loaded` line नहीं आती।** Editor module load नहीं हुआ — Output Log में `StartupModule` error या खराब `.uplugin` देखें।
- **REST passthrough से call करने पर tool body `'x' is required.` लौटाता है।** `-H "Content-Type: application/json"` भेजें; इसके बिना server JSON body छोड़ देता है।
- **Screenshot tools `Screenshot tools are unavailable: this editor runs with the null RHI (-nullrhi)` लौटाते हैं।** `-nullrhi` में कुछ render नहीं होता। Editor को इसके बिना restart करें: windowed, या `-RenderOffscreen` के साथ headless, जो screenshots render करता है (`unreal-mcp-cli` के साथ `--nullrhi` न दें)।
- **Sidecar नहीं है / sidecar बार-बार restart होता है।** **Connection** section में bridge status देखें (`Running (restarts: N)` / `Stopped`)। अगर log में `no sidecar binary resolved for rid <rid> …` दिखे, तो न bundled binary, न downloaded binary, न `UNREAL_MCP_BRIDGE_PATH` मिला। Fab install में bridge हाथ से install करना पड़ता है: **Get bridge** release पेज खोलता है, अपने platform की zip को उस folder में extract करें जो **Open bridge folder** दिखाता है, फिर **Connect** पर click करें (Fab build खुद कभी कोई program download या start नहीं करता); CLI / packaged release में यह bundled होता है, तो वहाँ इसका मतलब है कि plugin आपके platform के bridge के बिना package हुआ (सही build फिर से install करें); from-source build में `UNREAL_MCP_BRIDGE_PATH` set करें या source से bridge build करने के लिए `unreal-mcp-cli bootstrap-local` चलाएँ ([ARCHITECTURE](ARCHITECTURE.md) §6.3, §6.8)।
- **Ports।** IPC `30000–39999` में हर project के लिए तय port इस्तेमाल करता है और टकराव होने पर आगे probe करता है (फिर ephemeral port); local server `20000–29999` में तय hash port इस्तेमाल करता है, बिना probing के — CLI कोई config पढ़े बिना वही नंबर निकालता है, और server ठीक माँगे गए port पर bind करता है। सटीक नंबर debugging की सुविधा भर है, ज़रूरत नहीं।
- **Logs।** Main window का **Open log file** action, एक RQL query (`console | where Verbosity == "Error"`), या editor Output Log (`LogUnrealMcp` category) इस्तेमाल करें।
