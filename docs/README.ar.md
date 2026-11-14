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

‏**الحالة:** [أحدث إصدار](https://github.com/IvanMurzak/Unreal-MCP/releases/latest) (الإضافة وsidecar وCLI تتشارك رقم إصدار واحدًا) · Unreal Engine 5.5–5.8 · يُبنى sidecar لـ Windows x64 وmacOS (Intel وApple silicon) وLinux x64 · يُختبر كل إصدار على Windows (UE 5.5–5.8) وmacOS (UE 5.7، 5.8) · متاح على [Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) و[GitHub Releases](https://github.com/IvanMurzak/Unreal-MCP/releases).

## الميزات

‏✅ ‏**كتابة C++** — إنشاء هياكل الأصناف، وتعديل الشيفرة، والتجميع؛ تعود الأخطاء مع الملف والسطر ودرجة الخطورة\
‏✅ ‏**Blueprints** — الإنشاء والتعديل والتجميع، مع أخطاء منظَّمة يستطيع الوكيل إصلاحها\
‏✅ ‏**طبقة بيانات RQL** — استعلم عن الأكتورات والأصول والمستويات وغيّرها بلغة واحدة ([RQL](rql.md))\
‏✅ ‏**تغذية راجعة مرئية** — لقطات شاشة لنافذة العرض، وعرض اللعبة، والكاميرا، والأكتور المعزول\
‏✅ ‏**Python** — شغّل Python المحرر مع واجهة `unreal` البرمجية كاملة\
‏✅ ‏**Fab** — ابحث في Fab، واعرض مكتبتك، وأضف المحتوى الذي تملكه\
‏✅ ‏**الامتدادات** — GAS وNiagara وPCG وLandscape وControl Rig وEnhanced Input\
‏✅ ‏**أدواتك الخاصة** — أضف أدوات ومطالبات وموارد من أي إضافة ([الدليل](EXTENSIONS.md))\
‏✅ ‏**وضع Runtime** — فعّله اختياريًا للتحكم في لعبة قيد التشغيل ([docs/RUNTIME.md](RUNTIME.ar.md))\
‏✅ ‏**سحابي أو محلي** — ‏`ai-game.dev` جاهز للاستخدام، أو `gamedev-mcp-server` مستضاف ذاتيًا

‏**التغطية:** يستطيع الوكيل فعل كل ما تفعله تقريبًا في محرر Unreal. مجموعات الأدوات: طبقة بيانات RQL، وBlueprints، وشيفرة C++، ولقطات الشاشة، وFab، والكونسول، وPython المحرر، وقائمة الأدوات، إضافة إلى الامتدادات. كل الأدوات: [docs/TOOLS.md](TOOLS.ar.md).

‏**الكفاءة:** صُممت الأدوات لتكون مدمجة، فيحتاج الوكيل إلى استدعاءات ورموز (tokens) أقل لكل مهمة، ويعمل أسرع وبتكلفة أقل. بالنسبة لطبقة بيانات RQL، قيس ذلك في تقييم RQL نفسه (عالم لعبة تجريبي، بالمهام والنموذج والمُقيِّم نفسها): احتاجت RQL إلى استدعاءات أدوات أقل بنحو 9–18 مرة ورموز أقل بنحو 17–19 مرة لكل مهمة مقارنة بأدوات الانعكاس التقليدية (البحث / التفريغ إلى JSON / التعيين) ([النتائج](https://github.com/IvanMurzak/RQL/blob/main/RESULTS.md#33-rql-vs-conventional-reflection-tools)).

### ما يمكنك أن تطلبه

| اطلب من وكيلك | الأدوات التي يستخدمها |
| --- | --- |
| «أضف صنف أكتور `AHealthPickup` بلغة C++، وجمّعه وأصلح أي أخطاء» | `source-create-class`، `source-update`، `source-compile` |
| «اصنع Blueprint لباب يُفتح عند التداخل، وجمّعه، وضعه في المستوى» | `blueprint-*` |
| «عُدّ الأشجار في هذا المستوى وارفع كل الأضواء النقطية مترًا واحدًا» | `rql-query`، `rql-mutate` |
| «التقط لقطة شاشة من كاميرا اللاعب وتحقق من الإضاءة» | `screenshot-*` |
| «ابحث عن حزمة صخور مجانية على Fab وأضفها إلى المشروع» | `fab-search`، `fab-add` |
| «أعد تسمية الأصول المحددة دفعة واحدة بسكربت Python» | `python-execute` |

‏**C++ حلقة عمل من الدرجة الأولى.** يبني `source-compile` باستخدام Live Coding عندما يكون مفعّلًا، وإلا فباستخدام UnrealBuildTool، ويعيد كل رسالة تشخيص مع ملفها وسطرها ودرجة خطورتها، فيصلح الوكيل الأخطاء دون قراءة سجل البناء. ويُعلمك عندما يحتاج تغيير ما إلى بناء كامل وإعادة تشغيل المحرر (أصناف جديدة، أو تغيير تصريحات `UCLASS` / `UPROPERTY` / `UFUNCTION`). تبقى عمليات الملفات داخل `<Project>/Source/`. ومن الطرفية، يطبع `unreal-mcp-cli compile` رسائل تشخيص بصيغة `file:line:col` دون محرر.

‏**تُتعلَّم RQL من المحرر نفسه:** يسرد `rql-schema` كل مصدر ونوع وعضو، وتعلّم مهارة `unreal-rql` الصيغة مع وصفات جاهزة (يكتبها `unreal-mcp-cli setup-skills` لوكيلك).

## المتطلبات

- ‏**Unreal Engine 5.5 – 5.8** (يتحقق خط الإصدار من كل منها).
- **مشروع C++**، لأن الإضافة تُوزَّع كشيفرة مصدرية ويجمّع Unreal وحداتها لمشروعك بسلسلة أدوات C++ الخاصة بمنصتك. يتحول مشروع Blueprint فقط إلى مشروع C++ بخطوة واحدة: في المحرر، **Tools → New C++ Class…** وأنشئ أي صنف. لمشروع جديد، ينشئ `unreal-mcp-cli create-project ./MyGame` مشروع C++ بسيطًا. لا يحتاج إصدار Fab إلى مشروع C++: يثبّته Epic Games Launcher مُجمَّعًا مسبقًا.
- **لا حاجة إلى تثبيت .NET.** مضيف MCP برنامج .NET مستقل (`unreal-mcp-bridge`) مضمَّن داخل الإضافة لمنصتك؛ تشغّله الإضافة وتعيد تشغيله. (إصدار Fab لا يحتوي ملفات تنفيذية: يفتح زر **Get bridge** صفحة الإصدار، وتستخرج أنت الـ bridge في مجلد الـ bridge الخاص بالإضافة مرة واحدة.) لا يلزم .NET 9 SDK إلا لبناء الـ bridge من المصدر.
- ‏**Node.js 22.14+**، لـ `unreal-mcp-cli` فقط (حزمة npm؛ 22.14 هو أدنى إصدار Node لها).
- ‏**Windows أو macOS** (يُختبر كل إصدار على كليهما). **Linux x64:** يُبنى sidecar ويُنشر، لكن لا يعمل أي اختبار إصدار على Linux.

## البدء السريع

‏**1. ثبّت الإضافة**، بإحدى الطريقتين:

- ‏**[Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)** (دون طرفية): أضف **AI Game Developer** إلى محركك، وفعّله من **Edit → Plugins**، وافتح مشروعك.
- **أمران** (Node.js 22.14+):

</div>

```bash
npx unreal-mcp-cli install-plugin ./MyGame   # plugin + bundled bridge into MyGame/Plugins
npx unreal-mcp-cli open ./MyGame             # build if needed and open the editor
```

<div dir="rtl">

‏**2. اتصل من المحرر:** افتح **Tools → AI Game Developer**.

- لتثبيت Fab فقط: ثبّت الـ sidecar ‏`unreal-mcp-bridge` مرة واحدة (إصدار Fab لا يحمل ملفات تنفيذية): انقر **Get bridge** لفتح صفحة الإصدار، واستخرج ملف zip الخاص بمنصتك في المجلد الذي يعرضه **Open bridge folder**، ثم انقر **Connect**.
- ‏**Cloud** (الافتراضي): انقر **Authorize** ووافق على تسجيل الدخول في متصفحك. **محلي**: انتقل إلى **Custom** وانقر **Start** بجوار **MCP server**؛ ينزّل المحرر `gamedev-mcp-server` بعد التحقق من checksum (مع تثبيت Fab، ثبّت ذلك الخادم يدويًا: يذكر السجل رابط التنزيل).
- تحت **AI agent**، اختر وكيلك (Claude Code، Cursor، Codex، …) وانقر **Configure**.

‏**3. اطلب من وكيلك:** *«أنشئ ثلاثة مكعبات في صف وضوءًا نقطيًا فوقها، ثم التقط لقطة شاشة.»*

محلي أم Cloud؟ كلاهما مدعوم بالكامل، ويمكنك التبديل لاحقًا. **Cloud هو الافتراضي** لأنه لا يحتاج إلى فتح أي منافذ ويتجنب تعارض المنافذ مع البرامج الأخرى؛ والاتصال مجاني. يعمل **المحلي** دون حساب أو اتصال بالإنترنت، للأجهزة غير المتصلة والمعزولة (air-gapped).

| | **محلي** | **Cloud** |
| --- | --- | --- |
| الحساب | لا يلزم | تسجيل الدخول إلى ai-game.dev |
| خادم MCP | ‏`gamedev-mcp-server` على جهازك، يشغّله المحرر أو وكيلك | مُرحِّل ai-game.dev |
| حركة البيانات | تبقى على `localhost` | تمر عبر ai-game.dev ([ما الذي يسجّله](#what-the-cloud-relay-sees)) |
| المصادقة | لا شيء على loopback، أو رمز مشترك (خيار واحد) | ‏OAuth أو مفتاح لكل مشروع |
| مناسب لـ | العمل دون اتصال وفي البيئات المعزولة، وCI، والمشاريع الخاصة | لا منافذ مفتوحة ولا تعارض منافذ؛ حساب واحد لعدة مشاريع وأجهزة |

</div>

<details>
<summary><b>كل شيء من الطرفية</b> (CLI: ‏Cloud، محلي، مصادقة بالرمز)</summary>

<p align="center">
  <img src="img/readme/terminal.svg" alt="الطرفية: npm install -g unreal-mcp-cli، install-plugin، login، setup-mcp claude-code، فتح المحرر" width="100%"/>
</p>

<div dir="rtl">

‏**Cloud** (الوضع الافتراضي للإضافة):

</div>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

<div dir="rtl">

‏**محلي** (دون حساب):

</div>

```bash
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin ./MyGame
unreal-mcp-cli configure -p ./MyGame --mode Custom                 # plugin: local server
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --transport stdio # agent starts it
unreal-mcp-cli open ./MyGame
```

<div dir="rtl">

‏`setup-mcp --transport stdio` ينزّل إصدار `gamedev-mcp-server` المتحقَّق منه بـ checksum إلى المشروع ويوجّه الوكيل إليه. يستخدم الطرفان المنفذ نفسه الخاص بالمشروع على `localhost`.

‏**تفعيل المصادقة بالرمز (محلي):** أضف الخيارات إلى سطر `configure`، ثم شغّل `setup-mcp` مجددًا؛ بعدها تطلب الإضافة والخادم كلاهما السر.

</div>

```bash
unreal-mcp-cli configure -p ./MyGame --mode Custom --auth required --token <secret>
```

<div dir="rtl">

لماذا لا توجد مصادقة افتراضيًا في الوضع المحلي: يرتبط الخادم بـ loopback فقط ويرفض الطلبات القادمة من أصول متصفح غير محلية، فلا تصل إليه إلا البرامج الموجودة على جهازك. استخدم رمزًا عندما لا تثق بكل البرامج أو المستخدمين على ذلك الجهاز. يُحفظ السر في ملف `.env` الخاص بالمشروع (الذي يضيفه `configure` إلى gitignore) وفي ملف إعدادات الوكيل، فأبقِ ذلك الملف خارج نظام التحكم بالإصدارات.

</div>

</details>

<div dir="rtl">

<a id="what-the-cloud-relay-sees"></a>**ما الذي يراه المُرحِّل السحابي.** في وضع Cloud، تمر استدعاءات الأدوات عبر مُرحِّل MCP في ai-game.dev. المُرحِّل عديم الحالة (stateless): يوجّه استدعاءات الأدوات بين عميل الذكاء الاصطناعي والمحرر دون فحص محتواها أو حفظه، ولا يجمع محادثاتك ولا شيفرتك المصدرية ولا ملفات مشروعك ولا أصولك ولا بيانات المشاهد. يسجّل البيانات الوصفية للطلبات (عنوان IP للعميل، والطابع الزمني والمدة، ورمز الحالة ومسار الطلب، واسم الأداة، وحجم الطلب والاستجابة) والبيانات الوصفية لأحداث MCP (نوع الحدث، والتوقيت، والحالة، ومعرّفات الجلسة، واسم الأداة أو المطالبة أو المورد، وتفاصيل الأخطاء). وتُستخدم سجلات الطلبات وأحداث MCP أساسًا ضمن نافذة متجددة مدتها 90 يومًا؛ ولا تضمن السياسة الحذف التلقائي عند عمر محدد. التفاصيل: [سياسة الخصوصية](https://ai-game.dev/legal/privacy).

‏`install-plugin` ينزّل إصدار الشيفرة المصدرية الموقَّع للإضافة المطابق لإصدار CLI.

طرق أخرى للتثبيت:

- ‏**ملف zip بتنسيق Fab**: إلى جانب [صفحة Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)، يحمل كل إصدار على GitHub بنية بتنسيق Fab (`unreal-mcp-plugin-fab-<version>.zip`) دون ملفات تنفيذية؛ ومع أيٍّ منهما، يرشدك زر **Get bridge** في النافذة إلى تثبيت الـ sidecar مرة واحدة.
- **يدويًا**: انسخ [`UnrealMCP/`](../UnrealMCP/) إلى `<YourProject>/Plugins/UnrealMCP/`. نسخة الشيفرة المصدرية لا تتضمن sidecar مضمّنًا: شغّل `unreal-mcp-cli bootstrap-local` أو اضبط `UNREAL_MCP_BRIDGE_PATH`.

التحديث (`unreal-mcp-cli update`)، وأوضاع الاتصال، ومتغيرات البيئة، واستكشاف الأخطاء: [docs/CONFIGURATION.md](CONFIGURATION.ar.md).

### إعداد الوكيل

‏`unreal-mcp-cli setup-mcp <agent> -p ./MyGame` يكتب الإعدادات نيابةً عنك (`setup-mcp --list` يعرض كل الوكلاء المدعومين). ما الذي يكتبه:

</div>

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

<div dir="rtl">

‏`<pin>` يوجّه إلى محرر هذا المشروع. يكون الترويس مفتاح هذا المشروع عندما يكون الجهاز مسجَّل الدخول؛ و`--oauth` يكتب إعدادًا بالرابط فقط ويسجّل الوكيل دخوله بنفسه. ومع `--transport stdio` يشغّل الإدخال الخادم المحلي: `"command"` هو `gamedev-mcp-server` تحت `<project>/Intermediate/UnrealMCP/server/`، و`"args"` تضبط `port=<port>` و`client-transport=stdio` و`authorization=none`.

</div>

</details>

<details>
<summary><b>Cursor</b>: <code>.cursor/mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp cursor -p ./MyGame
```

<div dir="rtl">

إدخال `mcpServers` → `unreal-mcp` نفسه كما في Claude Code: ‏`"type": "http"` مع الرابط، أو `"type": "stdio"` مع `command` و`args` للخادم المحلي.

</div>

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

<div dir="rtl">

مع مفتاح مشروع، يحمل القسم أيضًا `http_headers` مع ترويسة `Authorization`.

</div>

</details>

<div dir="rtl">

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

جميع الأوامر والخيارات: [cli/docs/commands.md](../cli/docs/commands.md) أو `unreal-mcp-cli commands`.

## الأمان

- ‏**الإضافة ↔ sidecar:** تستمع الإضافة على منفذ TCP في loopback؛ ويتصل الـ sidecar بها برمز لمرة واحدة يُرسَل عبر stdin (لا يظهر أبدًا في سطر الأوامر ولا يُسجَّل أبدًا).
- ‏**الخادم المحلي:** يرتبط `gamedev-mcp-server` بـ loopback افتراضيًا ويردّ بـ `403` على أي طلب لا يُسمح بـ `Origin` الخاص به (دفاع ضد DNS rebinding). على loopback تشغّله الإضافة دون مصادقة ما لم تختر رمزًا (`UNREAL_MCP_AUTH_OPTION`، `UNREAL_MCP_TOKEN`). خيارات الخادم: [الاستضافة والمصادقة](https://github.com/IvanMurzak/GameDev-MCP-Server/blob/main/docs/hosting.md).
- ‏**Cloud:** يسجّل الوكيل دخوله عبر OAuth أو يستخدم مفتاحًا لكل مشروع يمكنك استبداله بـ `setup-mcp --regenerate-key`. المُرحِّل عديم الحالة ولا يحفظ محتوى الاستدعاءات؛ ويسجّل البيانات الوصفية للطلبات ولأحداث MCP ([التفاصيل](#what-the-cloud-relay-sees)).
- **ما تستطيع الأدوات فعله:** تعديل الملفات تحت `<Project>/Source/`، وتشغيل أوامر الكونسول وPython المحرر، وتغيير أي خاصية منعكسة.
- **وسائل التحكم فيها:**
  - **القائمة البيضاء** (حد صارم): يقدّم `UNREAL_MCP_TOOLS` الأدوات المدرجة فقط؛ أي أداة أخرى لا تُعرض ولا يمكن استدعاؤها.
  - **الإخفاء** (حد مرن): مربع الاختيار **Visible** في نافذة **MCP Tools** يزيل الأداة من `tools/list`، لكن العميل الذي يستدعيها باسمها يظل قادرًا على تشغيلها.
  - **تغييرات محدودة:** تبقى أدوات ملفات C++ داخل `<Project>/Source/`؛ واستدعاء `rql-mutate` يُنفَّذ كله أو لا شيء، وخطوة تراجع واحدة، ويمكن معاينته بـ `dry_run`، ويُرفض فوق 200 هدف ما لم يرفع الاستدعاء `max_targets` (حتى 1000)؛ ويتوقف `python-execute` بعد `timeout` (‏30 ثانية افتراضيًا)؛ ولكل استدعاء أداة حد زمني (`UNREAL_MCP_TOOL_TIMEOUT`، ‏5 دقائق افتراضيًا).
  - **ليست للذكاء الاصطناعي:** أدوات النظام (`ping`، وتوليد المهارات، و`editor-quit`) غائبة عن `tools/list`، ويرفض `editor-quit` التنفيذ ما لم يتطابق nonce جلسة CLI.
  - التفاصيل: [docs/TOOLS.md](TOOLS.ar.md).
- **وضع Runtime معطَّل افتراضيًا:** لا تتصل اللعبة أبدًا من تلقاء نفسها. يحتاج إلى إعداد `bRuntimeMcpEnabled` (معطَّل افتراضيًا) واستدعاء صريح لـ `Connect()`، ولا يقبل إلا مضيف loopback ما لم تسمح بمضيف بعيد، ويُرفض في بنيات Shipping ما لم يُجمَّع ضمنها. أبقِ الإضافة خارج الألعاب المحزَّمة باستخدام `TargetDenyList` ([docs/RUNTIME.md](RUNTIME.ar.md)).

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
| [cli/docs/commands.md](../cli/docs/commands.md) | مرجع أوامر `unreal-mcp-cli` ورموز الخروج |
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

‏**مجاني لمطوري الألعاب، بما في ذلك الألعاب التجارية.** يخضع Unreal MCP لترخيص [Apache-2.0](../LICENSE) © Ivan Murzak. ويستخدم مكوّنان مضمَّنان، RQL وCode-Exec، ترخيص [PolyForm Shield 1.0.0](https://polyformproject.org/licenses/shield/1.0.0)، الذي لا يقيّد إلا الشركات التي تبني أدوات ذكاء اصطناعي منافسة لتطوير الألعاب؛ ويمكنك استخدام كل ذلك لصنع ألعابك وإطلاقها وبيعها. الإشعارات: [`UnrealMCP/ThirdPartyNotices.txt`](../UnrealMCP/ThirdPartyNotices.txt).

</div>
