<b>[English](TOOLS.md) | [中文](TOOLS.zh-CN.md) | [日本語](TOOLS.ja.md) | [Español](TOOLS.es.md) | [فارسی](TOOLS.fa.md) | [हिन्दी](TOOLS.hi.md) | [العربية](TOOLS.ar.md)</b>

<div dir="rtl">

# أدوات Unreal-MCP

القائمة المرجعية للأدوات التي تسجّلها إضافة المحرر **UnrealMCP**. يأتي Unreal-MCP مع **30 أداة مدمجة في 8 عائلات** يستطيع وكيل الذكاء الاصطناعي استدعاءها، إضافة إلى **4 أدوات نظام** لا يستطيع استدعاءها (بنية تحتية للمضيف تخدم CLI وتطبيق سطح المكتب). معرّفات الأدوات بصيغة kebab-case (`rql-query`، `blueprint-compile`)، وهو الاصطلاح نفسه في Unity-MCP وGodot-MCP. تضيف [الامتدادات](#الامتدادات) و[إضافاتك الخاصة](EXTENSIONS.md) المزيد.

| العائلة | الأدوات | العدد |
| --- | --- | ---: |
| طبقة بيانات RQL | `rql-query`، `rql-mutate`، `rql-schema` | 3 |
| Blueprint | `blueprint-*`: create, get, add-component, remove-component, add-variable, modify-variable, set-default, add-function, add-event, compile, spawn | 11 |
| المصدر / C++ | `source-*`: read, create-class, update, delete, list, compile | 6 |
| لقطات الشاشة | `screenshot-*`: viewport, game-view, camera, isolated | 4 |
| Fab | `fab-search`، `fab-library`، `fab-add` | 3 |
| وحدة التحكم | `console-run-command` | 1 |
| Python | `python-execute` | 1 |
| قائمة الأدوات | `unreal-tool-list` | 1 |
| **النظام** (لا تُعرض على الذكاء الاصطناعي) | `ping`، `unreal-skill-create`، `unreal-skill-generate`، `editor-quit` | 4 |

> **مصدر الحقيقة.** الأدوات القياسية: `UnrealMcp*Tools.cpp` في `UnrealMCP/Source/UnrealMcpEditor/Private/Tools/` (باستثناء `UnrealMcpSkillTools.cpp`) و`UnrealMcpRqlTools.cpp` في `UnrealMcpRql`. أدوات النظام: `UnrealMcpPingTool.cpp` و`UnrealMcpSkillTools.cpp` و`UnrealMcpEditorSession.cpp` و`SkillGenerateTool.cs` في العملية المرافقة (sidecar). حدّث هذه الصفحة عند تغيّر تسجيل أي أداة.

جميع الأدوات القياسية الثلاثين **خاصة بالمحرر فقط**. اتصال Runtime (داخل اللعبة) لا يقدّم إلا `ping` إضافة إلى الأدوات التي تسجّلها لعبتك: راجع [RUNTIME.md](RUNTIME.ar.md).

## العائلات بالتفصيل

<details>
  <summary><b>RQL — طبقة البيانات (3)</b></summary>

لغة استعلام واحدة للمحرر كله بدلًا من أداة لكل عملية ([`docs/rql.md`](rql.md)): ابحث عن الأكتورات والمكوّنات والأصول، واقرأ الأعضاء، وصفِّ النتائج واجمعها، وعدّلها في استدعاء واحد. تحل محل أدوات الأكتور والكائن والأصل والمستوى وحالة المحرر والتحديد وسجل وحدة التحكم والانعكاس (reflection) السابقة.

| معرّف الأداة | ما تفعله |
| --- | --- |
| `rql-query` | استعلام RQL للقراءة فقط على المحرر الحي (`actors | where $ is PointLight | select $ref Label Intensity`)؛ نتائج جدولية مختصرة |
| `rql-mutate` | استعلام RQL ينتهي بـ `set` / `call` (`... | set Location = "(0, 0, 100)"`، `editor | call SpawnActor("PointLight")`): كل شيء أو لا شيء، خطوة تراجع واحدة، ومعاينة عبر `dry_run` |
| `rql-schema` | المصادر والدوال والأصناف الموجودة في عالم المحرر؛ ويعرض `type` أعضاء نوع واحد ودواله |

إلى جانب الخصائص المنعكسة، يصل RQL إلى صفوف DataTable (`dataTables`، `Rows["Sword"].Damage`)، ووسوم gameplay كنص (`gameplayTags`، `set Tags = ["Ability.Fire"]`)، وإعدادات Project Settings المحفوظة في ملف ini الخاص بها (`unreal.settings("Maps & Modes")`)، ومخططات Blueprint (`EventGraph | call AddCallFunction("PrintString")`، وتوصيلات المنافذ، و`call Compile()`)، وأشجار Widget Blueprint (`call AddWidget("Button", "Ok", parent)`)، وLevel Sequences (الارتباطات، والمسارات، و`call AddKey("Location.X", 30, 100)`).

مهارة `unreal-rql` (التي يكتبها `unreal-skill-generate`) تعلّم اللغة وتسرد ما تضيفه الامتدادات المثبتة.

</details>

<details>
  <summary><b>عائلة Blueprint (11) — الواجهة الرئيسية في Unreal</b></summary>

| معرّف الأداة | ما تفعله |
| --- | --- |
| `blueprint-create` | إنشاء صنف Blueprint جديد من مسار `UClass` أب |
| `blueprint-get` | ملخص المخطط لفحصه بواسطة LLM (المتغيرات، والمكوّنات، والدوال/الأحداث، وسلسلة الآباء) |
| `blueprint-add-component` | إضافة مكوّن عبر Simple Construction Script |
| `blueprint-remove-component` | إزالة مكوّن SCS |
| `blueprint-add-variable` | إضافة متغير عضو محدد النوع |
| `blueprint-modify-variable` | تعديل متغير عضو |
| `blueprint-set-default` | تعديل خاصية في CDO (القيم الافتراضية للصنف) |
| `blueprint-add-function` | إضافة هيكل دالة (مع توصيل عقدتي الدخول/النتيجة) |
| `blueprint-add-event` | إضافة/ربط هيكل حدث (BeginPlay، Tick، الإدخال، …) |
| `blueprint-compile` | تجميع Blueprint وإرجاع **قائمة منظمة بالأخطاء/التحذيرات** (حلقة التغذية الراجعة للذكاء الاصطناعي) |
| `blueprint-spawn` | إنشاء نسخة من Blueprint في المستوى الحالي |

</details>

<details>
  <summary><b>عائلة وحدة التحكم (1)</b></summary>

| معرّف الأداة | ما تفعله |
| --- | --- |
| `console-run-command` | تشغيل أمر في وحدة التحكم / CVar |

السجلات وحالة PIE والتحديد كلها عبر RQL: `console | where Verbosity == "Error"`، `editor | set IsPlaying = true`، `editor | set Selection = [...]`. يحاكي اختبار اللعب اللعبة داخل الاستدعاء نفسه (بشكل حتمي ومستقل عن تحديث المحرر بين الاستدعاءات): `editor | call StartPlay()`، `editor | call AdvancePlay(1.5, 0, "W, LeftShift")` (يضغط المفاتيح لمدة 1.5 ثانية، ويعيد موقع الـ pawn وسرعته والمسافة التي حرّكه بها الاستدعاء ووضع الحركة وسياقات الربط المطبقة)، `editor | call StopPlay()`؛ ويضيف امتداد Enhanced Input الاستدعاء `enhancedInput | call InjectInput(action, value, seconds)` ([docs/rql.md](rql.md#play-testing)).

</details>

<details>
  <summary><b>عائلة قائمة الأدوات (1)</b></summary>

| معرّف الأداة | ما تفعله |
| --- | --- |
| `unreal-tool-list` | سرد كل الأدوات، بما فيها المخفية عن `tools/list` (مُعلَّمة بأنها hidden؛ وتعمل عند استدعائها بالاسم)؛ `filter` بتعبير regex، و`includeDescription`، و`includeInputs` |

</details>

<details>
  <summary><b>عائلة Python (1)</b></summary>

| معرّف الأداة | ما تفعله |
| --- | --- |
| `python-execute` | تشغيل Python في المحرر (واجهة `unreal` كاملة، مستوردة مسبقًا): تعليمات عادية، وآخر تعبير هو النتيجة، ويعود `print` كسجلات، وتشير الأخطاء إلى أسطر `snippet.py`؛ يضيف `include` ملفات/مجلدات مساعدة من `<Project>/.ai-game-dev/snippets`؛ ويوقف `timeout` (بالثواني، الافتراضي 30) المقتطف الخارج عن السيطرة |

يستخدم PythonScriptPlugin الخاص بالمحرك (يفعّله Unreal-MCP؛ للمحرر فقط، ولا يكون أبدًا في لعبة محزّمة) وبيئة تشغيل [Code-Exec](https://github.com/IvanMurzak/Code-Exec) المضمّنة في `UnrealMCP/Content/Python`. توصيل مخططات Blueprint غير متاح في Python: استخدم أدوات Blueprint لذلك.

</details>

<details>
  <summary><b>عائلة Fab (3) — البحث عن محتوى Fab وإضافته</b></summary>

| معرّف الأداة | ما تفعله |
| --- | --- |
| `fab-search` | البحث في متجر [Fab](https://www.fab.com) (`query`، `listingType`، `freeOnly`، `sort`، والتصفح بين الصفحات)؛ يعيد لكل عنصر المعرّف uid والعنوان والنوع والسعر والصيغ |
| `fab-library` | سرد أصول Fab التي يملكها المستخدم المسجّل دخوله، مع مكان استيراد كل منها إن وُجد؛ ويبحث `filter` في المكتبة كلها |
| `fab-add` | تنزيل عنصر يملكه المستخدم واستيراده كما يفعل *Add to Project* في لوحة Fab (حزم Unreal Engine، وMegascans مع `quality`، وglTF/FBX)؛ يعيد مجلد المحتوى ويتخطى العناصر الموجودة مسبقًا في المشروع |

تعمل هذه الأدوات عبر إضافة Fab الخاصة بالمحرك (مفعّلة افتراضيًا منذ UE 5.5): تسجيل دخول Epic الخاص بها ومسارات الاستيراد فيها. أول استدعاء لـ `fab-library` / `fab-add` دون تسجيل دخول يفتح صفحة تسجيل دخول Epic في المتصفح (فتح Fab من شريط أدوات المحرر يسجّل الدخول أيضًا، من تسجيل دخول Epic محفوظ). الشراء، أو إضافة عنصر مجاني إلى المكتبة، يبقى على fab.com. المشاريع الكاملة وإضافات شيفرة المحرك تُثبَّت من Epic Games Launcher، ولا تُضاف إلى مشروع.

</details>

<details>
  <summary><b>عائلة المصدر / C++ (6)</b></summary>

| معرّف الأداة | ما تفعله |
| --- | --- |
| `source-read` | قراءة ملف مصدر C++ في المشروع (على أجزاء) |
| `source-create-class` | إنشاء هيكل صنف C++ جديد (header + cpp من القوالب) |
| `source-update` | تعديل ملف مصدر |
| `source-delete` | حذف ملف مصدر |
| `source-list` | سرد ملفات المصدر في الوحدة |
| `source-compile` | تجميع C++ الخاص بالمشروع (Live Coding إن كان مفعّلًا، وإلا UBT) مع تقرير منظم بالأخطاء؛ وعند إضافة أصناف جديدة أو تغيير تصريحات منعكسة (`restartRequired`) يوضح كيفية إعادة البناء وإعادة تشغيل المحرر |

كل عمليات الملفات محصورة داخل `<Project>/Source/`.

</details>

<details>
  <summary><b>عائلة لقطات الشاشة / التقاط العرض (4)</b></summary>

| معرّف الأداة | ما تفعله |
| --- | --- |
| `screenshot-viewport` | التقاط نافذة العرض النشطة في المحرر؛ والخيار `savePath` يحفظ ملف PNG داخل المشروع أيضًا |
| `screenshot-game-view` | التقاط عرض PIE / اللعبة؛ والخيار `savePath` كما سبق |
| `screenshot-camera` | العرض من أكتور كاميرا محدد عبر `USceneCaptureComponent2D`؛ والخيار `savePath` كما سبق |
| `screenshot-isolated` | عرض أكتور بمعزل عن غيره (SceneCapture2D مؤقت + قائمة show-only)؛ والخيار `savePath` كما سبق |

تعيد اللقطات **صورة PNG بصيغة محتوى صور MCP** (base64) ليتمكن LLM من فحص الصورة مباشرة. كل التقاط يرسم إطارًا جديدًا لحظة الاستدعاء، حتى عندما تكون نافذة المحرر في الخلفية أو مصغّرة، ويذكر نصه ما إذا كان الإطار جديدًا وما إذا كانت التأثيرات والرسوم المتحركة تُحدَّث. ما دام وكيل ذكاء اصطناعي متصلًا، تُبقي الإضافة المحرر يُحدَّث ويرسم بالمعدل الكامل في الخلفية (حتى وهو مصغّر) (يُتجاوز الخيار "Use Less CPU when in Background" لتلك الجلسة فقط؛ ويبقى تفضيلك المحفوظ دون تغيير). الأبعاد محدودة (الافتراضي 1024، والحد الأقصى 2048 لكل ضلع). يحتاج التقاط البكسلات إلى محرر يرسم: بنافذة، أو دون واجهة مع `-RenderOffscreen` (الافتراضي في `unreal-mcp-cli`). مع `-nullrhi` لا يُرسم شيء، لذا تعيد هذه الأدوات خطأ يذكر `-nullrhi` وطريقة الإصلاح.

</details>

<details>
  <summary><b>أدوات النظام (4) — غير متاحة للذكاء الاصطناعي</b></summary>

هذه **أدوات نظام**: بنية تحتية للمضيف تستدعيها `unreal-mcp-cli` وتطبيق سطح المكتب مباشرة عبر `POST /api/system-tools/<name>`. وهي غائبة عمدًا عن `tools/list` في MCP، لذلك لا تظهر أبدًا في جلسة ذكاء اصطناعي (ولا تستهلك رموزًا فيها) — وهو التقسيم نفسه الذي تعتمده Unity وGodot لأدوات النظام الخاصة بهما.

| معرّف الأداة | ما تفعله |
| --- | --- |
| `ping` | فحص الحيوية — رحلة ذهاب وإياب عبر سلسلة الإضافة ⇄ العملية المرافقة ⇄ الخادم |
| `unreal-skill-create` | توليد أداة MCP جديدة كملف C++ في وحدة المحرر الخاصة بالإضافة. **تتطلب إعادة بناء المحرر** قبل أن تصبح الأداة الجديدة قابلة للاستدعاء (Live Coding، أو إعادة بناء المشروع) — وتذكر النتيجة ذلك صراحة. تحتاج إلى تثبيت الإضافة من المصدر |
| `unreal-skill-generate` | إعادة توليد كل ملفات `SKILL.md` من الأدوات المسجّلة في مجلد skills نسبي للمشروع (الافتراضي `.claude/skills`) |
| `editor-quit` | حفظ الحزم المعدّلة التي لها مسار وإغلاق محرر شغّلته جلسة CLI (`unreal-mcp-cli close --owned-only`). ترفض ما لم يطابق `nonce` عقد الجلسة (lease)، لذلك لا تغلق أبدًا محررًا فتحته أنت |

</details>

## الامتدادات

ثبّت امتدادًا عندما يستخدم مشروعك أحد أنظمة المحرك أدناه، أو [أنشئ امتدادك الخاص](EXTENSIONS.md). كل امتداد هو وحدة RQL: يوسّع الأدوات الأساسية `rql-query` / `rql-mutate` / `rql-schema` ويضيف دليله إلى مهارة `unreal-rql`، لذلك لا تظهر أدوات MCP منفصلة في `tools/list`.

</div>

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas "<UEProject>"
```

<div dir="rtl">

تحتاج الامتدادات إلى Unreal Engine 5.7+، وUnreal MCP 0.26.0+ في المشروع نفسه، وأدوات بناء C++: تُنشر الإصدارات كمصدر وتُجمَّع لمشروعك (عند فتح المحرر التالي، أو فورًا مع `--build`). معرّف التثبيت هو `com.ivanmurzak.unreal-ai-<id>` حيث يأتي `<id>` من الجدول أدناه. خيارات التثبيت (`--version`، `--build`، و`--source` دون اتصال): [تثبيت امتداد في مشروع](EXTENSIONS.md#installing-an-extension-into-a-project).

| الامتداد (المعرّف) | الوصف |
| --- | --- |
| **[AI&nbsp;Control&nbsp;Rig](../extensions/UnrealAIControlRig/README.md)**<br>`control-rig` | إنشاء rigs بعظام وعناصر تحكم ومحلّل IK بعظمتين؛ والاستعلام عن Control Rig blueprints ومكوّناتها |
| **[AI&nbsp;Enhanced&nbsp;Input](../extensions/UnrealAIEnhancedInput/README.md)**<br>`enhanced-input` | الاستعلام عن Input Actions وMapping Contexts، وإنشاء actions وcontexts، وربط المفاتيح |
| **[AI GAS](../extensions/UnrealAIGAS/README.md)**<br>`gas` | بناء Gameplay Ability System دون C++: التأثيرات (التكلفة، وفترة التهدئة، والضرر، والتجدد)، وBlueprints للقدرات، ومكوّنات ability system مع attribute set جاهز لـ Health / Mana / Stamina، وربط الإدخال، وفحوص Play In Editor |
| **[AI Landscape](../extensions/UnrealAILandscape/README.md)**<br>`landscape` | Landscape وWater: إنشاء التضاريس ونحتها وطلاؤها؛ وإنشاء المحيطات والبحيرات والأنهار؛ والاستعلام عن التضاريس وwater bodies وwater zones |
| **[AI Niagara](../extensions/UnrealAINiagara/README.md)**<br>`niagara` | مؤثرات Niagara البصرية: الاستعلام عن الأنظمة وبواعثها (emitters)، ووضع الأنظمة في المستوى |
| **[AI PCG](../extensions/UnrealAIPCG/README.md)**<br>`pcg` | Procedural Content Generation: إنشاء مخططات PCG (العقد، والتوصيلات، والإعدادات، والمعاملات)، ووضعها على volumes، وتوليد التوزيعات (غابات، وصخور، وعناصر) وقراءة أعدادها |

## إظهار كل أداة على حدة

لكل أداة مربع اختيار **Visible** (ظاهرة) في نافذة **MCP Tools** — علامة التبويب المستقلة **MCP Tools** (المسجّلة تحت قائمة **Tools** في المحرر). تعرض النافذة عنوان كل أداة وعائلتها ووصفها، مع سطر ملخص "N / M tools visible". يحدد **Visible** فقط ما إذا كانت الأداة **مُدرجة في `tools/list` الخاص بـ MCP**، أي ما إذا كان وكلاء الذكاء الاصطناعي يرونها. وهو ليس تحكمًا في الوصول: الأداة المخفية تعمل عندما يستدعيها عميل بالاسم (`tools/call`)، كما تسرد `unreal-tool-list` الأدوات المخفية أيضًا (مُعلَّمة بأنها hidden) ليتمكن الوكيل من إيجادها.

ينطبق إعدادان (راجع [ARCHITECTURE](ARCHITECTURE.md) §7/§8):

- **القائمة البيضاء `enabledTools`** (يمكن تجاوزها عبر `UNREAL_MCP_TOOLS`) تحدد ما إذا كانت الأداة **تُقدَّم أصلًا** — عندما لا تكون فارغة، تُقدَّم الأدوات المدرجة فيها فقط، وأي أداة أخرى لا تُدرج ولا يمكن استدعاؤها (يرفضها `Execute()`)؛ والقائمة الفارغة تعني «بلا تصفية»؛
- **قائمة `hiddenTools`** — مربعات Visible التي تغيّرها في الواجهة — تُخفي أداة مقدَّمة عن `tools/list`.

يُحفظ كلاهما بين جلسات المحرر ويبقى بعد إعادة التحميل الفوري (hot-reload) لأي امتداد (الأداة المعاد تسجيلها ترث الإعداد المحفوظ، لذلك لا تُظهر إعادة البناء أبدًا أداة أخفيتها). ملف الإعدادات من إصدار أقدم للإضافة يبقى صالحًا: تُقرأ قائمة `disabledTools` فيه على أنها `hiddenTools`، ويكتب الحفظ التالي `hiddenTools`.

كل إعدادات الاتصال موجودة في قسم Connection داخل النافذة الرئيسية الوحيدة **AI Game Developer** (لا توجد علامة تبويب Settings منفصلة ولا صفحة في Project-Settings — تماثلًا مع Unity-MCP). نافذتا **MCP Prompts** و**MCP Resources** موصولتان لكنهما تأتيان فارغتين في هذا الإصدار — وتعرض كل منهما رسالة باهتة لحالة الفراغ (ملخص "N / M visible" خاص بنافذة Tools وحدها).

</div>
