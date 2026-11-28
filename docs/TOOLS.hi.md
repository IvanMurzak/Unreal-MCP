<b>[English](TOOLS.md) | [中文](TOOLS.zh-CN.md) | [日本語](TOOLS.ja.md) | [Español](TOOLS.es.md) | [فارسی](TOOLS.fa.md) | [हिन्दी](TOOLS.hi.md) | [العربية](TOOLS.ar.md)</b>

# Unreal-MCP tools

**UnrealMCP** editor plugin जिन tools को register करता है, उनकी आधिकारिक सूची। Unreal-MCP में **families में बँटे built-in tools** हैं जिन्हें आपका AI agent call कर सकता है, साथ ही **system tools** हैं जिन्हें वह call नहीं कर सकता (CLI और desktop app के लिए host की अंदरूनी व्यवस्था)। Tool ids kebab-case में होते हैं (`rql-query`, `blueprint-compile`), वही convention जो Unity-MCP और Godot-MCP में है। [Extensions](#extensions) और [आपके अपने plugins](EXTENSIONS.md) और tools जोड़ते हैं।

| Family | Tools |
| --- | --- |
| RQL data layer | `rql-query`, `rql-mutate`, `rql-schema` |
| Blueprint | `blueprint-*`: create, get, add-component, remove-component, add-variable, modify-variable, set-default, add-function, add-event, compile, spawn |
| Source / C++ | `source-*`: read, create-class, update, delete, list, compile |
| Screenshot | `screenshot-*`: viewport, game-view, camera, isolated |
| Fab | `fab-search`, `fab-library`, `fab-add`, `fab-sign-in` |
| Console | `console-run-command` |
| Python | `python-exec` |
| Tool list | `unreal-tool-list` |
| **System** (AI को नहीं दिखते) | `ping`, `unreal-skill-create`, `unreal-skill-generate`, `editor-quit` |

> **सच्चाई का स्रोत।** Standard tools: `UnrealMCP/Source/UnrealMcpEditor/Private/Tools/` में `UnrealMcp*Tools.cpp` (`UnrealMcpSkillTools.cpp` को छोड़कर) और `UnrealMcpRql` में `UnrealMcpRqlTools.cpp`। System tools: `UnrealMcpPingTool.cpp`, `UnrealMcpSkillTools.cpp`, `UnrealMcpEditorSession.cpp` और sidecar का `SkillGenerateTool.cs`। कोई registration बदले तो यह पेज update करें।

सभी standard tools **सिर्फ़ editor के लिए** हैं। Runtime (in-game) connection केवल `ping` और आपके game द्वारा register किए गए tools देता है: देखें [RUNTIME.md](RUNTIME.hi.md)।

## Families का विवरण

<details>
  <summary><b>RQL — data layer</b></summary>

हर operation के लिए अलग tool की जगह पूरे editor के लिए एक query language ([`docs/rql.md`](rql.md)): actors, components और assets खोजें, members पढ़ें, filter और aggregate करें, और एक ही call में उन्हें बदलें। यह पुराने actor, object, asset, level, editor-state, selection, console-log और reflection tools की जगह लेता है।

| Tool id | यह क्या करता है |
| --- | --- |
| `rql-query` | Live editor पर read-only RQL query (`actors | where $ is PointLight | select $ref Label Intensity`); compact table में results; `;` से अलग की गई कई queries एक ही call में |
| `rql-mutate` | `set` / `call` पर खत्म होने वाली RQL query (`... | set Location = "(0, 0, 100)"`, `editor | call SpawnActor("PointLight")`): या तो सब या कुछ नहीं, एक undo step, `dry_run` से preview; `;` से अलग की गई हर query एक अलग बदलाव है |
| `rql-schema` | Sources, functions, install किए गए extensions और editor world की classes; `type` किसी एक type के members और methods दिखाता है; `guide` install किए गए extension की गाइड लौटाता है (`{"guide": "landscape"}`) |

Reflected properties के अलावा RQL इन तक भी पहुँचता है: DataTable rows (`dataTables`, `Rows["Sword"].Damage`), text के रूप में gameplay tags (`gameplayTags`, `set Tags = ["Ability.Fire"]`), अपनी ini में save होने वाली Project Settings (`unreal.settings("Maps & Modes")`), Blueprint graphs (`EventGraph | call AddCallFunction("PrintString")`, pin connections, `call Compile()`), Widget Blueprint trees (`call AddWidget("Button", "Ok", parent)`) और Level Sequences (bindings, tracks, `call AddKey("Location.X", 30, 100)`)। Materials हर feature level और shader platform के compile errors और उन्हें इस्तेमाल करने वाले meshes के लिए ज़रूरी usage flags बताते हैं (`unreal.asset("/Game/M_Rock") | call Compile()`, `CompileErrors`, `MissingUsages`), जो `python-exec` का `recompile_material` नहीं बताता; compile न होने वाला material editor diagnostics note में भी दिखता है।

`unreal-rql` skill (जिसे `unreal-skill-generate` लिखता है) यह भाषा सिखाता है और बताता है कि install किए गए extensions क्या जोड़ते हैं।

</details>

<details>
  <summary><b>Blueprint family — Unreal की सबसे अहम surface</b></summary>

| Tool id | यह क्या करता है |
| --- | --- |
| `blueprint-create` | Parent `UClass` path से नई Blueprint class बनाता है |
| `blueprint-get` | LLM के देखने के लिए graph का सारांश (variables, components, functions/events, parent chain) |
| `blueprint-add-component` | Simple Construction Script के ज़रिए component जोड़ता है |
| `blueprint-remove-component` | SCS component हटाता है |
| `blueprint-add-variable` | Typed member variable जोड़ता है |
| `blueprint-modify-variable` | Member variable बदलता है |
| `blueprint-set-default` | CDO (class-default) property edit करता है |
| `blueprint-add-function` | Function stub जोड़ता है (entry/result nodes जुड़े हुए) |
| `blueprint-add-event` | Event stub जोड़ता/bind करता है (BeginPlay, Tick, input, …) |
| `blueprint-compile` | Blueprint compile करता है और **structured error/warning list** लौटाता है (AI का feedback loop) |
| `blueprint-spawn` | Blueprint का instance मौजूदा level में रखता है |

</details>

<details>
  <summary><b>Console family</b></summary>

| Tool id | यह क्या करता है |
| --- | --- |
| `console-run-command` | Console command / CVar चलाता है |

Logs, PIE state और selection RQL से मिलते हैं: `console | where Verbosity == "Error"`, `editor | set IsPlaying = true`, `editor | set Selection = [...]`। Play testing game को call के अंदर ही simulate करता है (deterministic, calls के बीच editor के tick होने से स्वतंत्र): `editor | call StartPlay()`, `editor | call AdvancePlay(1.5, 0, "W, LeftShift")` (keys को 1.5 s तक दबाए रखता है, और pawn की location, velocity, call में वह कितना चला, movement mode और लागू mapping contexts लौटाता है), `editor | call StopPlay()`; Enhanced Input extension `enhancedInput | call InjectInput(action, value, seconds)` जोड़ता है ([docs/rql.md](rql.md#play-testing))।

</details>

<details>
  <summary><b>Tool list family</b></summary>

| Tool id | यह क्या करता है |
| --- | --- |
| `unreal-tool-list` | हर tool की सूची, उनकी भी जो `tools/list` से छिपे हैं (hidden के रूप में चिह्नित; नाम से call करने पर वे फिर भी चलते हैं); regex `filter`, `includeDescription`, `includeInputs` |

</details>

<details>
  <summary><b>Python family</b></summary>

| Tool id | यह क्या करता है |
| --- | --- |
| `python-exec` | Editor में Python चलाता है (पूरा `unreal` API, पहले से import): सामान्य statements, आखिरी expression ही result है, `print` logs के रूप में लौटता है, errors `snippet.py` की lines बताते हैं; `include` `<Project>/.ai-game-dev/snippets` से helpers जोड़ता है: `.py` file code से पहले snippet के अपने namespace में चलती है (import नहीं होती), folder `sys.path` पर जाता है (उसके modules import होते हैं); `timeout` (seconds, default 30) बेकाबू snippet को रोकता है |

यह engine का PythonScriptPlugin (Unreal-MCP इसे enable करता है; सिर्फ़ editor में, packaged game में कभी नहीं) और `UnrealMCP/Content/Python` में vendored [Code-Exec](https://github.com/IvanMurzak/Code-Exec) runtime इस्तेमाल करता है। Blueprint graph की wiring Python को उपलब्ध नहीं है: उसके लिए Blueprint tools इस्तेमाल करें।

tool का पुराना नाम `python-execute` अगली कुछ releases तक `python-exec` के hidden alias के रूप में चलता रहेगा (पुराने नाम से calls काम करती रहेंगी, और `enabledTools` / `hiddenTools` में पुराना नाम `python-exec` के रूप में पढ़ा जाता है; agent का पुराने नाम वाला permission rule `python-exec` पर लागू नहीं होता); यह `tools/list` में नहीं है, इसकी कोई skill file नहीं है, और इसे हटा दिया जाएगा। `python-exec` इस्तेमाल करें।

</details>

<details>
  <summary><b>Fab family — Fab content खोजें और जोड़ें</b></summary>

| Tool id | यह क्या करता है |
| --- | --- |
| `fab-search` | [Fab](https://www.fab.com) marketplace में खोजता है (`query`, `listingType`, `freeOnly`, `sort`, paging); हर listing का uid, title, type, price, formats और Fab में दी गई Unreal Engine versions लौटाता है |
| `fab-library` | Signed-in user के अपने Fab assets की सूची, साथ में यह कि हर एक पहले से कहाँ import है; `filter` पूरी library में खोजता है |
| `fab-add` | User की अपनी listing download करता है और उसे Fab panel के *Add to Project* की तरह import करता है (Unreal Engine packs, `quality` के साथ Megascans, glTF/FBX); content folder लौटाता है, project में पहले से मौजूद listings छोड़ देता है; import के बनाए और unsaved छोड़े गए packages (Megascans / glTF) save करता है, और folder में अब भी unsaved packages बताता है (`requirements.savedPackages`, `requirements.unsavedPackages`; जाँचने को कोई package file न मिले तो कारण `requirements.packagesNotChecked` में बताता है); import को इस editor के सामने जाँचता है (newer engine से saved packages इसे error बनाते हैं; न पढ़ी जा सकने वाली या बहुत पुरानी files warnings हैं) और `requirements` लौटाता है: ज़रूरी plugins और वे enabled हैं या नहीं, startup cvars (Nanite Assemblies) और उनकी values, unresolved references; इस call में import हुए read-only engine base material (M_MS_Base) के instances को इस engine version के लिए fab-add की अपनी copy पर reparent करता है (`localBaseMaterial`; project में पहले से मौजूद listing या unsaved changes वाले instances कभी नहीं) |
| `fab-sign-in` | इस editor में user को Fab में sign in कराता है: browser में Epic का sign-in पेज खोलता है और user का इंतज़ार करता है (पहले से sign in हो तो तुरंत जवाब देता है, और इस editor में Epic की sign-in service शुरू न हुई हो तो तुरंत कारण के साथ)। Browser खोलने वाला एकमात्र Fab tool |

ये engine के अपने Fab plugin को चलाते हैं (UE 5.5 से default रूप से चालू): उसका Epic sign-in और उसके import workflows। Sign-in के बिना `fab-library` / `fab-add` तुरंत जवाब देते हैं और कभी browser नहीं खोलते: वे agent से कहते हैं कि chat में user से पूछे (`AskUserQuestion`, AI Game Dev App के desktop और CLI chats में एक blocking card) और `fab-sign-in` तभी call करे जब user मान जाए, ताकि कोई unattended run अपने-आप sign-in tab खुला न छोड़े; `fab-search` बताता है कि जोड़ने के लिए sign-in चाहिए या नहीं। Run से पहले sign in करने के लिए agent से `fab-sign-in` चलाने को कहें, या editor के toolbar से Fab खोलें। Epic इस computer पर sign-in याद रखता है, और editor शुरू होते ही उसे अपने-आप restore कर लेता है (बिना panel, बिना browser), इसलिए sign-in editor restart के बाद भी बना रहता है। खरीदना, या किसी मुफ़्त listing को library में जोड़ना, fab.com पर ही होता है। पूरे projects और engine code plugins Epic Games Launcher से install होते हैं, project में जोड़े नहीं जाते।

</details>

<details>
  <summary><b>Source / C++ family</b></summary>

| Tool id | यह क्या करता है |
| --- | --- |
| `source-read` | Project की C++ source file पढ़ता है (हिस्सों में) |
| `source-create-class` | नई C++ class का ढाँचा बनाता है (templates से header + cpp) |
| `source-update` | Source file edit करता है |
| `source-delete` | Source file delete करता है |
| `source-list` | Module की source files की सूची |
| `source-compile` | Project का C++ compile करता है (active हो तो Live Coding, वरना UBT) और structured error report देता है; नई classes या बदली हुई reflected declarations (`restartRequired`) पर बताता है कि editor को कैसे rebuild और restart करें |

सभी file operations `<Project>/Source/` तक सीमित हैं।

</details>

<details>
  <summary><b>Screenshot / viewport-capture family</b></summary>

| Tool id | यह क्या करता है |
| --- | --- |
| `screenshot-viewport` | Active editor viewport capture करता है; वैकल्पिक `savePath` PNG को project के अंदर भी लिखता है |
| `screenshot-game-view` | PIE / game view capture करता है; वैकल्पिक `savePath` ऊपर की तरह |
| `screenshot-camera` | `USceneCaptureComponent2D` के ज़रिए resolved camera actor से render करता है; वैकल्पिक `savePath` ऊपर की तरह |
| `screenshot-isolated` | किसी actor को अलग से render करता है (transient SceneCapture2D + show-only list); वैकल्पिक `savePath` ऊपर की तरह |

Captures base64 **PNG को MCP image content के रूप में** लौटाते हैं, ताकि LLM render को सीधे देख सके। हर capture call के समय एक नया frame render करता है, तब भी जब editor window background में हो या minimized हो, और उसका text बताता है कि frame ताज़ा है या नहीं और effects व animation tick हो रहे थे या नहीं। जब तक कोई AI agent जुड़ा है, plugin editor को background में (minimized होने पर भी) पूरी रफ़्तार से tick और render करता रखता है ("Use Less CPU when in Background" सिर्फ़ उसी session के लिए override होता है; आपकी saved preference नहीं बदलती)। Dimensions सीमित रहते हैं (default 1024, हर side पर अधिकतम 2048)। Pixel capture के लिए render करने वाला editor चाहिए: windowed, या `-RenderOffscreen` के साथ headless (`unreal-mcp-cli` का default)। `-nullrhi` में कुछ render नहीं होता, इसलिए ये tools ऐसा error लौटाते हैं जो `-nullrhi` और उसका समाधान बताता है।

</details>

<details>
  <summary><b>System tools — आपके AI को नहीं दिखते</b></summary>

ये **system tools** हैं: host की अंदरूनी व्यवस्था, जिसे `unreal-mcp-cli` और desktop app सीधे `POST /api/system-tools/<name>` पर चलाते हैं। ये जान-बूझकर MCP `tools/list` से बाहर रखे गए हैं, ताकि AI session में ये कभी न दिखें (और tokens खर्च न करें) — वही बँटवारा जो Unity और Godot अपने system tools के लिए करते हैं।

| Tool id | यह क्या करता है |
| --- | --- |
| `ping` | Liveness probe — plugin ⇄ sidecar ⇄ server chain का पूरा चक्कर लगाता है |
| `unreal-skill-create` | Plugin के editor module में C++ file के रूप में नया MCP tool बनाता है। नया tool call करने से पहले **editor rebuild ज़रूरी है** (Live Coding, या project rebuild करें) — result यह साफ़ बताता है। Plugin का source install चाहिए |
| `unreal-skill-generate` | Registered tools से हर `SKILL.md` को project-relative skills folder (default `.claude/skills`) में फिर से बनाता है |
| `editor-quit` | Path वाले dirty packages save करता है और CLI session द्वारा शुरू किया गया editor बंद करता है (`unreal-mcp-cli close --owned-only`)। `nonce` session lease से मेल न खाए तो मना कर देता है, इसलिए आपका खोला हुआ editor कभी बंद नहीं करता |

</details>

## Extensions

जब आपका project नीचे दिए किसी engine system का इस्तेमाल करे तो extension install करें, या [अपना खुद का बनाएँ](EXTENSIONS.md)। हर extension एक RQL module है: यह core `rql-query` / `rql-mutate` / `rql-schema` tools को बढ़ाता है और `unreal-rql` skill में अपनी गाइड जोड़ता है, इसलिए `tools/list` में अलग MCP tools नहीं आते।

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas "<UEProject>"
```

Extensions को Unreal Engine 5.7+ (Mesh Terrain, Mesh Terrain Water 5.8+), उसी project में Unreal MCP 0.26.0+ (ControlRig 0.28.0+, EnhancedInput 0.29.0+, MeshTerrain, MeshTerrainWater 0.31.3+) और C++ toolchain चाहिए: releases source के रूप में आती हैं और आपके project के लिए compile होती हैं (अगली बार editor खोलने पर, या `--build` से तुरंत)। Install id `com.ivanmurzak.unreal-ai-<id>` है, जिसमें id नीचे की table से है। Install options (`--version`, `--build`, offline `--source`): [Installing an extension into a project](EXTENSIONS.md#installing-an-extension-into-a-project)।

| Extension (id) | विवरण |
| --- | --- |
| **[AI&nbsp;Control&nbsp;Rig](../extensions/UnrealAIControlRig/README.md)**<br>`control-rig` | Bones, controls और two-bone IK solver के साथ rigs बनाएँ; Control Rig blueprints और components को query करें |
| **[AI&nbsp;Enhanced&nbsp;Input](../extensions/UnrealAIEnhancedInput/README.md)**<br>`enhanced-input` | Input Actions और Mapping Contexts को query करें, actions और contexts बनाएँ, keys map करें |
| **[AI GAS](../extensions/UnrealAIGAS/README.md)**<br>`gas` | C++ के बिना Gameplay Ability System authoring: effects (cost, cooldown, damage, regeneration), ability Blueprints, तैयार Health / Mana / Stamina attribute set के साथ ability system components, input bindings, Play In Editor checks |
| **[AI Landscape](../extensions/UnrealAILandscape/README.md)**<br>`landscape` | Landscape और Water: landscapes बनाएँ, sculpt और paint करें; oceans, lakes और rivers बनाएँ; landscapes, water bodies और water zones को query करें |
| **[AI Mesh&nbsp;Terrain](../extensions/UnrealAIMeshTerrain/README.md)**<br>`mesh-terrain` | Experimental, UE 5.8+: Mesh Terrain (Mesh Partition): terrains बनाएँ (flat या heightmap से); modifiers query करें, जोड़ें, बदलें, क्रम बदलें और हटाएँ; sculpt और paint करें; सतह की ऊँचाई, slope और paint channels; rebuild करें और build पूरा होने तक प्रतीक्षा करें |
| **[AI Mesh&nbsp;Terrain&nbsp;Water](../extensions/UnrealAIMeshTerrainWater/README.md)**<br>`mesh-terrain-water` | Experimental, UE 5.8+: Mesh Terrain पर Water plugin के bodies (MeshPartitionWater): झीलें और नदियाँ जिनके water modifiers terrain को काटते हैं, ocean; attach, detach और delete; water zone; हर body की तैयारी और समस्याएँ |
| **[AI Niagara](../extensions/UnrealAINiagara/README.md)**<br>`niagara` | Niagara VFX: systems और उनके emitters को query करें, systems को level में रखें |
| **[AI PCG](../extensions/UnrealAIPCG/README.md)**<br>`pcg` | Procedural Content Generation: PCG graphs बनाएँ (nodes, connections, settings, parameters), उन्हें volumes पर रखें, scatters generate करें (forests, rocks, props) और counts पढ़ें |

## हर tool की visibility

हर tool के लिए **MCP Tools** window में एक **Visible** checkbox है — यह standalone **MCP Tools** tab है (editor के **Tools** menu में registered)। Window हर tool का title, family और description दिखाती है, साथ में "N / M tools visible" सारांश line। **Visible** सिर्फ़ यह तय करता है कि tool **MCP `tools/list` में listed** है या नहीं, यानी AI agents उसे देखते हैं या नहीं। यह access control नहीं है: छिपा हुआ tool भी client के नाम से call करने (`tools/call`) पर चलता है, और `unreal-tool-list` छिपे tools को भी (hidden के रूप में चिह्नित) दिखाता है ताकि agent उन्हें ढूँढ सके।

दो settings लागू होती हैं (देखें [ARCHITECTURE](ARCHITECTURE.md) §7/§8):

- **`enabledTools` whitelist** (`UNREAL_MCP_TOOLS` से override हो सकती है) तय करती है कि tool **serve होगा या नहीं** — खाली न हो तो सिर्फ़ listed tools serve होते हैं, और बाकी कोई tool न listed होता है न call हो सकता है (`Execute()` उसे मना कर देता है); खाली का मतलब है "कोई filter नहीं";
- **`hiddenTools` list** — UI में आप जो Visible checkboxes बदलते हैं — serve हो रहे tool को `tools/list` से छिपाती है।

दोनों editor sessions के पार save रहती हैं और extension hot-reload के बाद भी बनी रहती हैं (दोबारा registered tool पुरानी setting ही लेता है, इसलिए rebuild कभी वह tool नहीं दिखाता जिसे आपने छिपाया था)। पुराने plugin version की config file भी काम करती रहती है: उसकी `disabledTools` list को `hiddenTools` की तरह पढ़ा जाता है, और अगली save `hiddenTools` लिखती है।

सभी connection settings एक ही **AI Game Developer** main window के Connection section में हैं (कोई अलग Settings tab या Project-Settings पेज नहीं है — Unity-MCP के बराबर)। **MCP Prompts** और **MCP Resources** windows जुड़ी हुई हैं, पर इस release में खाली आती हैं — हर एक हल्का-सा खाली-स्थिति संदेश दिखाती है ("N / M visible" सारांश सिर्फ़ Tools window में है)।
