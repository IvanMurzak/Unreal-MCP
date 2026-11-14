<b>[English](RUNTIME.md) | [中文](RUNTIME.zh-CN.md) | [日本語](RUNTIME.ja.md) | [Español](RUNTIME.es.md) | [فارسی](RUNTIME.fa.md) | [हिन्दी](RUNTIME.hi.md) | [العربية](RUNTIME.ar.md)</b>

<div dir="rtl">

# الاستخدام في وقت التشغيل (داخل اللعبة)

افتراضيًا يتحكم Unreal-MCP في **المحرر**. ويمكنه أيضًا العمل **داخل لعبة قيد التشغيل** — PIE أو Standalone أو بناء **Development** محزّم — ليتحكم مساعد الذكاء الاصطناعي في لعبتك مباشرة. هذا هو نظير Unreal لـ [دعم Runtime (داخل اللعبة) في Unity-MCP](https://github.com/IvanMurzak/Unity-MCP#runtime-usage-in-game): المقابل في UE لـ `UnityMcpPluginRuntime.Initialize().Build().Connect()` في Unity ولمثال Chess-bot المبني على `[AiTool]`.

نقطة الدخول في وقت التشغيل هي `UGameInstanceSubsystem`، وتحديدًا [`UUnrealMcpRuntimeSubsystem`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpRuntimeSubsystem.h) (في **وحدة runtime** الخاصة بالإضافة `UnrealMcpRuntime`). يُنشأ تلقائيًا مرة واحدة لكل `UGameInstance` لكنه **لا يتصل تلقائيًا أبدًا** — الاتصال دائمًا استدعاء صريح واختياري (راجع [عقد الأمان](#runtime-security-contract) أدناه).

## ثلاث طرق للاتصال

تصل الطرق الثلاث كلها إلى `UUnrealMcpRuntimeSubsystem::Connect(Host, Token, Mode, bAllowRemoteHost)` نفسها؛ ووضع الاتصال الافتراضي هو **Custom** (خادم loopback يوفّره المطوّر).

‏**1. من C++** (مثلًا في `GameMode::BeginPlay` الخاص بك):

</div>

```cpp
#include "UnrealMcpRuntimeSubsystem.h"

void AMyGameMode::BeginPlay()
{
    Super::BeginPlay();

    if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
        Mcp->Connect(TEXT("http://localhost:8080"), TEXT("my-token")); // Custom mode, loopback
    // ... and, when you are done:
    //  Mcp->Disconnect();
}
```

<div dir="rtl">

‏`Get(WorldContext)` دالة مساعدة ثابتة من نوع `BlueprintPure` تعيد الـ subsystem الخاص بـ game instance لذلك السياق (أو null). تعيد `Connect` القيمة `false` (ولا تتصل بشيء) إذا رفضت أي بوابة أمان الاتصال — راجع ما يلي.

‏**2. من Blueprint** — `Get Unreal MCP Runtime Subsystem` (عقدة `Get`، المدركة لـ `WorldContext`) → **`Connect`** (عقدة `BlueprintCallable` ضمن الفئة **Unreal MCP**؛ و`Token` / `Mode` / `bAllowRemoteHost` منافذ متقدمة). استخدمها مع عقدة **`Disconnect`** والعقدة النقية **`Is Connected`** لمعرفة الحالة.

‏**3. من وحدة التحكم** (لتسهيل عمل QA، دون إعادة تجميع) — تُسجَّل ما دام الـ subsystem موجودًا:

</div>

```
UnrealMcp.Connect <host> [token]
UnrealMcp.Disconnect
```

<div dir="rtl">

يستخدم مسار وحدة التحكم دائمًا loopback ووضع Custom.

## أدواتك وموجّهاتك ومواردك الخاصة داخل اللعبة

تأتي اللعبة مع أدوات اللعب **الخاصة بها** (ومع موجّهات وموارد إن كانت مفيدة) ويتحكم فيها الذكاء الاصطناعي مباشرة — المقابل في UE لـ `WithToolsFromAssembly` / `[AiTool]` Chess-bot في Unity. تكتب الأنواع الثلاثة تمامًا كما في امتداد المحرر ([EXTENSIONS.md](EXTENSIONS.md))، مع تغييرين لوحدة اللعبة: اجعلها **`Type=Runtime`** واجعلها تعتمد على **`UnrealMcpRuntime`** (لا `UnrealMcpEditor`). سجّل المزوّدين (providers) عند بدء الوحدة، أو استخدم الأغلفة القابلة للاكتشاف في الـ subsystem — زوج تسجيل/إلغاء تسجيل لكل نوع:

</div>

```cpp
if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
{
    Mcp->RegisterToolProvider(MyToolProvider);          // tools merge in; manifest re-pushed
    Mcp->RegisterPromptProvider(MyPromptProvider);      // prompts merge in; manifest re-pushed
    Mcp->RegisterResourceProvider(MyResourceProvider);  // resources merge in; manifest re-pushed
}
// ... before destroying the providers, call the matching Unregister*Provider(...) for each.
```

<div dir="rtl">

المثال الكامل القابل للبناء هو [`samples/UnrealAIRuntimeSample/`](../samples/UnrealAIRuntimeSample) — إضافة `Type=Runtime` تقرأ أداتها **`game-time-dilation`** قيمة `AWorldSettings::TimeDilation` في العالم الحي وتضبطها (حركة بطيئة / تسريع)، ويمكن استدعاؤها في لعبة قيد التشغيل عبر اتصال MCP في وقت التشغيل. (يوضح المثال مسار الأدوات؛ أما الموجّهات والموارد فتُسجَّل عبر واجهة الأغلفة الثلاثة نفسها المعروضة أعلاه، والعائلات الأساسية المرفقة هي المرجع القابل للتشغيل.) تفاصيل جانب المؤلف (العقد، ودورة الحياة، والترتيب، والعزل) موجودة في [قسم Runtime usage في `docs/EXTENSIONS.md`](EXTENSIONS.md#runtime-usage-in-game-extensions).

## مجموعة أدوات وقت التشغيل مقابل أدوات المحرر فقط

يحمل اتصال وقت التشغيل أداة مدمجة **واحدة** بالضبط: **`ping`** (فحص الحيوية). `ping` [أداة نظام](TOOLS.ar.md)، لذا تجيب على `POST /api/system-tools/ping` ولا تُعرض على وكيل الذكاء الاصطناعي — أي أن اللعبة المحزّمة التي لا تسجّل شيئًا خاصًا بها تعرض قائمة أدوات **فارغة**، وهو وصف صادق لها. كل ما يستطيع وكيل الذكاء الاصطناعي فعله في وقت التشغيل **تجلبه أنت**: سجّل أدواتك عبر ناقل الامتدادات أعلاه (`RegisterToolProvider`) وستظهر بشكل طبيعي.

كل عائلات تطوير المحرك — طبقة بيانات RQL (`rql-query` / `rql-mutate` / `rql-schema` على الأكتورات والمكوّنات والأصول والمستويات ووحدة التحكم وحالة المحرر)، وكل أدوات لقطات الشاشة، إضافة إلى إنشاء Blueprint، وتعديل مصدر C++ وتجميعه، و`console-run-command`، و`unreal-tool-list`، و`python-execute`، وأدوات Fab — **خاصة بالمحرر فقط** ([أدوات المحرر الثلاثون](TOOLS.ar.md)). فهي تتحكم في المحرر وبعضها من فئة RCE (مثل `rql-mutate` و`console-run-command` و`python-execute`)، لذلك لا تُجمَّع في لعبة منشورة افتراضيًا. لا يوجد محرر في لعبة محزّمة، لذا فإن الواجهة المدمجة في وقت التشغيل مقصورة عمدًا على `ping` وما تسجّله لعبتك من أدوات.

## <a id="editor-only-exclude-from-packaged-games"></a>للمحرر فقط — استبعاد Unreal-MCP من الألعاب المحزّمة

إذا كنت تريد Unreal-MCP فقط **لأدوات الذكاء الاصطناعي داخل محرر Unreal** وتنوي نشر لعبة محزّمة **دون أي أثر لـ Unreal-MCP** — دون وحدة runtime، ودون ملف تنفيذي للعملية المرافقة المبنية على `.NET` — فثبّت الإضافة على المحرر عبر **`TargetDenyList`** في **ملف `.uproject` الخاص بمشروعك المستهلك** (لا في `.uplugin` الخاص بالإضافة):

</div>

```jsonc
// <YourProject>.uproject — "Plugins" array
{
    "Name": "UnrealMCP",
    "Enabled": true,
    "TargetDenyList": [ "Game", "Client", "Server" ]
}
```

<div dir="rtl">

‏UE يحترم `TargetDenyList` / `TargetAllowList` في مرجع الإضافة (`PluginReferenceDescriptor::IsEnabledForTarget`): تبقى الإضافة مفعّلة في هدف **Editor**، لكنها **تُستبعد من عمليات البناء المحزّمة `Game` / `Client` / `Server`** — فلا تُجهَّز ولا تُجمَّع في منتجك المنشور وحدة البنية التحتية لوقت التشغيل ولا العملية المرافقة المضمّنة `unreal-mcp-bridge`. وحدات نوع Editor (`UnrealMcpEditor`) تُحذف من بناء اللعبة في كل الأحوال؛ كما تُسقط قائمة المنع وحدة البنية التحتية `UnrealMcpRuntime` و`RuntimeDependencies` الخاصة بعمليتها المرافقة (§12.5)، فتحصل على **أثر مُجمَّع صفري**.

> **تنبيه — لا تُنشئ اعتمادًا مباشرًا على الوحدة.** لا تستبعد `TargetDenyList` إلا وحدات الإضافة *نفسها*. إذا أضافت إحدى وحدات **اللعبة** لديك `UnrealMcpRuntime` (أو أي وحدة `UnrealMcp*`) إلى `PublicDependencyModuleNames` / `PrivateDependencyModuleNames` في ملف `*.Build.cs`، أو استخدمت `#include` لملف header من `UnrealMcp*`، فإن ذلك الاعتماد المباشر **يتجاوز قائمة المنع** ويجمّع UBT وحدة runtime في لعبتك على أي حال. مشروع المحرر فقط الخالص لا يشير إلى أي شيء من الإضافة، لذا يبقى نظيفًا بطبيعته — لا تُضف اعتمادًا على الوحدة إلا عندما تريد فعلًا [الاستخدام في وقت التشغيل (داخل اللعبة)](#الاستخدام-في-وقت-التشغيل-داخل-اللعبة).

أما **للاستخدام في وقت التشغيل (داخل اللعبة)**، فاترك قائمة المنع **غير مفعّلة**: تُشحن وحدة البنية التحتية `UnrealMcpRuntime` + الأداة المدمجة `ping` داخل البناء المحزّم (§12.5)، وتسجّل أدوات اللعب الخاصة بك عبر `IUnrealMcpToolProvider` — راجع [ثلاث طرق للاتصال](#ثلاث-طرق-للاتصال) و[EXTENSIONS.md](EXTENSIONS.md). الخياران متنافيان: قائمة منع لمشروع المحرر فقط، ولا قائمة منع للعبة تستضيف اتصال MCP حيًا.

## <a id="runtime-security-contract"></a>عقد الأمان

اتصال وقت التشغيل هو **تحكم عن بُعد في لعبة قيد التشغيل** (إنشاء الأكتورات، وكتابة أي خاصية منعكسة، وأي CVars عبر `console-run-command`، واستدعاء دوال `UFunction`) — وسيكون من فئة RCE لو أمكن الوصول إليه في منتج منشور. لذلك تُقفَل واجهة وقت التشغيل بـ **خمس طبقات حماية** ([`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §12.8)، تُطبَّق كلها داخل `Connect()`:

1. **بالاشتراك الاختياري فقط.** يُنشأ الـ subsystem تلقائيًا لكنه **لا يتصل تلقائيًا أبدًا** — لا اتصال تلقائي عند التحميل ولا اتصال تلقائي من config asset. لا يحدث الاتصال إلا عبر استدعاء صريح لـ `Connect()`.
2. **مفتاح إيقاف، معطّل افتراضيًا.** القيمة الافتراضية لـ `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled` (Project Settings → Plugins → **Unreal MCP (Runtime)**) هي **`false`**. ما دام معطّلًا، يُرفض كل `Connect()` ولا تُشغَّل أي عملية مرافقة. إنه إعداد من نوع **Game** (`DefaultGame.ini`، القسم `[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]`)، لذا ينتقل إلى البناء المحزّم حيث يجب أن تعمل البوابة. تفعيله **لا** يؤدي إلى اتصال تلقائي — ما زال استدعاء `Connect()` صريح مطلوبًا.
3. **بوابة Shipping.** القيمة الافتراضية للعلم `bUnrealMcpAllowShipping` في `*.Build.cs` هي **`false`** (→ `UNREAL_MCP_ALLOW_SHIPPING=0`)؛ في بناء **Shipping** يسجّل `Connect()` رسالة ويعيد `false` ما لم يُضمَّن ذلك العلم عمدًا عند التجميع. عمليات بناء Development / PIE لا تتأثر.
4. **مضيف loopback افتراضيًا.** يرفض `Connect()` أي `Host` ليس loopback ما لم يمرر المستدعي صراحة `bAllowRemoteHost = true`. يُبقي الإعداد الافتراضي الاتصال على `localhost` / `127.0.0.0/8` / `::1`.
5. **IPC عبر loopback + رمز لمرة واحدة عبر stdin.** IPC بين الإضافة ⇄ العملية المرافقة يقتصر على loopback ويُصادَق عليه برمز لمرة واحدة يُسلَّم عبر **stdin، وليس argv أبدًا**، ولا يُسجَّل أبدًا (النموذج نفسه في المحرر، ARCHITECTURE §1.4).

إضافة إلى ذلك، مسار وقت التشغيل **لسطح المكتب فقط** (Win64 / Mac / Linux): تشغيل عملية `unreal-mcp-bridge` المرافقة المبنية على .NET غير ممكن على منصات الكونسول أو الأجهزة المحمولة، لذلك لا توجد لتلك المنصات أي واجهة MCP في وقت التشغيل.

> **دليل التشغيل الشامل.** الشرح الكامل للمشغّل — تفعيل مفتاح الإيقاف، والاتصال من PIE ومن بناء Development محزّم، وتجربة الأدوات عبر الاتصال الحي — موجود في [`docs/RUNTIME-E2E.md`](RUNTIME-E2E.md). أما الجزء الحتمي (بوابات الأمان، وتبديل world-resolver، وأمان connect/disconnect/orphan) فتثبّته مواصفات Automation دون واجهة (headless).

</div>
