<b>[English](RUNTIME.md) | [中文](RUNTIME.zh-CN.md) | [日本語](RUNTIME.ja.md) | [Español](RUNTIME.es.md) | [فارسی](RUNTIME.fa.md) | [हिन्दी](RUNTIME.hi.md) | [العربية](RUNTIME.ar.md)</b>

<div dir="rtl">

# استفاده در زمان اجرا (درون بازی)

به طور پیش‌فرض Unreal-MCP **ویرایشگر** را کنترل می‌کند. این پلاگین همچنین می‌تواند **درون یک بازی در حال اجرا** — PIE، Standalone یا یک build بسته‌بندی‌شده **Development** — کار کند تا یک دستیار هوش مصنوعی بازی شما را به صورت زنده کنترل کند. این همتای Unreal برای [پشتیبانی Runtime (درون بازی) در Unity-MCP](https://github.com/IvanMurzak/Unity-MCP#runtime-usage-in-game) است: معادل UE برای `UnityMcpPluginRuntime.Initialize().Build().Connect()` در Unity و نمونه Chess-bot آن با `[AiTool]`.

نقطه ورود Runtime یک `UGameInstanceSubsystem` است: [`UUnrealMcpRuntimeSubsystem`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpRuntimeSubsystem.h) (در **ماژول runtime** پلاگین به نام `UnrealMcpRuntime`). این subsystem به طور خودکار یک بار برای هر `UGameInstance` نمونه‌سازی می‌شود ولی **هرگز خودکار وصل نمی‌شود** — اتصال همیشه یک فراخوانی صریح و انتخابی است ([قرارداد امنیتی](#runtime-security-contract) را در پایین ببینید).

## سه روش اتصال

هر سه روش به همان `UUnrealMcpRuntimeSubsystem::Connect(Host, Token, Mode, bAllowRemoteHost)` می‌رسند؛ حالت اتصال به طور پیش‌فرض **Custom** است (یک سرور loopback که توسعه‌دهنده فراهم می‌کند).

‏**1. از C++** (مثلاً در `GameMode::BeginPlay` شما):

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

‏`Get(WorldContext)` یک helper ایستای `BlueprintPure` است که subsystem مربوط به game instance آن context را برمی‌گرداند (یا null). اگر هر یک از دروازه‌های امنیتی درخواست را رد کند، `Connect` مقدار `false` برمی‌گرداند (و چیزی وصل نمی‌کند) — پایین‌تر را ببینید.

‏**2. از Blueprint** — `Get Unreal MCP Runtime Subsystem` (نود `Get`، آگاه از `WorldContext`) → **`Connect`** (یک نود `BlueprintCallable` در دسته **Unreal MCP**؛ `Token` / `Mode` / `bAllowRemoteHost` پین‌های پیشرفته هستند). برای وضعیت، آن را با نود **`Disconnect`** و نود pure **`Is Connected`** همراه کنید.

‏**3. از کنسول** (برای راحتی QA، بدون کامپایل دوباره) — تا زمانی که subsystem زنده است ثبت می‌شوند:

</div>

```
UnrealMcp.Connect <host> [token]
UnrealMcp.Disconnect
```

<div dir="rtl">

مسیر کنسول همیشه از loopback و حالت Custom استفاده می‌کند.

## ابزارها، پرامپت‌ها و منابع درون بازیِ خودتان

یک بازی ابزارهای گیم‌پلی **خودش** را (و در صورت نیاز پرامپت‌ها و منابع) همراه دارد و هوش مصنوعی آن‌ها را به صورت زنده به کار می‌گیرد — معادل UE برای `WithToolsFromAssembly` / `[AiTool]` Chess-bot در Unity. هر سه را دقیقاً مانند یک افزونه ویرایشگر می‌نویسید ([EXTENSIONS.md](EXTENSIONS.md))، با دو تغییر برای یک ماژول بازی: آن را **`Type=Runtime`** کنید و به **`UnrealMcpRuntime`** وابسته کنید (نه `UnrealMcpEditor`). provider های خود را هنگام راه‌اندازی ماژول ثبت کنید، یا از wrapper های قابل کشف subsystem استفاده کنید — یک جفت register/unregister برای هر نوع:

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

نمونه کامل و قابل build در [`samples/UnrealAIRuntimeSample/`](../samples/UnrealAIRuntimeSample) است — یک پلاگین `Type=Runtime` که ابزار **`game-time-dilation`** آن مقدار `AWorldSettings::TimeDilation` دنیای زنده را می‌خواند/تنظیم می‌کند (حرکت آهسته / سریع) و در یک بازی در حال اجرا از طریق اتصال MCP در Runtime قابل فراخوانی است. (این نمونه مسیر ابزار را نشان می‌دهد؛ پرامپت‌ها و منابع از طریق همان API سه‌گانه wrapper که در بالا آمد ثبت می‌شوند و خانواده‌های اصلی ارائه‌شده مرجع قابل اجرا هستند.) جزئیات سمت نویسنده (قرارداد، چرخه عمر، ترتیب، جداسازی) در [بخش Runtime usage در `docs/EXTENSIONS.md`](EXTENSIONS.md#runtime-usage-in-game-extensions) آمده است.

## مجموعه ابزارهای Runtime در برابر ابزارهای فقط‌ویرایشگر

یک اتصال Runtime دقیقاً **یک** ابزار داخلی دارد: **`ping`** (بررسی زنده بودن). `ping` یک [ابزار سیستمی](TOOLS.fa.md) است، بنابراین در `POST /api/system-tools/ping` پاسخ می‌دهد و به عامل هوش مصنوعی معرفی نمی‌شود — یعنی بازی بسته‌بندی‌شده‌ای که خودش چیزی ثبت نکند فهرست ابزار **خالی** نشان می‌دهد، که توصیف صادقانه‌ای از آن است. هر کاری که یک عامل هوش مصنوعی در Runtime می‌تواند انجام دهد **با ابزارهای خودتان** است: ابزارهای خود را از طریق گذرگاه افزونه بالا (`RegisterToolProvider`) ثبت کنید تا به طور عادی ظاهر شوند.

همه خانواده‌های توسعه موتور — لایه داده RQL (`rql-query` / `rql-mutate` / `rql-schema` روی اکتورها، کامپوننت‌ها، دارایی‌ها، لول‌ها، کنسول و وضعیت ویرایشگر)، همه ابزارهای اسکرین‌شات، به‌علاوه ساخت Blueprint، ویرایش و کامپایل سورس C++، `console-run-command`، `unreal-tool-list`، `python-execute` و ابزارهای Fab — **فقط در ویرایشگر** کار می‌کنند ([ابزارهای ویرایشگر](TOOLS.fa.md)). آن‌ها ویرایشگر را کنترل می‌کنند و چند تا از آن‌ها در رده RCE هستند (مثلاً `rql-mutate`، `console-run-command`، `python-execute`)، بنابراین به طور پیش‌فرض در بازی منتشرشده کامپایل نمی‌شوند. در بازی بسته‌بندی‌شده ویرایشگری وجود ندارد، پس سطح داخلی Runtime عمداً فقط `ping` و هر ابزاری است که بازی شما ثبت کند.

## <a id="editor-only-exclude-from-packaged-games"></a>فقط ویرایشگر — خارج کردن Unreal-MCP از بازی‌های بسته‌بندی‌شده

اگر Unreal-MCP را فقط برای **ابزارهای هوش مصنوعی درون ویرایشگر Unreal** می‌خواهید و قصد دارید یک بازی بسته‌بندی‌شده **بدون هیچ ردپایی از Unreal-MCP** منتشر کنید — بدون ماژول runtime، بدون باینری sidecar مبتنی بر `.NET` — پلاگین را با یک **`TargetDenyList`** در **`.uproject` پروژه مصرف‌کننده خودتان** (نه `.uplugin` پلاگین) به ویرایشگر محدود کنید:

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

‏UE مقادیر `TargetDenyList` / `TargetAllowList` را روی ارجاع به پلاگین رعایت می‌کند (`PluginReferenceDescriptor::IsEnabledForTarget`): پلاگین در هدف **Editor** فعال می‌ماند، ولی **از build های بسته‌بندی‌شده `Game` / `Client` / `Server` حذف می‌شود** — بنابراین نه ماژول زیرساخت runtime و نه sidecar همراه `unreal-mcp-bridge` در محصول منتشرشده شما stage یا کامپایل نمی‌شوند. ماژول‌های نوع Editor (`UnrealMcpEditor`) در هر صورت از build بازی حذف می‌شوند؛ deny-list ماژول زیرساخت `UnrealMcpRuntime` و `RuntimeDependencies` مربوط به sidecar آن را هم حذف می‌کند (§12.5) و **ردپای کامپایل‌شده صفر** به شما می‌دهد.

> **هشدار — وابستگی مستقیم ماژول ایجاد نکنید.** یک `TargetDenyList` فقط ماژول‌های *خود* پلاگین را حذف می‌کند. اگر یکی از ماژول‌های **بازی** شما `UnrealMcpRuntime` (یا هر ماژول `UnrealMcp*`) را به `PublicDependencyModuleNames` / `PrivateDependencyModuleNames` در `*.Build.cs` خود اضافه کند، یا یک header از `UnrealMcp*` را `#include` کند، آن وابستگی مستقیم **deny-list را لغو می‌کند** و UBT ماژول runtime را در هر صورت در بازی شما کامپایل می‌کند. یک پروژه کاملاً فقط‌ویرایشگر به هیچ چیزی از پلاگین ارجاع نمی‌دهد، بنابراین ذاتاً تمیز می‌ماند — فقط وقتی واقعاً [استفاده Runtime (درون بازی)](#استفاده-در-زمان-اجرا-درون-بازی) را می‌خواهید وابستگی ماژول اضافه کنید.

برای استفاده **Runtime (درون بازی)**، deny-list را **نگذارید**: ماژول زیرساخت `UnrealMcpRuntime` + ابزار داخلی `ping` در build بسته‌بندی‌شده قرار می‌گیرند (§12.5) و ابزارهای گیم‌پلی خود را از طریق `IUnrealMcpToolProvider` ثبت می‌کنید — [سه روش اتصال](#سه-روش-اتصال) و [EXTENSIONS.md](EXTENSIONS.md) را ببینید. این دو با هم جمع نمی‌شوند: deny-list برای پروژه فقط‌ویرایشگر، بدون deny-list برای بازی‌ای که میزبان یک اتصال زنده MCP است.

## <a id="runtime-security-contract"></a>قرارداد امنیتی

یک اتصال Runtime **کنترل از راه دور یک بازی در حال اجرا** است (ایجاد اکتور، نوشتن هر property ی reflect‌شده، CVar های دلخواه از طریق `console-run-command`، فراخوانی `UFunction` ها) — اگر در محصول منتشرشده قابل دسترسی بود، در رده RCE قرار می‌گرفت. بنابراین سطح Runtime با **پنج لایه محافظت** قفل شده است ([`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §12.8) که همه درون `Connect()` اعمال می‌شوند:

1. **فقط با انتخاب خودتان.** subsystem خودکار نمونه‌سازی می‌شود ولی **هرگز خودکار وصل نمی‌شود** — هیچ اتصال خودکاری هنگام بارگذاری و هیچ اتصال خودکاری از config asset وجود ندارد. اتصال فقط و فقط با یک فراخوانی صریح `Connect()` رخ می‌دهد.
2. **کلید قطع، پیش‌فرض خاموش.** `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled` (Project Settings → Plugins → **Unreal MCP (Runtime)**) به طور پیش‌فرض **`false`** است. تا وقتی خاموش است، هر `Connect()` رد می‌شود و هیچ sidecar ی اجرا نمی‌شود. این یک تنظیم پیکربندی **Game** است (`DefaultGame.ini`، بخش `[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]`)، بنابراین به build بسته‌بندی‌شده منتقل می‌شود، جایی که این دروازه باید اثر کند. روشن کردن آن **باعث اتصال خودکار نمی‌شود** — همچنان یک `Connect()` صریح لازم است.
3. **دروازه Shipping.** پرچم `bUnrealMcpAllowShipping` در `*.Build.cs` به طور پیش‌فرض **`false`** است (→ `UNREAL_MCP_ALLOW_SHIPPING=0`)؛ در build **Shipping**، `Connect()` لاگ می‌نویسد و `false` برمی‌گرداند، مگر اینکه آن پرچم عمداً در build گنجانده شده باشد. build های Development / PIE تحت تأثیر نیستند.
4. **میزبان loopback به طور پیش‌فرض.** `Connect()` هر `Host` غیر loopback را رد می‌کند، مگر اینکه فراخوان صریحاً `bAllowRemoteHost = true` بدهد. پیش‌فرض، اتصال را روی `localhost` / `127.0.0.0/8` / `::1` نگه می‌دارد.
5. **IPC روی loopback + توکن یک‌بارمصرف از stdin.** IPC بین پلاگین ⇄ sidecar فقط روی loopback است و با یک توکن یک‌بارمصرف احراز هویت می‌شود که از طریق **stdin، هرگز argv** تحویل داده می‌شود و هرگز لاگ نمی‌شود (همان مدل ویرایشگر، ARCHITECTURE §1.4).

علاوه بر این، مسیر Runtime **فقط برای دسکتاپ** است (Win64 / Mac / Linux): اجرای پروسه sidecar مبتنی بر .NET یعنی `unreal-mcp-bridge` روی کنسول‌ها یا موبایل ممکن نیست، بنابراین آن پلتفرم‌ها اصلاً سطح MCP در Runtime ندارند.

> **راهنمای سرتاسری.** راهنمای کامل اپراتور — فعال کردن کلید قطع، اتصال از PIE و از یک build بسته‌بندی‌شده Development، و آزمودن ابزارها روی اتصال زنده — در [`docs/RUNTIME-E2E.md`](RUNTIME-E2E.md) است. بخش قطعی آن (دروازه‌های امنیتی، تعویض world-resolver، ایمنی connect/disconnect/orphan) با spec های headless Automation تثبیت شده است.

</div>
