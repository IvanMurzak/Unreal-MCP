<b>[English](CONFIGURATION.md) | [中文](CONFIGURATION.zh-CN.md) | [日本語](CONFIGURATION.ja.md) | [Español](CONFIGURATION.es.md) | [فارسی](CONFIGURATION.fa.md) | [हिन्दी](CONFIGURATION.hi.md) | [العربية](CONFIGURATION.ar.md)</b>

<div dir="rtl">

# پیکربندی، بروزرسانی و رفع مشکلات

چگونه ویرایشگر را وصل کنید، پلاگین را به‌روز نگه دارید، آن را با متغیرهای محیطی تنظیم کنید و مشکلات رایج را رفع کنید. مراحل نصب در [README](README.fa.md) آمده است؛ طراحی پشت همه این‌ها در [ARCHITECTURE.md](ARCHITECTURE.md) است.

- [اجرای اول](#اجرای-اول)
- [بروزرسانی پلاگین](#بروزرسانی-پلاگین)
- [متغیرهای محیطی](#متغیرهای-محیطی)
- [رفع مشکلات](#رفع-مشکلات)

## اجرای اول

1. پنجره اصلی **AI Game Developer** را از منوی **Tools** ویرایشگر باز کنید (تب آن در دسته منوی Tools ثبت شده است).
2. **یک حالت اتصال انتخاب کنید:**
   - ‏**Cloud** (پیش‌فرض) — به [ai-game.dev](https://ai-game.dev) وصل می‌شود. روی **Authorize** کلیک کنید تا **روند device-code** در OAuth شروع شود: پنجره یک URL تأیید و یک کد کوتاه کاربر نشان می‌دهد؛ URL را باز کنید، کد را وارد کنید، تأیید کنید، و ویرایشگر مجوزدهی را کامل می‌کند. برای پاک کردن توکن ابری ذخیره‌شده از **Revoke** استفاده کنید.
   - ‏**Custom** — به یک `gamedev-mcp-server` محلی که خودتان اجرا می‌کنید (یا هر سرور سازگار دیگر) وصل می‌شود. URL سرور را وارد کنید و کلاینت هوش مصنوعی خود را به آن متصل کنید. سرور را خودتان اجرا کنید، یا بگذارید ویرایشگر آن را اجرا و متوقف کند (`UNREAL_MCP_START_SERVER`، فقط حالت Custom + http؛ [متغیرهای محیطی](#متغیرهای-محیطی) را ببینید).
3. بخش **Connection** یک نقطه وضعیت، یک برچسب وضعیت و یک دکمه Connect / Disconnect / Stop نشان می‌دهد؛ وضعیت bridge به صورت `Running (restarts: N)` یا `Stopped` نمایش داده می‌شود. با آن‌ها مطمئن شوید sidecar فعال است.
4. کلاینت هوش مصنوعی خود (Claude Code، Cursor، اپلیکیشن AI Game Developer، …) را به سرور متصل کنید. بخش **AI agents** عامل‌هایی را که اکنون متصل‌اند فهرست می‌کند؛ برای نوشتن پیکربندی کلاینت MCP از `unreal-mcp-cli setup-mcp` استفاده کنید.

تنظیمات اتصال در `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` ذخیره می‌شوند (همه قالب‌های UE پوشه `Saved/` را در gitignore دارند، بنابراین به طور پیش‌فرض توکن‌ها هرگز وارد VCS نمی‌شوند).

> **URL پین‌شده کلاینت MCP.** `unreal-mcp-cli setup-mcp <agent>` یک پیکربندی کلاینت MCP می‌نویسد که به URL ابری **پین‌شده به پروژه** یعنی `<base>/mcp/p/<pin>` اشاره می‌کند، تا عامل حتی وقتی حساب شما چند ویرایشگر را کنترل می‌کند به ویرایشگر **همین** پروژه هدایت شود. برای نوشتن URL ساده `<base>/mcp` به جای آن، `--no-pin` را بدهید. پین فقط یک بخش مسیر برای مسیریابی است — منبع OAuth همچنان `<base>/mcp` است.

> **کلید پروژه.** وقتی این دستگاه وارد حساب شده باشد (`unreal-mcp-cli login`، یا Sign in در ویرایشگر)، پیکربندی Cloud برای هر عامل یک `Authorization: Bearer agd_pk_…` هم دارد — یک کلید بدون انقضا و قابل ابطال که به پین همین پروژه متصل است — بنابراین هیچ عاملی به OAuth جداگانه نیاز ندارد. کلید در `~/.ai-game-dev/project-keys.json` ذخیره و دوباره استفاده می‌شود. برای پیکربندی فقط با URL (عامل خودش وارد می‌شود) `--oauth` را بدهید، یا برای ساخت کلید جدید و ابطال کلید قبلی `--regenerate-key` را (پنل AI agent در ویرایشگر دکمه **Regenerate key** دارد). اگر وارد نشده باشید، یا سرور نتواند کلید بسازد ⇐ پیکربندی فقط با URL.

## بروزرسانی پلاگین

بروزرسانی درجا همیشه باید شما را روی کد **جدید** قرار دهد. خطر از کامپایلر افزایشی UE است: اگر سورس پلاگین تغییر کند (فایل‌های `.cpp` جدید، یک ماژول جدید) ولی کش build قدیمی `UnrealMCP/Intermediate/` باقی بماند، UE ممکن است یک کامپایل جزئی بر اساس فهرست فایل‌های قدیمی ماژول انجام دهد و بی‌صدا شما را روی کد قدیمی/ناقص نگه دارد. هر کانال این را متفاوت مدیریت می‌کند:

- ‏**Fab / Epic Marketplace ← خودکار.** Epic Games Launcher باینری‌های پیش‌کامپایل‌شده را درجا جایگزین می‌کند؛ چیزی برای کامپایل و کشی برای پاک کردن وجود ندارد.
- ‏**`unreal-mcp-cli update` ← build تمیز خودکار.** `update` سورس پلاگین را دوباره کپی می‌کند و **به طور پیش‌فرض** پوشه قدیمی `Intermediate/` و `Binaries/` مربوط به C++ پلاگین نصب‌شده را حذف می‌کند تا UE در اجرای بعدی ویرایشگر یک کامپایل تمیز انجام دهد — بدون مراحل دستی. bridge همراه (sidecar) در `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` دست‌نخورده می‌ماند: نصب‌های release-source آن را از `Source/ThirdParty/...` تازه می‌کنند، در حالی که نصب‌های repo/dev در صورت نیاز نسخه همراه قبلی را حفظ می‌کنند. نصب‌های **junction** توسعه هرگز پاک نمی‌شوند (این کار خروجی‌های درخت سورس زنده شما را از بین می‌برد). برای صرف‌نظر از پاک کردن کش، `--no-clean` را بدهید.

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

- **کپی دستی ← کش را خودتان پاک کنید.** اگر `<YourProject>/Plugins/UnrealMCP/` را دستی بازنویسی می‌کنید، **ابتدا ویرایشگر را ببندید**، `<YourProject>/Plugins/UnrealMCP/Intermediate/` و `Binaries/` مربوط به C++ را حذف کنید (اگر bridge همراه وجود دارد `Binaries/ThirdParty/` را نگه دارید)، سپس ویرایشگر را دوباره اجرا کنید تا UE به طور تمیز کامپایل کند.

## متغیرهای محیطی

پلاگین پیکربندی را با این اولویت می‌خواند: **env پروسه ← `<Project>/.env` ← فایل پیکربندی ← پیش‌فرض‌های داخلی**. متغیرهای شناخته‌شده `UNREAL_MCP_*` (تعریف‌شده در `UnrealMCP/Source/UnrealMcpRuntime/Private/Config/UnrealMcpConfig.cpp`؛ `UNREAL_MCP_CREDENTIALS_DIR` را bridge می‌خواند و `UNREAL_MCP_SERVER_PATH` را CLI):

| متغیر | کاربرد |
| --- | --- |
| `UNREAL_MCP_CONNECTION_MODE` | `Cloud` یا `Custom`. **پیش‌فرض `Cloud` است.** |
| `UNREAL_MCP_HOST` | URL سرور (حالت Custom)، مثلاً `http://localhost:<port>` |
| `UNREAL_MCP_CLOUD_URL` | URL بک‌اند ابری (پیش‌فرض ai-game.dev) |
| `UNREAL_MCP_TOKEN` | توکن احراز هویت (توکن IPC برای sidecar / توکن سرور). **محرمانه — هرگز commit نکنید.** |
| `UNREAL_MCP_AUTH_OPTION` | `none` یا `oauth` (احراز هویت سرور محلی) |
| `UNREAL_MCP_KEEP_CONNECTED` | ذخیره/بازیابی وضعیت اتصال |
| `UNREAL_MCP_TOOLS` | جایگزینی فهرست ابزارهای فعال (لیست سفید؛ خالی = بدون فیلتر) |
| `UNREAL_MCP_START_SERVER` | `true`: وقتی bridge پورت پروژه را گزارش داد، ویرایشگر خودش `gamedev-mcp-server` محلی را اجرا می‌کند (فقط حالت Custom + http؛ به صورت `startServer` ذخیره می‌شود) و هنگام خروج آن را متوقف می‌کند. `unreal-mcp-cli exec` / `open --headless` آن را تنظیم می‌کنند. |
| `UNREAL_MCP_TRANSPORT` | `stdio` یا `http` |
| `UNREAL_MCP_LOG_LEVEL` | میزان جزئیات لاگ |
| `UNREAL_MCP_TOOL_TIMEOUT` | مدت زمانی که یک فراخوانی ابزار می‌تواند در ویرایشگر اجرا شود، به میلی‌ثانیه (پیش‌فرض `300000`؛ به صورت `toolTimeoutMs` ذخیره می‌شود). برای snippet های طولانی `python-execute` یا کار با دارایی‌ها آن را افزایش دهید. ویرایشگر این مقدار به‌علاوه 10 ثانیه را به عنوان `tool-timeout` به سرور محلی‌ای که اجرا می‌کند می‌دهد؛ یک سرور قدیمی‌تر (پیش از McpPlugin 8.11.0) فراخوانی‌های ابزار را صرف‌نظر از این مقدار به 5 دقیقه محدود می‌کند ([ARCHITECTURE §4](ARCHITECTURE.md)). فراخوانی‌ای که پیش از شروع در ویرایشگر به timeout برسد یا لغو شود، کنار گذاشته می‌شود و بعداً اجرا نمی‌شود. |
| `UNREAL_MCP_BRIDGE_PATH` | مسیر یک باینری sidecar — **جایگزین برای dev/CI**؛ بر باینری همراه اولویت دارد (§6). یک release بسته‌بندی‌شده sidecar همراه را خودکار پیدا می‌کند، بنابراین کاربران نهایی هرگز آن را تنظیم نمی‌کنند؛ build های از سورس از آن استفاده می‌کنند. |
| `UNREAL_MCP_CREDENTIALS_DIR` | مسیر مطلق پوشه مخزن اعتبارنامه دستگاه که bridge به جای `~/.ai-game-dev` می‌خواند (و تازه می‌کند)، و در محیط پروسه ویرایشگر تنظیم می‌شود (از `.env` خوانده نمی‌شود) — یک مسیر، هرگز یک توکن؛ مقدار نسبی لاگ و نادیده گرفته می‌شود. برای ویرایشگرهای توسعه/تست که launcher آن‌ها با پروفایل دیگری وارد می‌شود (محیط توسعه محلی AI Game Dev App)؛ همتای `GODOT_MCP_CREDENTIALS_DIR` در Godot. |
| `UNREAL_MCP_SERVER_PATH` | مسیر یک باینری محلی `gamedev-mcp-server` (توسط `unreal-mcp-cli` خوانده می‌شود، نه پلاگین) — دانلود سرور + بررسی نسخه را رد می‌کند (§6). |
| `AIGD_SESSION_IDLE_SECONDS` | توسط `unreal-mcp-cli` روی ویرایشگری که اجرا می‌کند تنظیم می‌شود: پس از این تعداد ثانیه بدون فراخوانی ابزار (فراخوانی‌های در جریان، play-in-editor، کامپایل دارایی‌ها و ورودی کاربر فعالیت حساب می‌شوند) ویرایشگر بسته‌های تغییرکرده‌ای را که مسیر دارند ذخیره می‌کند و خارج می‌شود. فقط همراه با `AIGD_SESSION_OWNER=cli` و یک nonce فعال می‌شود، و فقط تا وقتی lease پروژه هنوز نام این ویرایشگر را دارد؛ تنظیم‌نشده = هرگز. |
| `AIGD_SESSION_OWNER` / `AIGD_SESSION_NONCE` | `cli` و nonce فایل `<project>/.ai-game-dev/session.json`: ویرایشگری را که یک جلسه CLI اجرا کرده علامت‌گذاری می‌کنند. فقط چنین ویرایشگری `editor-quit` را می‌پذیرد، و هنگام خروج، اگر lease هنوز nonce آن را داشته باشد، lease را حذف می‌کند. |

> **هرگز `.env` را commit نکنید.** یک `.env` در ریشه پروژه می‌تواند `UNREAL_MCP_TOKEN` را نگه دارد، و قالب‌های پروژه UE هیچ `.gitignore` ی همراه ندارند. `unreal-mcp-cli configure` فایل `.env` را به `.gitignore` پروژه هدف اضافه می‌کند؛ scaffold خود این مخزن از قبل آن را gitignore کرده است. توکن IPC برای sidecar از طریق stdin (هرگز argv) منتقل می‌شود و هرگز لاگ نمی‌شود.

## رفع مشکلات

- ‏**`No connected clients. Retrying [1..10]` و سپس HTTP 500 از یک سرور محلی.** حالت اتصال به طور پیش‌فرض `Cloud` بوده، بنابراین sidecar به جای سرور محلی شما با ai-game.dev تماس گرفته است. `UNREAL_MCP_CONNECTION_MODE=Custom` را تنظیم کنید (env، `.env`، یا کلید UI).
- **خط `[Unreal-MCP] plugin loaded` هنگام راه‌اندازی دیده نمی‌شود.** ماژول ویرایشگر بارگذاری نشده است — Output Log را برای خطای `StartupModule` یا یک `.uplugin` نادرست بررسی کنید.
- **بدنه یک ابزار هنگام فراخوانی از طریق REST passthrough مقدار `'x' is required.` را برمی‌گرداند.** `-H "Content-Type: application/json"` را بفرستید؛ بدون آن سرور بدنه JSON را دور می‌اندازد.
- **ابزارهای اسکرین‌شات پیام `Screenshot tools are unavailable: this editor runs with the null RHI (-nullrhi)` را برمی‌گردانند.** با `-nullrhi` هیچ چیز رندر نمی‌شود. ویرایشگر را بدون آن دوباره اجرا کنید: پنجره‌ای، یا headless با `-RenderOffscreen` که اسکرین‌شات‌ها را رندر می‌کند (با `unreal-mcp-cli`، `--nullrhi` را حذف کنید).
- ‏**sidecar وجود ندارد / sidecar مدام از نو راه‌اندازی می‌شود.** وضعیت bridge را در بخش **Connection** بررسی کنید (`Running (restarts: N)` / `Stopped`). اگر لاگ `no sidecar binary resolved for rid <rid> …` را نشان می‌دهد، نه باینری همراه، نه باینری دانلودشده و نه `UNREAL_MCP_BRIDGE_PATH` پیدا نشده است. یک نصب Fab به نصب دستی bridge نیاز دارد: **Get bridge** صفحه release را باز می‌کند، zip مربوط به پلتفرم خود را در پوشه‌ای که **Open bridge folder** نشان می‌دهد استخراج کنید، سپس روی **Connect** کلیک کنید (نسخه Fab هرگز خودش برنامه‌ای را دانلود یا اجرا نمی‌کند)؛ یک CLI / release بسته‌بندی‌شده آن را همراه دارد، پس در آنجا یعنی پلاگین بدون bridge پلتفرم شما بسته‌بندی شده است (build درست را دوباره نصب کنید)؛ در build از سورس، `UNREAL_MCP_BRIDGE_PATH` را تنظیم کنید یا `unreal-mcp-cli bootstrap-local` را اجرا کنید تا یکی از سورس ساخته شود ([ARCHITECTURE](ARCHITECTURE.md) §6.3، §6.8).
- **پورت‌ها.** IPC از یک پورت قطعی برای هر پروژه در بازه `30000–39999` استفاده می‌کند و در صورت تداخل پورت‌های بعدی را امتحان می‌کند (و سپس یک پورت موقت)؛ سرور محلی از یک پورت hash قطعی در `20000–29999` بدون امتحان پورت‌های دیگر استفاده می‌کند — CLI همان عدد را بدون خواندن هیچ پیکربندی‌ای به دست می‌آورد، و سرور دقیقاً روی پورت درخواستی bind می‌شود. عدد دقیق فقط برای دیباگ مفید است، نه یک الزام.
- **لاگ‌ها.** از عمل **Open log file** در پنجره اصلی، یک کوئری RQL (`console | where Verbosity == "Error"`)، یا Output Log ویرایشگر (دسته `LogUnrealMcp`) استفاده کنید.

</div>
