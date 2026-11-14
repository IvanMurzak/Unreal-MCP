<div align="center">

<img src="img/readme/hero.svg" alt="Unreal MCP — دع Claude أو Cursor أو Copilot أو أي وكيل MCP يفحص محرر Unreal ويتحكم فيه" width="100%"/>

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

<div dir="rtl">

‏**Unreal MCP** (أداة AI Game Developer لـ Unreal) يربط أي وكيل ذكاء اصطناعي يدعم MCP بـ **محرر Unreal**. اطلب بكلمات بسيطة، فيُنشئ الوكيل الأكتورات، ويعدّل المستويات، ويكتب Blueprints ويجمّعها، ويعدّل شيفرة C++ ويجمّعها، ويدير الأصول، ويتحقق من عمله بلقطات الشاشة. إنه نظير Unreal لكل من [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) و[Godot-MCP](https://github.com/IvanMurzak/Godot-MCP)، ويستخدم الخلفية السحابية نفسها ([ai-game.dev](https://ai-game.dev)) أو خادم [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) الخاص بك.

</div>

<p align="center">
  <img src="img/readme/architecture.svg" alt="يتحدث وكيل الذكاء الاصطناعي مع الخادم عبر MCP؛ ويمرر الخادم الرسائل عبر SignalR إلى العملية المرافقة unreal-mcp-bridge، التي تصل إلى إضافة C++ المسماة UnrealMCP داخل محرر Unreal عبر TCP على loopback" width="100%"/>
</p>

<div dir="rtl">

محرر Unreal مكتوب بـ C++، لذلك يعمل مضيف MCP المبني على .NET بجانبه كعملية مرافقة صغيرة (**sidecar**) باسم `unreal-mcp-bridge`، تشغّلها الإضافة وتراقبها وتعيد تشغيلها نيابة عنك. تفاصيل التصميم: [docs/ARCHITECTURE.md](ARCHITECTURE.md).

## الميزات

‏✅ ‏**طبقة بيانات RQL** — استعلم عن الأكتورات والأصول والمستويات وعدّلها بلغة واحدة ([RQL](rql.md))\
‏✅ ‏**Blueprints** — إنشاء وتعديل وتجميع، مع أخطاء منظمة يستطيع الوكيل إصلاحها\
‏✅ ‏**C++** — إنشاء هيكل شيفرة المشروع وتعديلها وتجميعها (Live Coding أو UBT)\
‏✅ ‏**تغذية راجعة مرئية** — لقطات للعرض (viewport) ونافذة اللعبة والكاميرا وأكتور معزول\
‏✅ ‏**Python** — تشغيل Python في المحرر مع واجهة `unreal` البرمجية كاملة\
‏✅ ‏**Fab** — البحث في Fab، وعرض مكتبتك، وإضافة المحتوى الذي تملكه\
‏✅ ‏**الامتدادات** — GAS وNiagara وPCG وLandscape وControl Rig وEnhanced Input\
‏✅ ‏**أدواتك الخاصة** — أضف أدوات وموجّهات وموارد من أي إضافة ([الدليل](EXTENSIONS.md))\
‏✅ ‏**وضع Runtime** — فعّله اختياريًا للتحكم في لعبة قيد التشغيل ([docs/RUNTIME.md](RUNTIME.ar.md))\
‏✅ ‏**سحابي أو محلي** — `ai-game.dev` جاهز مباشرة، أو `gamedev-mcp-server` تستضيفه بنفسك

‏30 أداة مدمجة في 8 عائلات؛ القائمة الكاملة في [docs/TOOLS.md](TOOLS.ar.md).

## المتطلبات

- ‏**Unreal Engine 5.5 – 5.8** (يتحقق مسار الإصدار من كل منها) و**مشروع C++**: تجمّع الإضافة وحدات المحرر داخل مشروعك.
- ‏**Node.js 22.14+** من أجل `unreal-mcp-cli`.
- ‏Windows أو macOS أو Linux. لا حاجة لتثبيت .NET: العملية المرافقة مضمّنة داخل الإضافة. (.NET 9 SDK مطلوب فقط لبناء bridge من المصدر.)

## البدء السريع

</div>

<p align="center">
  <img src="img/readme/terminal.svg" alt="الطرفية: npm install -g unreal-mcp-cli، ثم install-plugin، ثم login، ثم setup-mcp claude-code، ثم فتح المحرر" width="100%"/>
</p>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

<div dir="rtl">

‏`install-plugin` ينزّل إصدار المصدر الموقّع للإضافة المطابق لإصدار CLI. في المحرر، افتح **Tools → AI Game Developer** لترى الاتصال وحالة bridge والوكلاء المتصلين. ثم اطلب من وكيلك:

> *«أنشئ ثلاثة مكعبات في صف واحد ومصباحًا نقطيًا فوقها، ثم التقط لقطة شاشة.»*

طرق أخرى للتثبيت:

- ‏**Fab / Epic Marketplace**: [AI Game Developer على Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33). لا يمكن لنسخة Fab أن تتضمن ملفات تنفيذية، لذلك يرشدك زر **Get bridge** (الحصول على bridge) في النافذة هناك إلى تثبيت العملية المرافقة مرة واحدة.
- **يدويًا**: انسخ [`UnrealMCP/`](../UnrealMCP/) إلى `<YourProject>/Plugins/UnrealMCP/`. نسخة المصدر (checkout) لا تتضمن عملية مرافقة: شغّل `unreal-mcp-cli bootstrap-local` أو اضبط `UNREAL_MCP_BRIDGE_PATH`.

التحديث (`unreal-mcp-cli update`) وأوضاع الاتصال ومتغيرات البيئة واستكشاف الأخطاء وإصلاحها: [docs/CONFIGURATION.md](CONFIGURATION.ar.md).

## الاستخدام

في معظم الأوقات تكفي محادثة وكيلك. كما تستدعي CLI الأدوات نفسها مباشرة، وتشغّل محررًا دون واجهة رسومية (headless) إذا لم يكن هناك محرر قيد التشغيل:

</div>

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

<div dir="rtl">

جميع الأوامر والخيارات: [cli/README.md](../cli/docs/README.ar.md) أو `unreal-mcp-cli commands`.

## الامتدادات

‏الامتدادات إضافات اختيارية توسّع RQL؛ ولا تضيف أدوات خاصة بها.

</div>

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

<div dir="rtl">

| الامتداد | ما يضيفه | المتطلبات |
| --- | --- | --- |
| [AI&nbsp;ControlRig](../extensions/UnrealAIControlRig/README.md) | منصات تحريك إجرائية (rigs) وربطها بـ Anim Blueprint | `ControlRig`, `RigVM`, UE 5.7+‎ |
| [AI&nbsp;EnhancedInput](../extensions/UnrealAIEnhancedInput/README.md) | إجراءات الإدخال وسياقات التعيين والمشغّلات والمعدِّلات | `EnhancedInput`, UE 5.7+‎ |
| [AI&nbsp;GAS](../extensions/UnrealAIGAS/README.md) | Gameplay Effects وBlueprints القدرات ومجموعات السمات | `GameplayAbilities`, `EnhancedInput`, `GameplayTagsEditor`, UE 5.7+‎ |
| [AI&nbsp;Landscape](../extensions/UnrealAILandscape/README.md) | التضاريس والمحيطات والبحيرات والأنهار | `Water`, UE 5.7+‎ |
| [AI&nbsp;Niagara](../extensions/UnrealAINiagara/README.md) | أنظمة Niagara وبواعثها ووحداتها والتصادم | `Niagara`, `ChaosNiagara`, UE 5.7+‎ |
| [AI&nbsp;PCG](../extensions/UnrealAIPCG/README.md) | رسوم PCG البيانية والأحجام والتوزيعات المولَّدة | `PCG`, UE 5.7+‎ |

‏تُجمَّع الامتدادات من المصدر داخل مشروعك (عند فتح المحرر في المرة التالية، أو فورًا مع `--build`)؛ ويفعّل المثبّت إضافات المحرك التي تحتاجها.

## التوثيق

| الصفحة | المحتوى |
|---|---|
| [docs/TOOLS.md](TOOLS.ar.md) | كل الأدوات المدمجة وأدوات النظام، والامتدادات، وإظهار كل أداة |
| [docs/rql.md](rql.md) | لغة الاستعلام RQL: المصادر، والأعضاء، والتعديلات، واختبار اللعب |
| [docs/CONFIGURATION.md](CONFIGURATION.ar.md) | التشغيل الأول، والتحديث، ومتغيرات البيئة، واستكشاف الأخطاء وإصلاحها |
| [docs/EXTENSIONS.md](EXTENSIONS.md) | كتابة وحدات RQL وأدوات وموجّهات وموارد خاصة بك |
| [docs/RUNTIME.md](RUNTIME.ar.md) | وضع Runtime (داخل اللعبة)، وبوابات الأمان الخاصة به، وإبعاد الإضافة عن الألعاب المنشورة |
| [cli/README.md](../cli/docs/README.ar.md) | مرجع أوامر `unreal-mcp-cli` ورموز الخروج |
| [docs/ARCHITECTURE.md](ARCHITECTURE.md) | التصميم المرجعي (IPC، والتسجيل، والعملية المرافقة، والواجهة، والإعدادات) |
| [docs/RELEASING.md](RELEASING.md) | للمشرفين: الإصدارات، ومشغلات CI، والأسرار |
| [docs/EXTENSION-MONOREPO.md](EXTENSION-MONOREPO.md) | للمشرفين: اختبار الامتدادات وإصدارها |

## بنية المستودع

| المسار | ما هو |
|---|---|
| [`UnrealMCP/`](../UnrealMCP/) | إضافة Unreal (C++): `UnrealMcpRuntime`، `UnrealMcpRql`، `UnrealMcpEditor` |
| [`bridge/`](../bridge/) | `unreal-mcp-bridge`، العملية المرافقة المبنية على .NET 9 |
| [`cli/`](../cli/) | `unreal-mcp-cli`، حزمة npm |
| [`extensions/`](../extensions/) | ستة امتدادات اختيارية للمحرك بإصدارات مستقلة |
| [`samples/`](../samples/) | قالب امتداد للمحرر ومثال لأداة Runtime (داخل اللعبة) |

## روابط

- ‏[ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G) · [الإبلاغ عن مشكلة](https://github.com/IvanMurzak/Unreal-MCP/issues)
- المشاريع الشقيقة: [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) · [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP)
- المكونات المشتركة: [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) · [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) · [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet)
- ‏[Model Context Protocol](https://modelcontextprotocol.io/)

## الترخيص

‏[Apache-2.0](../LICENSE) © Ivan Murzak

</div>
