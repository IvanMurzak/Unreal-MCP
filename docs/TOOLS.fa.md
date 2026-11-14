<b>[English](TOOLS.md) | [中文](TOOLS.zh-CN.md) | [日本語](TOOLS.ja.md) | [Español](TOOLS.es.md) | [فارسی](TOOLS.fa.md) | [हिन्दी](TOOLS.hi.md) | [العربية](TOOLS.ar.md)</b>

<div dir="rtl">

# ابزارهای Unreal-MCP

فهرست مرجع ابزارهایی که پلاگین ویرایشگر **UnrealMCP** ثبت می‌کند. Unreal-MCP با **ابزارهای داخلی، دسته‌بندی‌شده در خانواده‌ها،** ارائه می‌شود که عامل هوش مصنوعی شما می‌تواند فراخوانی کند، به‌علاوه **ابزارهای سیستمی** که عامل نمی‌تواند فراخوانی کند (زیرساخت میزبان برای CLI و اپلیکیشن دسکتاپ). شناسه ابزارها به شکل kebab-case است (`rql-query`، `blueprint-compile`)، همان قراردادی که Unity-MCP و Godot-MCP دارند. [افزونه‌ها](#extensions) و [پلاگین‌های خودتان](EXTENSIONS.md) ابزارهای بیشتری اضافه می‌کنند.

| خانواده | ابزارها |
| --- | --- |
| لایه داده RQL | `rql-query`، `rql-mutate`، `rql-schema` |
| Blueprint | `blueprint-*`: create, get, add-component, remove-component, add-variable, modify-variable, set-default, add-function, add-event, compile, spawn |
| سورس / C++ | `source-*`: read, create-class, update, delete, list, compile |
| اسکرین‌شات | `screenshot-*`: viewport, game-view, camera, isolated |
| Fab | `fab-search`، `fab-library`، `fab-add` |
| کنسول | `console-run-command` |
| Python | `python-execute` |
| فهرست ابزارها | `unreal-tool-list` |
| **سیستمی** (به هوش مصنوعی نشان داده نمی‌شود) | `ping`، `unreal-skill-create`، `unreal-skill-generate`، `editor-quit` |

> **منبع حقیقت.** ابزارهای استاندارد: `UnrealMcp*Tools.cpp` در `UnrealMCP/Source/UnrealMcpEditor/Private/Tools/` (به جز `UnrealMcpSkillTools.cpp`) و `UnrealMcpRqlTools.cpp` در `UnrealMcpRql`. ابزارهای سیستمی: `UnrealMcpPingTool.cpp`، `UnrealMcpSkillTools.cpp`، `UnrealMcpEditorSession.cpp` و `SkillGenerateTool.cs` در sidecar. هر وقت ثبت یک ابزار تغییر کرد، این صفحه را به‌روز کنید.

همه ابزارهای استاندارد **فقط در ویرایشگر** کار می‌کنند. یک اتصال Runtime (درون بازی) فقط `ping` و ابزارهایی را که بازی شما ثبت می‌کند ارائه می‌دهد: [RUNTIME.md](RUNTIME.fa.md) را ببینید.

## خانواده‌ها با جزئیات

<details>
  <summary><b>RQL — لایه داده</b></summary>

یک زبان کوئری برای کل ویرایشگر به جای یک ابزار برای هر عملیات ([`docs/rql.md`](rql.md)): اکتورها، کامپوننت‌ها و دارایی‌ها را پیدا کنید، اعضا را بخوانید، فیلتر و تجمیع کنید و در یک فراخوانی تغییرشان دهید. این زبان جایگزین ابزارهای قبلی اکتور، شیء، دارایی، لول، وضعیت ویرایشگر، انتخاب، لاگ کنسول و reflection شده است.

| شناسه ابزار | کار آن |
| --- | --- |
| `rql-query` | کوئری فقط‌خواندنی RQL روی ویرایشگر زنده (`actors | where $ is PointLight | select $ref Label Intensity`)؛ نتایج جدولی فشرده |
| `rql-mutate` | کوئری RQL که با `set` / `call` تمام می‌شود (`... | set Location = "(0, 0, 100)"`، `editor | call SpawnActor("PointLight")`): همه یا هیچ، یک مرحله undo، پیش‌نمایش با `dry_run` |
| `rql-schema` | منابع، توابع و کلاس‌های موجود در دنیای ویرایشگر؛ `type` اعضا و متدهای یک نوع را فهرست می‌کند |

فراتر از propertyهای reflect‌شده، RQL به ردیف‌های DataTable (`dataTables`، `Rows["Sword"].Damage`)، gameplay tag ها به صورت متن (`gameplayTags`، `set Tags = ["Ability.Fire"]`)، Project Settings ذخیره‌شده در ini خودشان (`unreal.settings("Maps & Modes")`)، گراف‌های Blueprint (`EventGraph | call AddCallFunction("PrintString")`، اتصال پین‌ها، `call Compile()`)، درخت‌های Widget Blueprint (`call AddWidget("Button", "Ok", parent)`) و Level Sequence ها (binding ها، track ها، `call AddKey("Location.X", 30, 100)`) دسترسی دارد.

‏Skill با نام `unreal-rql` (که `unreal-skill-generate` آن را می‌نویسد) این زبان را آموزش می‌دهد و آنچه افزونه‌های نصب‌شده اضافه می‌کنند را فهرست می‌کند.

</details>

<details>
  <summary><b>خانواده Blueprint — مهم‌ترین بخش Unreal</b></summary>

| شناسه ابزار | کار آن |
| --- | --- |
| `blueprint-create` | ساخت یک کلاس Blueprint جدید از مسیر یک `UClass` والد |
| `blueprint-get` | خلاصه گراف برای بررسی توسط LLM (متغیرها، کامپوننت‌ها، توابع/رویدادها، زنجیره والدها) |
| `blueprint-add-component` | افزودن یک کامپوننت از طریق Simple Construction Script |
| `blueprint-remove-component` | حذف یک کامپوننت SCS |
| `blueprint-add-variable` | افزودن یک متغیر عضو با نوع مشخص |
| `blueprint-modify-variable` | تغییر یک متغیر عضو |
| `blueprint-set-default` | ویرایش یک property در CDO (پیش‌فرض کلاس) |
| `blueprint-add-function` | افزودن اسکلت یک تابع (نودهای ورودی/نتیجه متصل‌شده) |
| `blueprint-add-event` | افزودن/بایند کردن اسکلت یک رویداد (BeginPlay، Tick، ورودی، …) |
| `blueprint-compile` | کامپایل Blueprint و برگرداندن **فهرست ساختاریافته خطاها/هشدارها** (حلقه بازخورد هوش مصنوعی) |
| `blueprint-spawn` | قرار دادن نمونه‌ای از Blueprint در لول فعلی |

</details>

<details>
  <summary><b>خانواده کنسول</b></summary>

| شناسه ابزار | کار آن |
| --- | --- |
| `console-run-command` | اجرای یک دستور کنسول / CVar |

لاگ‌ها، وضعیت PIE و انتخاب از طریق RQL در دسترس‌اند: `console | where Verbosity == "Error"`، `editor | set IsPlaying = true`، `editor | set Selection = [...]`. تست بازی، بازی را درون همان فراخوانی شبیه‌سازی می‌کند (قطعی و مستقل از tick شدن ویرایشگر بین فراخوانی‌ها): `editor | call StartPlay()`، `editor | call AdvancePlay(1.5, 0, "W, LeftShift")` (کلیدها را 1.5 ثانیه نگه می‌دارد و موقعیت و سرعت pawn، میزان جابه‌جایی آن در این فراخوانی، حالت حرکت و mapping context های اعمال‌شده را برمی‌گرداند)، `editor | call StopPlay()`؛ افزونه Enhanced Input هم `enhancedInput | call InjectInput(action, value, seconds)` را اضافه می‌کند ([docs/rql.md](rql.md#play-testing)).

</details>

<details>
  <summary><b>خانواده فهرست ابزارها</b></summary>

| شناسه ابزار | کار آن |
| --- | --- |
| `unreal-tool-list` | فهرست همه ابزارها، از جمله ابزارهای پنهان از `tools/list` (با علامت hidden؛ با فراخوانی با نام همچنان اجرا می‌شوند)؛ `filter` با regex، `includeDescription`، `includeInputs` |

</details>

<details>
  <summary><b>خانواده Python</b></summary>

| شناسه ابزار | کار آن |
| --- | --- |
| `python-execute` | اجرای Python در ویرایشگر (کل API `unreal`، از پیش import‌شده): دستورهای ساده، آخرین عبارت نتیجه است، `print` به صورت لاگ برمی‌گردد، خطاها به خطوط `snippet.py` اشاره می‌کنند؛ `include` فایل‌ها/پوشه‌های کمکی را از `<Project>/.ai-game-dev/snippets` اضافه می‌کند؛ `timeout` (ثانیه، پیش‌فرض 30) یک snippet گیرکرده را متوقف می‌کند |

از PythonScriptPlugin موتور (که Unreal-MCP فعالش می‌کند؛ فقط در ویرایشگر، هرگز در بازی بسته‌بندی‌شده) و runtime [Code-Exec](https://github.com/IvanMurzak/Code-Exec) که در `UnrealMCP/Content/Python` گنجانده شده استفاده می‌کند. سیم‌کشی گراف Blueprint در Python در دسترس نیست: برای آن از ابزارهای Blueprint استفاده کنید.

</details>

<details>
  <summary><b>خانواده Fab — جستجو و افزودن محتوای Fab</b></summary>

| شناسه ابزار | کار آن |
| --- | --- |
| `fab-search` | جستجو در مارکت‌پلیس [Fab](https://www.fab.com) (`query`، `listingType`، `freeOnly`، `sort`، صفحه‌بندی)؛ uid، عنوان، نوع، قیمت و فرمت‌های هر listing را برمی‌گرداند |
| `fab-library` | فهرست دارایی‌های Fab که کاربر واردشده مالک آن‌هاست، همراه با محلی که هر کدام قبلاً در آن import شده است؛ `filter` در کل کتابخانه جستجو می‌کند |
| `fab-add` | دانلود listing ای که کاربر مالک آن است و import آن مانند *Add to Project* در پنل Fab (پک‌های Unreal Engine، Megascans با `quality`، glTF/FBX)؛ پوشه محتوا را برمی‌گرداند و listing هایی را که از قبل در پروژه هستند رد می‌کند |

این ابزارها پلاگین Fab خود موتور را به کار می‌گیرند (از UE 5.5 به طور پیش‌فرض فعال است): ورود Epic و روندهای import آن را. اولین `fab-library` / `fab-add` بدون ورود، صفحه ورود Epic را در مرورگر باز می‌کند (باز کردن Fab از نوار ابزار ویرایشگر هم با یک ورود Epic به‌خاطرسپرده‌شده وارد می‌شود). خرید، یا افزودن یک listing رایگان به کتابخانه، در fab.com انجام می‌شود. پروژه‌های کامل و پلاگین‌های کد موتور از Epic Games Launcher نصب می‌شوند، نه اینکه به پروژه اضافه شوند.

</details>

<details>
  <summary><b>خانواده سورس / C++</b></summary>

| شناسه ابزار | کار آن |
| --- | --- |
| `source-read` | خواندن یک فایل سورس C++ پروژه (بخش‌بخش) |
| `source-create-class` | ایجاد اسکلت یک کلاس C++ جدید (header + cpp از قالب‌ها) |
| `source-update` | ویرایش یک فایل سورس |
| `source-delete` | حذف یک فایل سورس |
| `source-list` | فهرست فایل‌های سورس ماژول |
| `source-compile` | کامپایل C++ پروژه (Live Coding در صورت فعال بودن، وگرنه UBT) با گزارش ساختاریافته خطاها؛ برای کلاس‌های جدید یا تعریف‌های reflect‌شده تغییرکرده (`restartRequired`) می‌گوید چگونه دوباره build کنید و ویرایشگر را از نو اجرا کنید |

همه عملیات فایل به `<Project>/Source/` محدود هستند.

</details>

<details>
  <summary><b>خانواده اسکرین‌شات / گرفتن تصویر ویوپورت</b></summary>

| شناسه ابزار | کار آن |
| --- | --- |
| `screenshot-viewport` | گرفتن تصویر از ویوپورت فعال ویرایشگر؛ `savePath` اختیاری PNG را در پروژه هم ذخیره می‌کند |
| `screenshot-game-view` | گرفتن تصویر از نمای PIE / بازی؛ `savePath` اختیاری مانند بالا |
| `screenshot-camera` | رندر از یک اکتور دوربین مشخص‌شده با `USceneCaptureComponent2D`؛ `savePath` اختیاری مانند بالا |
| `screenshot-isolated` | رندر یک اکتور به صورت ایزوله (SceneCapture2D موقت + فهرست show-only)؛ `savePath` اختیاری مانند بالا |

تصاویر به صورت **PNG در قالب محتوای تصویری MCP** (base64) برگردانده می‌شوند تا LLM بتواند رندر را مستقیم بررسی کند. هر capture در لحظه فراخوانی یک فریم جدید رندر می‌کند، حتی وقتی پنجره ویرایشگر در پس‌زمینه یا کوچک‌شده است، و متن آن می‌گوید آیا فریم تازه است و آیا افکت‌ها و انیمیشن tick می‌شدند. تا وقتی یک عامل هوش مصنوعی متصل است، پلاگین ویرایشگر را در پس‌زمینه (حتی کوچک‌شده) با نرخ کامل در حال tick و رندر نگه می‌دارد (گزینه "Use Less CPU when in Background" فقط برای همان جلسه نادیده گرفته می‌شود؛ ترجیح ذخیره‌شده شما تغییر نمی‌کند). ابعاد محدود می‌شوند (پیش‌فرض 1024، سقف قطعی 2048 برای هر ضلع). گرفتن پیکسل به ویرایشگری نیاز دارد که رندر کند: پنجره‌ای، یا headless با `-RenderOffscreen` (پیش‌فرض `unreal-mcp-cli`). با `-nullrhi` هیچ چیزی رندر نمی‌شود، پس این ابزارها خطایی برمی‌گردانند که `-nullrhi` و راه‌حل را نام می‌برد.

</details>

<details>
  <summary><b>ابزارهای سیستمی — در اختیار هوش مصنوعی شما قرار نمی‌گیرند</b></summary>

این‌ها **ابزارهای سیستمی** هستند: زیرساخت میزبان که `unreal-mcp-cli` و اپلیکیشن دسکتاپ مستقیماً از طریق `POST /api/system-tools/<name>` به کار می‌گیرند. این ابزارها عمداً در `tools/list` پروتکل MCP نیستند، بنابراین هرگز در یک جلسه هوش مصنوعی ظاهر نمی‌شوند (و توکنی مصرف نمی‌کنند) — همان تقسیمی که Unity و Godot برای ابزارهای سیستمی خود دارند.

| شناسه ابزار | کار آن |
| --- | --- |
| `ping` | بررسی زنده بودن — یک رفت‌وبرگشت در زنجیره پلاگین ⇄ sidecar ⇄ سرور |
| `unreal-skill-create` | ساخت یک ابزار MCP جدید به صورت یک فایل C++ در ماژول ویرایشگر پلاگین. پیش از قابل فراخوانی شدن ابزار جدید **باید ویرایشگر دوباره build شود** (Live Coding، یا build دوباره پروژه) — نتیجه صریحاً این را می‌گوید. به نصب پلاگین از سورس نیاز دارد |
| `unreal-skill-generate` | تولید دوباره همه `SKILL.md` ها از ابزارهای ثبت‌شده در یک پوشه skills نسبت به پروژه (پیش‌فرض `.claude/skills`) |
| `editor-quit` | ذخیره بسته‌های تغییرکرده‌ای که مسیر دارند و بستن ویرایشگری که یک جلسه CLI اجرا کرده است (`unreal-mcp-cli close --owned-only`). تا وقتی `nonce` با lease جلسه مطابقت نداشته باشد امتناع می‌کند، بنابراین هرگز ویرایشگری را که خودتان باز کرده‌اید نمی‌بندد |

</details>

## <a id="extensions"></a>افزونه‌ها

اگر پروژه شما از یکی از سیستم‌های موتور زیر استفاده می‌کند یک افزونه نصب کنید، یا [افزونه خودتان را بسازید](EXTENSIONS.md). هر افزونه یک ماژول RQL است: ابزارهای اصلی `rql-query` / `rql-mutate` / `rql-schema` را گسترش می‌دهد و راهنمای خود را به skill `unreal-rql` اضافه می‌کند، بنابراین ابزار MCP جداگانه‌ای در `tools/list` ظاهر نمی‌شود.

</div>

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas "<UEProject>"
```

<div dir="rtl">

افزونه‌ها به Unreal Engine 5.7+، Unreal MCP 0.26.0+ در همان پروژه و یک toolchain برای C++ نیاز دارند: نسخه‌ها به صورت سورس منتشر می‌شوند و برای پروژه شما کامپایل می‌شوند (در باز شدن بعدی ویرایشگر، یا همین حالا با `--build`). شناسه نصب `com.ivanmurzak.unreal-ai-<id>` است که `<id>` آن از جدول زیر می‌آید. گزینه‌های نصب (`--version`، `--build`، `--source` آفلاین): [نصب یک افزونه در پروژه](EXTENSIONS.md#installing-an-extension-into-a-project).

| افزونه (شناسه) | توضیح |
| --- | --- |
| **[AI&nbsp;Control&nbsp;Rig](../extensions/UnrealAIControlRig/README.md)**<br>`control-rig` | ساخت rig با استخوان‌ها، کنترل‌ها و یک حل‌کننده IK دواستخوانی؛ کوئری روی Control Rig blueprint ها و کامپوننت‌ها |
| **[AI&nbsp;Enhanced&nbsp;Input](../extensions/UnrealAIEnhancedInput/README.md)**<br>`enhanced-input` | کوئری روی Input Action ها و Mapping Context ها، ساخت action و context، نگاشت کلیدها |
| **[AI GAS](../extensions/UnrealAIGAS/README.md)**<br>`gas` | ساخت Gameplay Ability System بدون C++: افکت‌ها (هزینه، cooldown، آسیب، بازیابی)، Blueprint های ability، کامپوننت‌های ability system با یک attribute set آماده Health / Mana / Stamina، اتصال ورودی‌ها، بررسی‌های Play In Editor |
| **[AI Landscape](../extensions/UnrealAILandscape/README.md)**<br>`landscape` | Landscape و Water: ساخت، حجاری و رنگ‌آمیزی landscape؛ ساخت اقیانوس، دریاچه و رودخانه؛ کوئری روی landscape ها، water body ها و water zone ها |
| **[AI Niagara](../extensions/UnrealAINiagara/README.md)**<br>`niagara` | جلوه‌های بصری Niagara: کوئری روی سیستم‌ها و emitter های آن‌ها، قرار دادن سیستم‌ها در لول |
| **[AI PCG](../extensions/UnrealAIPCG/README.md)**<br>`pcg` | Procedural Content Generation: ساخت گراف‌های PCG (نودها، اتصال‌ها، تنظیمات، پارامترها)، قرار دادن آن‌ها روی volume ها، تولید پراکنش‌ها (جنگل، صخره، اشیای صحنه) و خواندن تعدادها |

## نمایش هر ابزار

هر ابزار یک چک‌باکس **Visible** (قابل مشاهده) در پنجره **MCP Tools** دارد — تب مستقل **MCP Tools** (ثبت‌شده در منوی **Tools** ویرایشگر). پنجره عنوان، خانواده و توضیح هر ابزار را همراه با یک خط خلاصه "N / M tools visible" نشان می‌دهد. **Visible** فقط تعیین می‌کند که ابزار **در `tools/list` پروتکل MCP فهرست شود** یا نه، یعنی عامل‌های هوش مصنوعی آن را ببینند یا نه. این یک کنترل دسترسی نیست: ابزار پنهان وقتی کلاینتی آن را با نام فراخوانی کند (`tools/call`) همچنان اجرا می‌شود، و `unreal-tool-list` ابزارهای پنهان را هم فهرست می‌کند (با علامت hidden) تا عامل بتواند آن‌ها را پیدا کند.

دو تنظیم اعمال می‌شود ([ARCHITECTURE](ARCHITECTURE.md) §7/§8 را ببینید):

- **لیست سفید `enabledTools`** (قابل جایگزینی با `UNREAL_MCP_TOOLS`) تعیین می‌کند که آیا ابزار **اصلاً ارائه شود** — وقتی خالی نباشد، فقط ابزارهای فهرست‌شده ارائه می‌شوند و هر ابزار دیگری نه فهرست می‌شود و نه قابل فراخوانی است (`Execute()` آن را رد می‌کند)؛ خالی یعنی «بدون فیلتر»؛
- **فهرست `hiddenTools`** — همان چک‌باکس‌های Visible که در UI تغییر می‌دهید — یک ابزار ارائه‌شده را از `tools/list` پنهان می‌کند.

هر دو بین جلسه‌های ویرایشگر ذخیره می‌شوند و پس از hot-reload یک افزونه باقی می‌مانند (ابزاری که دوباره ثبت می‌شود تنظیم حفظ‌شده را به ارث می‌برد، بنابراین یک rebuild هرگز ابزاری را که پنهان کرده‌اید نشان نمی‌دهد). فایل پیکربندی نسخه‌های قدیمی‌تر پلاگین همچنان کار می‌کند: فهرست `disabledTools` آن به عنوان `hiddenTools` خوانده می‌شود و ذخیره بعدی `hiddenTools` را می‌نویسد.

همه تنظیمات اتصال در بخش Connection پنجره اصلی و یگانه **AI Game Developer** قرار دارند (تب Settings جداگانه یا صفحه Project-Settings وجود ندارد — همانند Unity-MCP). پنجره‌های **MCP Prompts** و **MCP Resources** پیاده‌سازی شده‌اند ولی در این نسخه خالی ارائه می‌شوند — هر کدام یک پیام کم‌رنگ حالت خالی نشان می‌دهد (خلاصه "N / M visible" فقط مخصوص پنجره Tools است).

</div>
