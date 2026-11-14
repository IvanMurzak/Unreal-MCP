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

## ویژگی‌ها

‏✅ ‏**لایه داده RQL** — کوئری و تغییر اکتورها، دارایی‌ها و لول‌ها با یک زبان ([RQL](rql.md))\
‏✅ ‏**Blueprint** — ساخت، ویرایش و کامپایل، با خطاهای ساختاریافته‌ای که عامل می‌تواند رفع کند\
‏✅ ‏**C++** — ایجاد اسکلت، ویرایش و کامپایل سورس پروژه (Live Coding یا UBT)\
‏✅ ‏**بازخورد بصری** — اسکرین‌شات از ویوپورت، نمای بازی، دوربین و اکتور ایزوله\
‏✅ ‏**Python** — اجرای Python ویرایشگر با کل API `unreal`\
‏✅ ‏**Fab** — جستجو در Fab، فهرست کتابخانه شما، افزودن محتوایی که مالک آن هستید\
‏✅ ‏**افزونه‌ها** — GAS، Niagara، PCG، Landscape، Control Rig، Enhanced Input\
‏✅ ‏**ابزارهای خودتان** — افزودن ابزار، پرامپت و منبع از هر پلاگینی ([راهنما](EXTENSIONS.md))\
‏✅ ‏**حالت Runtime** — در صورت تمایل، یک بازی در حال اجرا را کنترل کنید ([docs/RUNTIME.md](RUNTIME.fa.md))\
‏✅ ‏**ابری یا محلی** — `ai-game.dev` بدون هیچ تنظیمی، یا یک `gamedev-mcp-server` روی میزبان خودتان

‏30 ابزار داخلی در 8 خانواده؛ فهرست کامل در [docs/TOOLS.md](TOOLS.fa.md) است.

## پیش‌نیازها

- ‏**Unreal Engine 5.5 – 5.8** (خط انتشار هر کدام را اعتبارسنجی می‌کند) و یک **پروژه C++**: پلاگین ماژول‌های ویرایشگر را در پروژه شما کامپایل می‌کند.
- ‏**Node.js 22.14+** برای `unreal-mcp-cli`.
- ‏Windows، macOS یا Linux. نیازی به نصب .NET نیست: sidecar درون پلاگین ارائه می‌شود. (.NET 9 SDK فقط برای ساخت bridge از سورس لازم است.)

## شروع سریع

</div>

<p align="center">
  <img src="img/readme/terminal.svg" alt="ترمینال: npm install -g unreal-mcp-cli، install-plugin، login، setup-mcp claude-code، باز کردن ویرایشگر" width="100%"/>
</p>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

<div dir="rtl">

‏`install-plugin` نسخه سورس امضاشده پلاگین را که با نسخه CLI مطابقت دارد دانلود می‌کند. در ویرایشگر، **Tools → AI Game Developer** را باز کنید تا اتصال، وضعیت bridge و عامل‌های متصل را ببینید. سپس از عامل خود بخواهید:

> *«سه مکعب در یک ردیف و یک چراغ نقطه‌ای بالای آن‌ها بساز، سپس یک اسکرین‌شات بگیر.»*

روش‌های دیگر نصب:

- ‏**Fab / Epic Marketplace**: [AI Game Developer در Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33). نسخه Fab نمی‌تواند فایل اجرایی همراه داشته باشد، بنابراین در آنجا دکمه **Get bridge** (دریافت bridge) در پنجره، شما را یک بار در نصب sidecar راهنمایی می‌کند.
- **دستی**: [`UnrealMCP/`](../UnrealMCP/) را در `<YourProject>/Plugins/UnrealMCP/` کپی کنید. یک checkout از سورس، sidecar همراه ندارد: `unreal-mcp-cli bootstrap-local` را اجرا کنید یا `UNREAL_MCP_BRIDGE_PATH` را تنظیم کنید.

به‌روزرسانی (`unreal-mcp-cli update`)، حالت‌های اتصال، متغیرهای محیطی و عیب‌یابی: [docs/CONFIGURATION.md](CONFIGURATION.fa.md).

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

‏[Apache-2.0](../LICENSE) © Ivan Murzak

</div>
