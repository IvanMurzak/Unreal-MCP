<div align="center">

<img src="img/readme/hero.svg" alt="Unreal MCP — به Claude، Cursor، Copilot یا هر عامل MCP اجازه دهید ویرایشگر Unreal را بررسی و کنترل کند" width="100%"/>

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

‏**Unreal MCP** (AI Game Developer برای Unreal) هر عامل هوش مصنوعی سازگار با MCP را به **ویرایشگر Unreal** وصل می‌کند. با زبان ساده درخواست کنید تا عامل اکتور بسازد، لول‌ها را ویرایش کند، Blueprint بنویسد و کامپایل کند، کد C++ را ویرایش و کامپایل کند، دارایی‌ها را مدیریت کند و کارش را با اسکرین‌شات بررسی کند. این پروژه همتای Unreal برای [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) و [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) است و از همان بک‌اند ابری ([ai-game.dev](https://ai-game.dev)) یا [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) خودتان استفاده می‌کند.

</div>

<p align="center">
  <img src="img/readme/architecture.svg" alt="یک عامل هوش مصنوعی با پروتکل MCP با سرور صحبت می‌کند؛ سرور از طریق SignalR پیام‌ها را به sidecar به نام unreal-mcp-bridge می‌رساند و sidecar از طریق TCP روی loopback به پلاگین C++ به نام UnrealMCP در ویرایشگر Unreal دسترسی دارد" width="100%"/>
</p>

<div dir="rtl">

ویرایشگر Unreal با C++ نوشته شده است، بنابراین میزبان MCP مبتنی بر .NET در کنار آن به صورت یک **sidecar** (فرایند همراه) کوچک (`unreal-mcp-bridge`) اجرا می‌شود که پلاگین آن را برایتان اجرا، پایش و در صورت نیاز دوباره راه‌اندازی می‌کند. جزئیات طراحی: [docs/ARCHITECTURE.md](ARCHITECTURE.md).

‏**وضعیت:** [آخرین نسخه](https://github.com/IvanMurzak/Unreal-MCP/releases/latest) (پلاگین، sidecar و CLI یک شماره نسخه مشترک دارند) · Unreal Engine 5.5–5.8 · sidecar برای Windows x64، macOS (Intel و Apple silicon) و Linux x64 ساخته می‌شود · هر نسخه روی Windows (UE 5.5–5.8) و macOS (UE 5.7، 5.8) تست می‌شود · در [Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) و [GitHub Releases](https://github.com/IvanMurzak/Unreal-MCP/releases) در دسترس است.

## ویژگی‌ها

‏✅ ‏**نوشتن C++** — ساخت اسکلت کلاس، ویرایش سورس، کامپایل؛ خطاها با فایل، خط و شدت برمی‌گردند\
‏✅ ‏**Blueprint** — ساخت، ویرایش و کامپایل، با خطاهای ساختاریافته‌ای که عامل می‌تواند رفع کند\
‏✅ ‏**لایه داده RQL** — کوئری و تغییر اکتورها، دارایی‌ها و لول‌ها با یک زبان ([RQL](rql.md))\
‏✅ ‏**بازخورد بصری** — اسکرین‌شات از ویوپورت، نمای بازی، دوربین و اکتور ایزوله\
‏✅ ‏**Python** — اجرای Python ویرایشگر با کل API `unreal`\
‏✅ ‏**Fab** — جستجو در Fab، فهرست کتابخانه شما، افزودن محتوایی که مالک آن هستید\
‏✅ ‏**افزونه‌ها** — GAS، Niagara، PCG، Landscape، Control Rig، Enhanced Input\
‏✅ ‏**ابزارهای خودتان** — افزودن ابزار، پرامپت و منبع از هر پلاگینی ([راهنما](EXTENSIONS.md))\
‏✅ ‏**حالت Runtime** — در صورت تمایل، یک بازی در حال اجرا را کنترل کنید ([docs/RUNTIME.md](RUNTIME.fa.md))\
‏✅ ‏**ابری یا محلی** — `ai-game.dev` بدون هیچ تنظیمی، یا یک `gamedev-mcp-server` روی میزبان خودتان

‏30 ابزار داخلی در 8 خانواده؛ فهرست کامل در [docs/TOOLS.md](TOOLS.fa.md) است.

### چه چیزهایی می‌توانید بخواهید

| از عامل خود بخواهید | ابزارهایی که استفاده می‌کند |
| --- | --- |
| «یک کلاس اکتور `AHealthPickup` در C++ اضافه کن، کامپایلش کن و خطاها را رفع کن» | `source-create-class`، `source-update`، `source-compile` |
| «یک Blueprint برای دری بساز که با هم‌پوشانی باز شود، کامپایلش کن و در لول قرارش بده» | `blueprint-*` |
| «درخت‌های این لول را بشمار و همه چراغ‌های نقطه‌ای را ۱ متر بالا ببر» | `rql-query`، `rql-mutate` |
| «از دوربین بازیکن اسکرین‌شات بگیر و نورپردازی را بررسی کن» | `screenshot-*` |
| «یک بسته سنگ رایگان در Fab پیدا کن و به پروژه اضافه کن» | `fab-search`، `fab-add` |
| «دارایی‌های انتخاب‌شده را با یک اسکریپت Python به صورت دسته‌ای تغییر نام بده» | `python-execute` |

‏**C++ یک چرخه درجه‌یک است.** `source-compile` اگر Live Coding فعال باشد با آن، وگرنه با UnrealBuildTool می‌سازد و هر پیام تشخیصی را با فایل، خط و شدتش برمی‌گرداند، پس عامل بدون خواندن لاگ ساخت خطاها را رفع می‌کند. اگر تغییری به ساخت کامل و راه‌اندازی دوباره ویرایشگر نیاز داشته باشد (کلاس‌های جدید، تغییر در اعلان‌های `UCLASS` / `UPROPERTY` / `UFUNCTION`) این را اعلام می‌کند. عملیات فایل درون `<Project>/Source/` می‌ماند. از ترمینال، `unreal-mcp-cli compile` بدون ویرایشگر پیام‌های `file:line:col` چاپ می‌کند.

‏**RQL از خود ویرایشگر آموخته می‌شود:** `rql-schema` هر منبع، نوع و عضو را فهرست می‌کند و مهارت `unreal-rql` نحو را با دستورالعمل‌ها آموزش می‌دهد (`unreal-mcp-cli setup-skills` آن را برای عامل شما می‌نویسد).

## پیش‌نیازها

- ‏**Unreal Engine 5.5 – 5.8** (خط انتشار هر کدام را اعتبارسنجی می‌کند).
- **یک پروژه C++**، چون پلاگین به صورت سورس ارائه می‌شود و Unreal ماژول‌های آن را با toolchain مربوط به C++ پلتفرم شما برای پروژه‌تان کامپایل می‌کند. یک پروژه فقط-Blueprint در یک قدم به پروژه C++ تبدیل می‌شود: در ویرایشگر، **Tools → New C++ Class…** و هر کلاسی بسازید. برای پروژه جدید، `unreal-mcp-cli create-project ./MyGame` یک پروژه C++ حداقلی می‌سازد.
- **بدون نصب .NET.** میزبان MCP یک برنامه .NET خودکفا (`unreal-mcp-bridge`) است که برای پلتفرم شما درون پلاگین قرار دارد؛ پلاگین آن را اجرا و دوباره راه‌اندازی می‌کند. (نسخه Fab فایل اجرایی ندارد؛ دکمه **Get bridge** آن bridge را یک بار نصب می‌کند.) فقط ساخت bridge از سورس به .NET 9 SDK نیاز دارد.
- ‏**Node.js 22.14+**، فقط برای `unreal-mcp-cli` (یک پکیج npm؛ 22.14 حداقل نسخه Node آن است).
- ‏**Windows یا macOS** (هر نسخه روی هر دو تست می‌شود). **Linux x64:** sidecar ساخته و منتشر می‌شود، اما هیچ تست انتشاری روی Linux اجرا نمی‌شود.

## شروع سریع

‏**۱. پلاگین را نصب کنید**، به یکی از این روش‌ها:

- ‏**[Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)** (بدون ترمینال): **AI Game Developer** را به موتور خود اضافه کنید، آن را در **Edit → Plugins** فعال کنید و پروژه‌تان را باز کنید.
- **دو دستور** (Node.js 22.14+):

</div>

```bash
npx unreal-mcp-cli install-plugin ./MyGame   # plugin + bundled bridge into MyGame/Plugins
npx unreal-mcp-cli open ./MyGame             # build if needed and open the editor
```

<div dir="rtl">

‏**۲. در ویرایشگر وصل شوید:** **Tools → AI Game Developer** را باز کنید.

- فقط برای نصب از Fab: یک بار روی **Get bridge** کلیک کنید (نسخه Fab فایل اجرایی ندارد، پس این کار sidecar به نام `unreal-mcp-bridge` را نصب می‌کند).
- ‏**Cloud** (پیش‌فرض): روی **Authorize** کلیک کنید و ورود را در مرورگر تأیید کنید. **محلی**: به **Custom** بروید و روی **Start** کنار **MCP server** کلیک کنید؛ ویرایشگر `gamedev-mcp-server` با checksum تأییدشده را دانلود می‌کند (در نصب Fab، این سرور را دستی نصب کنید: لاگ نشانی دانلود را می‌گوید).
- زیر **AI agent** عامل خود را انتخاب کنید (Claude Code، Cursor، Codex، …) و روی **Configure** کلیک کنید.

‏**۳. از عامل خود بخواهید:** *«سه مکعب در یک ردیف و یک چراغ نقطه‌ای بالای آن‌ها بساز، سپس یک اسکرین‌شات بگیر.»*

محلی یا Cloud؟ هر دو کاملاً پشتیبانی می‌شوند و بعداً می‌توانید عوض کنید. **Cloud پیش‌فرض است** چون به باز کردن هیچ پورتی نیاز ندارد و از تداخل پورت با برنامه‌های دیگر جلوگیری می‌کند؛ این اتصال رایگان است. **محلی** بدون حساب و بدون اینترنت کار می‌کند، برای ماشین‌های آفلاین و ایزوله (air-gapped).

| | **محلی** | **Cloud** |
| --- | --- | --- |
| حساب | لازم نیست | ورود به ai-game.dev |
| سرور MCP | ‏`gamedev-mcp-server` روی ماشین شما، که ویرایشگر یا عامل اجرا می‌کند | رله ai-game.dev |
| ترافیک | روی `localhost` می‌ماند | از ai-game.dev عبور می‌کند ([چه چیزی ثبت می‌کند](#what-the-cloud-relay-sees)) |
| احراز هویت | روی loopback هیچ، یا یک توکن مشترک (یک فلگ) | ‏OAuth یا کلید مخصوص هر پروژه |
| مناسب برای | کار آفلاین و ایزوله، CI، پروژه‌های خصوصی | بدون پورت باز و بدون تداخل پورت؛ یک حساب برای چند پروژه و ماشین |

</div>

<details>
<summary><b>همه چیز از ترمینال</b> (CLI: Cloud، محلی، احراز هویت با توکن)</summary>

<p align="center">
  <img src="img/readme/terminal.svg" alt="ترمینال: npm install -g unreal-mcp-cli، install-plugin، login، setup-mcp claude-code، باز کردن ویرایشگر" width="100%"/>
</p>

<div dir="rtl">

‏**Cloud** (حالت پیش‌فرض پلاگین):

</div>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

<div dir="rtl">

‏**محلی** (بدون حساب):

</div>

```bash
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin ./MyGame
unreal-mcp-cli configure -p ./MyGame --mode Custom                 # plugin: local server
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --transport stdio # agent starts it
unreal-mcp-cli open ./MyGame
```

<div dir="rtl">

‏`setup-mcp --transport stdio` نسخه `gamedev-mcp-server` با checksum تأییدشده را در پروژه دانلود می‌کند و عامل را به آن متصل می‌کند. هر دو طرف از همان پورت مخصوص پروژه روی `localhost` استفاده می‌کنند.

‏**فعال کردن احراز هویت با توکن (محلی):** فلگ‌ها را به خط `configure` اضافه کنید، سپس `setup-mcp` را دوباره اجرا کنید؛ از آن پس پلاگین و سرور هر دو secret را می‌خواهند.

</div>

```bash
unreal-mcp-cli configure -p ./MyGame --mode Custom --auth required --token <secret>
```

<div dir="rtl">

چرا در حالت محلی پیش‌فرض بدون احراز هویت است: سرور فقط روی loopback گوش می‌دهد و درخواست‌هایی را که از originهای غیرمحلی مرورگر می‌آیند رد می‌کند، پس فقط برنامه‌های روی ماشین خودتان به آن دسترسی دارند. وقتی به همه برنامه‌ها یا کاربران آن ماشین اعتماد ندارید، از توکن استفاده کنید. secret در فایل `.env` پروژه (که `configure` آن را به gitignore اضافه می‌کند) و در فایل پیکربندی عامل ذخیره می‌شود، پس آن فایل را از کنترل نسخه بیرون نگه دارید.

</div>

</details>

<div dir="rtl">

<a id="what-the-cloud-relay-sees"></a>**رله ابری چه می‌بیند.** در حالت Cloud، فراخوانی‌های ابزار از ai-game.dev عبور می‌کنند؛ ai-game.dev آن‌ها را فقط مسیریابی می‌کند و چیزی را که می‌فرستید ذخیره نمی‌کند: هیچ محتوای درخواست یا پاسخی، پس نه کد سورس، نه فایل‌های پروژه و نه دارایی‌ها. فقط فراداده (نام ابزار، زمان‌بندی، اندازه‌ها، نشانی IP، رویدادهای MCP) را به مدت ۹۰ روز نگه می‌دارد. [سیاست حریم خصوصی](https://ai-game.dev/legal/privacy).

‏`install-plugin` نسخه سورس امضاشده پلاگین را که با نسخه CLI مطابقت دارد دانلود می‌کند.

روش‌های دیگر نصب:

- ‏**zip با قالب Fab**: علاوه بر [صفحه Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)، هر release در GitHub یک build با قالب Fab (`unreal-mcp-plugin-fab-<version>.zip`) بدون فایل اجرایی دارد؛ با هر کدام، دکمه **Get bridge** در پنجره شما را یک بار در نصب sidecar راهنمایی می‌کند.
- **دستی**: [`UnrealMCP/`](../UnrealMCP/) را در `<YourProject>/Plugins/UnrealMCP/` کپی کنید. یک checkout از سورس، sidecar همراه ندارد: `unreal-mcp-cli bootstrap-local` را اجرا کنید یا `UNREAL_MCP_BRIDGE_PATH` را تنظیم کنید.

به‌روزرسانی (`unreal-mcp-cli update`)، حالت‌های اتصال، متغیرهای محیطی و عیب‌یابی: [docs/CONFIGURATION.md](CONFIGURATION.fa.md).

### راه‌اندازی عامل

‏`unreal-mcp-cli setup-mcp <agent> -p ./MyGame` پیکربندی را برایتان می‌نویسد (`setup-mcp --list` همه عامل‌های پشتیبانی‌شده را نشان می‌دهد). آنچه می‌نویسد:

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

‏`<pin>` به ویرایشگر همین پروژه مسیریابی می‌کند. وقتی ماشین وارد حساب شده باشد، هدر کلید همین پروژه است؛ `--oauth` پیکربندی فقط-URL می‌نویسد و عامل خودش وارد می‌شود. با `--transport stdio` این ورودی سرور محلی را اجرا می‌کند: `"command"` همان `gamedev-mcp-server` زیر `<project>/Intermediate/UnrealMCP/server/` است و `"args"` مقادیر `port=<port>`، `client-transport=stdio` و `authorization=none` را تنظیم می‌کند.

</div>

</details>

<details>
<summary><b>Cursor</b>: <code>.cursor/mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp cursor -p ./MyGame
```

<div dir="rtl">

همان ورودی `mcpServers` → `unreal-mcp` در Claude Code: `"type": "http"` با URL، یا `"type": "stdio"` با `command` و `args` برای سرور محلی.

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

با کلید پروژه، این بخش `http_headers` با هدر `Authorization` را هم دارد.

</div>

</details>

<div dir="rtl">

## استفاده

بیشتر وقت‌ها فقط با عامل خود صحبت می‌کنید. CLI هم همان ابزارها را مستقیم فراخوانی می‌کند و اگر ویرایشگری در حال اجرا نباشد، یک ویرایشگر بدون رابط گرافیکی (headless) اجرا می‌کند:

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

همه دستورها و گزینه‌ها: [cli/README.md](../cli/docs/README.fa.md) یا `unreal-mcp-cli commands`.

## امنیت

- ‏**پلاگین ↔ sidecar:** پلاگین روی یک پورت TCP در loopback گوش می‌دهد؛ sidecar با یک توکن یک‌بارمصرف که از طریق stdin فرستاده می‌شود به آن وصل می‌شود (هرگز روی خط فرمان و هرگز در لاگ).
- ‏**سرور محلی:** `gamedev-mcp-server` به طور پیش‌فرض روی loopback گوش می‌دهد و به درخواستی که `Origin` آن مجاز نیست پاسخ `403` می‌دهد (دفاع در برابر DNS rebinding). روی loopback، پلاگین آن را بدون احراز هویت اجرا می‌کند، مگر اینکه توکن انتخاب کنید (`UNREAL_MCP_AUTH_OPTION`، `UNREAL_MCP_TOKEN`). گزینه‌های سرور: [میزبانی و احراز هویت](https://github.com/IvanMurzak/GameDev-MCP-Server/blob/main/docs/hosting.md).
- ‏**Cloud:** عامل با OAuth وارد می‌شود یا از کلید مخصوص پروژه استفاده می‌کند که با `setup-mcp --regenerate-key` قابل تعویض است. رله هیچ محتوای درخواست یا پاسخی ذخیره نمی‌کند، فقط فراداده را به مدت ۹۰ روز ([جزئیات](#what-the-cloud-relay-sees)).
- **ابزارها چه می‌توانند بکنند:** ویرایش فایل‌های زیر `<Project>/Source/`، اجرای دستورهای کنسول و Python ویرایشگر، تغییر هر ویژگی reflect‌شده.
- **کنترل‌ها روی آن‌ها:**
  - **فهرست مجاز** (محدودیت سخت): `UNREAL_MCP_TOOLS` فقط ابزارهای فهرست‌شده را ارائه می‌دهد؛ هر ابزار دیگری نه فهرست می‌شود و نه قابل فراخوانی است.
  - **پنهان کردن** (نرم): چک‌باکس **Visible** در پنجره **MCP Tools** ابزار را از `tools/list` حذف می‌کند، اما کلاینتی که آن را با نام فراخوانی کند همچنان اجرایش می‌کند.
  - **تغییرات محدود:** ابزارهای فایل C++ درون `<Project>/Source/` می‌مانند؛ یک فراخوانی `rql-mutate` یا کامل انجام می‌شود یا اصلاً، یک گام undo است، با `dry_run` پیش‌نمایش می‌شود و بالای ۲۰۰ هدف رد می‌شود مگر اینکه فراخوانی `max_targets` را بالا ببرد (تا ۱۰۰۰)؛ `python-execute` پس از `timeout` (پیش‌فرض ۳۰ ثانیه) متوقف می‌شود؛ هر فراخوانی ابزار محدودیت زمانی دارد (`UNREAL_MCP_TOOL_TIMEOUT`، پیش‌فرض ۵ دقیقه).
  - **نه برای هوش مصنوعی:** ابزارهای سیستمی (`ping`، تولید مهارت، `editor-quit`) در `tools/list` نیستند و `editor-quit` رد می‌کند مگر اینکه nonce نشست CLI مطابقت داشته باشد.
  - جزئیات: [docs/TOOLS.md](TOOLS.fa.md).
- ‏**حالت Runtime به طور پیش‌فرض خاموش است:** بازی هرگز خودش وصل نمی‌شود. به تنظیم `bRuntimeMcpEnabled` (پیش‌فرض خاموش) و یک فراخوانی صریح `Connect()` نیاز دارد، فقط میزبان loopback را می‌پذیرد مگر اینکه میزبان راه دور را مجاز کنید، و در buildهای Shipping رد می‌شود مگر اینکه درون آن کامپایل شده باشد. با `TargetDenyList` پلاگین را از بازی‌های بسته‌بندی‌شده بیرون نگه دارید ([docs/RUNTIME.md](RUNTIME.fa.md)).

## افزونه‌ها

‏افزونه‌ها پلاگین‌های اختیاری‌ای هستند که RQL را گسترش می‌دهند؛ ابزار جداگانه‌ای اضافه نمی‌کنند.

</div>

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

<div dir="rtl">

| افزونه | چه اضافه می‌کند | پیش‌نیازها |
| --- | --- | --- |
| [AI&nbsp;ControlRig](../extensions/UnrealAIControlRig/README.md) | ریگ‌های انیمیشن رویه‌ای و اتصال آن‌ها به Anim Blueprint | `ControlRig`, `RigVM`, UE 5.7+‎ |
| [AI&nbsp;EnhancedInput](../extensions/UnrealAIEnhancedInput/README.md) | اکشن‌های ورودی، کانتکست‌های نگاشت، تریگرها و مادیفایرها | `EnhancedInput`, UE 5.7+‎ |
| [AI&nbsp;GAS](../extensions/UnrealAIGAS/README.md) | Gameplay Effectها، Blueprintهای توانایی و مجموعه‌های اتریبیوت | `GameplayAbilities`, `EnhancedInput`, `GameplayTagsEditor`, UE 5.7+‎ |
| [AI&nbsp;Landscape](../extensions/UnrealAILandscape/README.md) | لنداسکیپ‌ها، اقیانوس‌ها، دریاچه‌ها و رودخانه‌ها | `Water`, UE 5.7+‎ |
| [AI&nbsp;Niagara](../extensions/UnrealAINiagara/README.md) | سیستم‌ها، امیترها، ماژول‌ها و برخورد Niagara | `Niagara`, `ChaosNiagara`, UE 5.7+‎ |
| [AI&nbsp;PCG](../extensions/UnrealAIPCG/README.md) | گراف‌های PCG، حجم‌ها و پراکنش‌های تولیدشده | `PCG`, UE 5.7+‎ |

‏افزونه‌ها از سورس در پروژه شما کامپایل می‌شوند (دفعه بعد که ویرایشگر را باز می‌کنید، یا همان لحظه با `--build`)؛ نصب‌کننده پلاگین‌های موتوری را که لازم دارند فعال می‌کند.

## مستندات

| صفحه | محتوا |
|---|---|
| [docs/TOOLS.md](TOOLS.fa.md) | همه ابزارهای داخلی و سیستمی، افزونه‌ها، نمایش هر ابزار |
| [docs/rql.md](rql.md) | زبان کوئری RQL: منابع، اعضا، تغییرات، تست بازی |
| [docs/CONFIGURATION.md](CONFIGURATION.fa.md) | اجرای اول، به‌روزرسانی، متغیرهای محیطی، عیب‌یابی |
| [docs/EXTENSIONS.md](EXTENSIONS.md) | نوشتن ماژول‌های RQL، ابزارها، پرامپت‌ها و منابع خودتان |
| [docs/RUNTIME.md](RUNTIME.fa.md) | حالت Runtime (درون بازی)، دروازه‌های امنیتی آن، دور نگه داشتن پلاگین از بازی‌های منتشرشده |
| [cli/README.md](../cli/docs/README.fa.md) | مرجع دستورهای `unreal-mcp-cli` و کدهای خروج |
| [docs/ARCHITECTURE.md](ARCHITECTURE.md) | طراحی مرجع (IPC، ثبت ابزارها، sidecar، UI، پیکربندی) |
| [docs/RELEASING.md](RELEASING.md) | برای نگه‌دارندگان: انتشار، runner های CI، اسرار |
| [docs/EXTENSION-MONOREPO.md](EXTENSION-MONOREPO.md) | برای نگه‌دارندگان: تست و انتشار افزونه‌ها |

## ساختار مخزن

| مسیر | چیست |
|---|---|
| [`UnrealMCP/`](../UnrealMCP/) | پلاگین Unreal (C++): `UnrealMcpRuntime`، `UnrealMcpRql`، `UnrealMcpEditor` |
| [`bridge/`](../bridge/) | `unreal-mcp-bridge`، sidecar مبتنی بر .NET 9 |
| [`cli/`](../cli/) | `unreal-mcp-cli`، پکیج npm |
| [`extensions/`](../extensions/) | شش افزونه اختیاری موتور با انتشارهای جداگانه |
| [`samples/`](../samples/) | یک قالب افزونه ویرایشگر و یک نمونه ابزار Runtime (درون بازی) |

## پیوندها

- ‏[ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G) · [گزارش مشکل](https://github.com/IvanMurzak/Unreal-MCP/issues)
- همتایان: [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) · [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP)
- اجزای مشترک: [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) · [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) · [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet)
- ‏[Model Context Protocol](https://modelcontextprotocol.io/)

## مجوز

‏**برای توسعه‌دهندگان بازی رایگان است، بازی‌های تجاری هم.** Unreal MCP تحت [Apache-2.0](../LICENSE) © Ivan Murzak است. دو جزء همراه، RQL و Code-Exec، از مجوز [PolyForm Shield 1.0.0](https://polyformproject.org/licenses/shield/1.0.0) استفاده می‌کنند که فقط شرکت‌هایی را محدود می‌کند که ابزارهای هوش مصنوعی رقیب برای توسعه بازی می‌سازند؛ شما می‌توانید از همه آن برای ساخت، انتشار و فروش بازی‌هایتان استفاده کنید. اطلاعیه‌ها: [`UnrealMCP/ThirdPartyNotices.txt`](../UnrealMCP/ThirdPartyNotices.txt).

</div>
