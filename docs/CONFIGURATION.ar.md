<b>[English](CONFIGURATION.md) | [中文](CONFIGURATION.zh-CN.md) | [日本語](CONFIGURATION.ja.md) | [Español](CONFIGURATION.es.md) | [فارسی](CONFIGURATION.fa.md) | [हिन्दी](CONFIGURATION.hi.md) | [العربية](CONFIGURATION.ar.md)</b>

<div dir="rtl">

# الإعدادات والتحديثات واستكشاف الأخطاء وإصلاحها

كيف توصل المحرر، وتُبقي الإضافة محدّثة، وتضبطها بمتغيرات البيئة، وتصلح المشكلات الشائعة. خطوات التثبيت موجودة في [README](README.ar.md)؛ والتصميم الذي يقوم عليه كل ذلك في [ARCHITECTURE.md](ARCHITECTURE.md).

- [التشغيل الأول](#التشغيل-الأول)
- [تحديث الإضافة](#تحديث-الإضافة)
- [متغيرات البيئة](#متغيرات-البيئة)
- [استكشاف الأخطاء وإصلاحها](#استكشاف-الأخطاء-وإصلاحها)

## التشغيل الأول

1. افتح النافذة الرئيسية **AI Game Developer** من قائمة **Tools** في المحرر (علامة التبويب مسجّلة تحت فئة قائمة Tools).
2. **اختر وضع الاتصال:**
   - ‏**Cloud** (الافتراضي) — يتصل بـ [ai-game.dev](https://ai-game.dev). انقر **Authorize** لبدء **تدفق device-code** في OAuth: تعرض النافذة رابط تحقق ورمز مستخدم قصيرًا؛ افتح الرابط، وأدخل الرمز، ووافق، فيُكمل المحرر التفويض. استخدم **Revoke** لمسح الرمز السحابي المخزّن.
   - ‏**Custom** — يتصل بخادم `gamedev-mcp-server` محلي تشغّله أنت (أو أي خادم متوافق). أدخل رابط الخادم ووجّه عميل الذكاء الاصطناعي إليه. شغّل الخادم بنفسك، أو دع المحرر يشغّله ويوقفه (`UNREAL_MCP_START_SERVER`، في وضع Custom + http فقط؛ راجع [متغيرات البيئة](#متغيرات-البيئة)).
3. يعرض قسم **Connection** نقطة حالة، وتسمية حالة، وزر Connect / Disconnect / Stop؛ وتظهر حالة bridge على شكل `Running (restarts: N)` أو `Stopped`. استخدمها للتأكد من أن العملية المرافقة تعمل.
4. وجّه عميل الذكاء الاصطناعي لديك (Claude Code، Cursor، تطبيق AI Game Developer، …) إلى الخادم. يسرد قسم **AI agents** الوكلاء المتصلين حاليًا؛ ولكتابة إعدادات عميل MCP استخدم `unreal-mcp-cli setup-mcp`.

تُحفظ إعدادات الاتصال في `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` (مجلد `Saved/` مُدرج في gitignore في كل قوالب UE، لذا لا تصل الرموز إلى نظام التحكم في الإصدارات افتراضيًا).

> **رابط عميل MCP المثبّت.** يكتب `unreal-mcp-cli setup-mcp <agent>` إعدادات عميل MCP تشير إلى الرابط السحابي **المثبّت على المشروع** `<base>/mcp/p/<pin>`، فيُوجَّه الوكيل إلى محرر **هذا** المشروع حتى عندما يتحكم حسابك في عدة محررات. مرّر `--no-pin` لكتابة الرابط المجرد `<base>/mcp` بدلًا منه. التثبيت (pin) مجرد جزء من مسار التوجيه — ويبقى مورد OAuth هو `<base>/mcp`.

> **مفتاح المشروع.** عندما يكون هذا الجهاز مسجّل الدخول (`unreal-mcp-cli login`، أو Sign in في المحرر)، تحمل إعدادات Cloud أيضًا `Authorization: Bearer agd_pk_…` — مفتاح لا تنتهي صلاحيته ويمكن إلغاؤه، ومرتبط بتثبيت هذا المشروع — لكل وكيل، فلا يحتاج أي وكيل إلى OAuth خاص به. يُخزَّن المفتاح مؤقتًا في `~/.ai-game-dev/project-keys.json` ويُعاد استخدامه. مرّر `--oauth` لإعدادات بالرابط فقط (يسجّل الوكيل دخوله بنفسه)، أو `--regenerate-key` لإصدار مفتاح جديد وإلغاء القديم (في لوحة AI agent بالمحرر زر **Regenerate key**). إذا لم تكن مسجّل الدخول، أو تعذّر على الخادم إصدار مفتاح ⇐ إعدادات بالرابط فقط.

## تحديث الإضافة

يجب أن يتركك التحديث في المكان دائمًا على الشيفرة **الجديدة**. يأتي الخطر من المجمّع التزايدي في UE: إذا تغيّر مصدر الإضافة (ملفات `.cpp` جديدة، أو وحدة جديدة) لكن بقيت ذاكرة البناء المؤقتة القديمة `UnrealMCP/Intermediate/`، فقد يُجري UE تجميعًا جزئيًا وفق قائمة ملفات قديمة للوحدة ويتركك بصمت على شيفرة قديمة/ناقصة. كل قناة تعالج ذلك بطريقة مختلفة:

- ‏**Fab / Epic Marketplace (بعد نشر العنصر) ← تلقائي.** يستبدل Epic Games Launcher الملفات الثنائية المجمّعة مسبقًا في مكانها؛ لا شيء لتجميعه ولا ذاكرة مؤقتة لمسحها.
- ‏**`unreal-mcp-cli update` ← إعادة بناء نظيفة تلقائية.** يعيد `update` نسخ مصدر الإضافة، و**افتراضيًا** يحذف مجلد `Intermediate/` القديم ومجلد `Binaries/` الخاص بـ C++ في الإضافة المثبتة، ليُجري UE تجميعًا نظيفًا عند تشغيل المحرر التالي — دون خطوات يدوية. يبقى bridge المضمّن (العملية المرافقة) تحت `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` سليمًا: تحدّثه تثبيتات release-source من `Source/ThirdParty/...`، بينما تحافظ تثبيتات repo/dev على النسخة المضمّنة السابقة عند الحاجة. تثبيتات **junction** الخاصة بالتطوير لا تُنظَّف أبدًا (لأن ذلك سيمسح مخرجات شجرة المصدر الحية لديك). مرّر `--no-clean` لتجنب مسح الذاكرة المؤقتة.

</div>

  ```bash
  unreal-mcp-cli update <YourProject>            # default: clean rebuild on version change
  unreal-mcp-cli update <YourProject> --force    # re-copy even when versions match
  unreal-mcp-cli update <YourProject> --no-clean # keep the existing build cache
  unreal-mcp-cli update <YourProject> --no-extensions # leave the installed extensions as they are
  unreal-mcp-cli update <YourProject> --no-self  # leave the CLI as it is
  unreal-mcp-cli update <YourProject> --check    # only report; exit code 2 when something is outdated
  ```

<div dir="rtl">

- **النسخ اليدوي ← امسح الذاكرة المؤقتة بنفسك.** إذا استبدلت `<YourProject>/Plugins/UnrealMCP/` يدويًا، **أغلق المحرر أولًا**، واحذف `<YourProject>/Plugins/UnrealMCP/Intermediate/` ومجلد `Binaries/` الخاص بـ C++ (احتفظ بـ `Binaries/ThirdParty/` إذا كان هناك bridge مضمّن)، ثم أعد التشغيل ليجمّع UE من جديد بشكل نظيف.

## متغيرات البيئة

تقرأ الإضافة الإعدادات بهذه الأولوية: **بيئة العملية ← `<Project>/.env` ← ملف الإعدادات ← القيم الافتراضية المدمجة**. متغيرات `UNREAL_MCP_*` المعتمدة (معرّفة في `UnrealMCP/Source/UnrealMcpRuntime/Private/Config/UnrealMcpConfig.cpp`؛ ويقرأ bridge المتغير `UNREAL_MCP_CREDENTIALS_DIR`، وتقرأ CLI المتغير `UNREAL_MCP_SERVER_PATH`):

| المتغير | الغرض |
| --- | --- |
| `UNREAL_MCP_CONNECTION_MODE` | `Cloud` أو `Custom`. **الافتراضي `Cloud`.** |
| `UNREAL_MCP_HOST` | رابط الخادم (وضع Custom)، مثل `http://localhost:<port>` |
| `UNREAL_MCP_CLOUD_URL` | رابط الخلفية السحابية (الافتراضي ai-game.dev) |
| `UNREAL_MCP_TOKEN` | رمز المصادقة (رمز IPC للعملية المرافقة / رمز الخادم). **سرّي — لا تُودِعه أبدًا في المستودع.** |
| `UNREAL_MCP_AUTH_OPTION` | `none` أو `oauth` (مصادقة الخادم المحلي) |
| `UNREAL_MCP_KEEP_CONNECTED` | حفظ حالة الاتصال واستعادتها |
| `UNREAL_MCP_TOOLS` | تجاوز قائمة الأدوات المفعّلة (قائمة بيضاء؛ الفارغة = بلا تصفية) |
| `UNREAL_MCP_START_SERVER` | `true`: يشغّل المحرر بنفسه خادم `gamedev-mcp-server` المحلي بمجرد أن يُبلغ bridge عن منفذ المشروع (في وضع Custom + http فقط؛ يُحفظ باسم `startServer`) ويوقفه عند الخروج. يضبطه `unreal-mcp-cli exec` / `open --headless`. |
| `UNREAL_MCP_TRANSPORT` | `stdio` أو `http` |
| `UNREAL_MCP_LOG_LEVEL` | مستوى تفصيل السجلات |
| `UNREAL_MCP_TOOL_TIMEOUT` | المدة التي يمكن أن يستغرقها استدعاء أداة واحد في المحرر، بالمللي ثانية (الافتراضي `300000`؛ يُحفظ باسم `toolTimeoutMs`). ارفعه لمقتطفات `python-execute` الطويلة أو للعمل على الأصول. يمرر المحرر هذه القيمة مضافًا إليها 10 ثوانٍ إلى الخادم المحلي الذي يشغّله باسم `tool-timeout`؛ أما الخادم الأقدم (قبل McpPlugin 8.11.0) فيحدّ استدعاءات الأدوات بـ 5 دقائق بغض النظر عن هذه القيمة ([ARCHITECTURE §4](ARCHITECTURE.md)). الاستدعاء الذي تنتهي مهلته أو يُلغى قبل أن يبدأه المحرر يُتخطّى، ولا يُشغَّل لاحقًا. |
| `UNREAL_MCP_BRIDGE_PATH` | مسار ملف تنفيذي للعملية المرافقة — **بديل للتطوير/CI**؛ وله الأولوية على الملف المضمّن (§6). يعثر الإصدار المحزّم على العملية المرافقة المضمّنة تلقائيًا، لذا لا يضبطه المستخدمون النهائيون أبدًا؛ وتستخدمه عمليات البناء من المصدر. |
| `UNREAL_MCP_CREDENTIALS_DIR` | المسار المطلق لمجلد مخزن بيانات اعتماد الجهاز الذي يقرأه bridge (ويحدّثه) بدلًا من `~/.ai-game-dev`، ويُضبط في بيئة عملية المحرر (ولا يُقرأ من `.env`) — مسار، وليس رمزًا أبدًا؛ والقيمة النسبية تُسجَّل وتُتجاهل. مخصص لمحررات التطوير/الاختبار التي يسجّل مشغّلها الدخول بملف تعريف آخر (بيئة التطوير المحلية لتطبيق AI Game Dev App)؛ وهو نظير `GODOT_MCP_CREDENTIALS_DIR` في Godot. |
| `UNREAL_MCP_SERVER_PATH` | مسار ملف تنفيذي محلي لـ `gamedev-mcp-server` (تقرأه `unreal-mcp-cli`، لا الإضافة) — يتخطى تنزيل الخادم + فحص الإصدار (§6). |
| `AIGD_SESSION_IDLE_SECONDS` | تضبطه `unreal-mcp-cli` على المحرر الذي تشغّله: بعد هذا العدد من الثواني دون استدعاءات أدوات (تُحتسب الاستدعاءات الجارية وplay-in-editor وتجميع الأصول وإدخال المستخدم نشاطًا) يحفظ المحرر الحزم المعدّلة التي لها مسار ويُغلق. لا يُفعَّل إلا مع `AIGD_SESSION_OWNER=cli` وقيمة nonce، وما دام عقد (lease) المشروع يذكر هذا المحرر؛ وعدم ضبطه = لا إغلاق أبدًا. |
| `AIGD_SESSION_OWNER` / `AIGD_SESSION_NONCE` | `cli` وقيمة nonce من `<project>/.ai-game-dev/session.json`: يحددان أن المحرر شغّلته جلسة CLI. لا يقبل `editor-quit` إلا محرر كهذا، وعند الخروج يزيل العقد إذا كان لا يزال يحمل قيمة nonce الخاصة به. |

> **لا تُودِع `.env` في المستودع أبدًا.** قد يحتوي ملف `.env` في جذر المشروع على `UNREAL_MCP_TOKEN`، وقوالب مشاريع UE لا تأتي مع `.gitignore`. يضيف `unreal-mcp-cli configure` الملف `.env` إلى `.gitignore` في المشروع الهدف؛ وهيكل هذا المستودع نفسه يتجاهله بالفعل. ينتقل رمز IPC للعملية المرافقة عبر stdin (وليس argv أبدًا) ولا يُسجَّل أبدًا.

## استكشاف الأخطاء وإصلاحها

- ‏**`No connected clients. Retrying [1..10]` ثم HTTP 500 من خادم محلي.** كان وضع الاتصال افتراضيًا `Cloud`، فاتصلت العملية المرافقة بـ ai-game.dev بدلًا من خادمك المحلي. اضبط `UNREAL_MCP_CONNECTION_MODE=Custom` (في البيئة، أو `.env`، أو مفتاح التبديل في الواجهة).
- **لا يظهر سطر `[Unreal-MCP] plugin loaded` عند الإقلاع.** فشل تحميل وحدة المحرر — افحص Output Log بحثًا عن خطأ في `StartupModule` أو ملف `.uplugin` تالف.
- **جسم الأداة يعيد `'x' is required.` عند استدعائها عبر REST passthrough.** أرسل `-H "Content-Type: application/json"`؛ فبدونه يتجاهل الخادم جسم JSON.
- **أدوات لقطات الشاشة تعيد `Screenshot tools are unavailable: this editor runs with the null RHI (-nullrhi)`.** لا يُرسم شيء مع `-nullrhi`. أعد تشغيل المحرر دونه: بنافذة، أو دون واجهة مع `-RenderOffscreen` الذي يرسم لقطات الشاشة (مع `unreal-mcp-cli`، احذف `--nullrhi`).
- **لا توجد عملية مرافقة / العملية المرافقة تعيد التشغيل باستمرار.** افحص حالة bridge في قسم **Connection** (`Running (restarts: N)` / `Stopped`). إذا أظهر السجل `no sidecar binary resolved for rid <rid> …`، فلم يُعثر على ملف مضمّن ولا ملف منزّل ولا `UNREAL_MCP_BRIDGE_PATH`. يحتاج تثبيت Fab إلى تثبيت bridge يدويًا: يفتح **Get bridge** صفحة الإصدار، فاستخرج ملف zip الخاص بمنصتك إلى المجلد الذي يعرضه **Open bridge folder**، ثم انقر **Connect** (نسخة Fab لا تنزّل ولا تشغّل أي برنامج من تلقاء نفسها أبدًا)؛ أما CLI / الإصدار المحزّم فيضمّنه، لذا يعني ذلك هناك أن الإضافة حُزّمت دون bridge الخاص بمنصتك (أعد تثبيت البناء الصحيح)؛ وفي البناء من المصدر، اضبط `UNREAL_MCP_BRIDGE_PATH` أو شغّل `unreal-mcp-cli bootstrap-local` لبناء واحد من المصدر ([ARCHITECTURE](ARCHITECTURE.md) §6.3، §6.8).
- **المنافذ.** يستخدم IPC منفذًا حتميًا لكل مشروع ضمن `30000–39999` ويجرّب المنافذ التالية عند التعارض (ثم منفذًا مؤقتًا)؛ ويستخدم الخادم المحلي منفذًا حتميًا مشتقًا من hash ضمن `20000–29999` دون تجريب منافذ أخرى — تشتق CLI الرقم نفسه دون قراءة أي إعدادات، ويرتبط الخادم بالمنفذ المطلوب بالضبط. الرقم الدقيق مفيد لتصحيح الأخطاء فقط، وليس شرطًا.
- **السجلات.** استخدم إجراء **Open log file** في النافذة الرئيسية، أو استعلام RQL (`console | where Verbosity == "Error"`)، أو Output Log في المحرر (الفئة `LogUnrealMcp`).

</div>
