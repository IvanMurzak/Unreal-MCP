<div align="center">

<img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/readme/hero.svg" alt="Unreal MCP — به هر عامل هوش مصنوعی اجازه دهید ویرایشگر Unreal را بررسی و کنترل کند" width="100%"/>

<br/>

[![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Node.js](https://img.shields.io/badge/Node.js-22.14%2B-0E7490?logo=nodedotjs&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://nodejs.org/) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE) [![Docs](https://img.shields.io/badge/docs-ai--game.dev-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

  <p>
    <a href="https://claude.ai/download"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://visualstudio.microsoft.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/visual-studio-64.png" alt="Visual Studio" title="Visual Studio" height="36"></a>&nbsp;&nbsp;
    <a href="https://opencode.ai/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/open-code-64.png" alt="OpenCode" title="OpenCode" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/Kilo-Org/kilocode"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/mcp-clients/kilo-code-64.png" alt="Kilo Code" title="Kilo Code" height="36"></a>
  </p>

  <b>[English](../README.md) | [中文](README.zh-CN.md) | [日本語](README.ja.md) | [Español](README.es.md) | [فارسی](README.fa.md) | [हिन्दी](README.hi.md) | [العربية](README.ar.md)</b>

</div>

<div dir="rtl">

‏**`unreal-mcp-cli`** ابزار خط فرمان [Unreal-MCP](../../docs/README.fa.md) است، همتای Unreal برای [`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) و [`godot-cli`](https://www.npmjs.com/package/godot-cli). این ابزار پلاگین UnrealMCP را نصب می‌کند، شما را در [ai-game.dev](https://ai-game.dev) وارد می‌کند، عامل هوش مصنوعی شما را پیکربندی می‌کند، ویرایشگر Unreal را با تنظیمات اتصال درست باز می‌کند و ابزارهای پلاگین را از ترمینال فراخوانی می‌کند.

</div>

<p align="center">
  <img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/readme/terminal.svg" alt="ترمینال: npm install -g unreal-mcp-cli، install-plugin، login، setup-mcp claude-code، باز کردن ویرایشگر" width="100%"/>
</p>

<div dir="rtl">

## ویژگی‌ها

‏✅ ‏**نصب پلاگین** — کپی (یا junction) پلاگین UnrealMCP در پوشه `Plugins/` پروژه\
‏✅ ‏**افزونه‌ها** — افزودن افزونه‌های اختیاری موتور (GAS، Niagara، PCG، …)\
‏✅ ‏**ورود** — ورود با OAuth device-code به `ai-game.dev`، یک بار برای هر دستگاه\
‏✅ ‏**راه‌اندازی عامل‌ها** — پیکربندی MCP برای Claude Code، Cursor، Copilot، Codex و دیگران\
‏✅ ‏**باز و بسته کردن** — باز کردن ویرایشگر، پنجره‌ای یا headless؛ فقط ویرایشگرهای اجراشده با CLI بسته می‌شوند\
‏✅ ‏**فراخوانی ابزارها** — `rql`، `exec`، `run-tool`؛ در صورت نیاز یک ویرایشگر headless اجرا می‌شود\
‏✅ ‏**ساخت و تست** — کامپایل C++ با UBT و اجرای تست‌های Automation بدون ویرایشگر\
‏✅ ‏**مناسب عامل‌ها** — `--format json`، کدهای خروج پایدار، یک مانیفست `commands`\
‏✅ ‏**چندسکویی** — Windows، macOS و Linux

## شروع سریع

به [Node.js](https://nodejs.org/) نسخه 22.14 یا جدیدتر نیاز دارد.

</div>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # plugin + bundled bridge into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

<div dir="rtl">

‏`install-plugin` و `update` در صورت وجود از یک checkout محلی `UnrealMCP/` استفاده می‌کنند؛ در غیر این صورت asset سورس پلاگین را از GitHub Release مطابق با نسخه CLI دانلود می‌کنند و پیش از استخراج، امضای آن را (minisign / Ed25519) بررسی می‌کنند. `--plugin-source <dir>` جایگزین برای حالت آفلاین / CI / توسعه است.

## استفاده

</div>

```bash
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"
unreal-mcp-cli compile ./MyGame --strict
unreal-mcp-cli test ./MyGame --junit-output junit.xml
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

<div dir="rtl">

‏`rql`، `exec` و `run-tool` از ویرایشگر در حال اجرا استفاده می‌کنند یا یک ویرایشگر headless اجرا می‌کنند؛ `compile` و `test` بدون ویرایشگر اجرا می‌شوند.

## دستورها

| بخش | دستورها |
| --- | --- |
| راه‌اندازی | `create-project` · `install-engine` · `install-plugin` · `bootstrap-local` |
| پکیج‌ها | `install-extension` · `update` · `remove-plugin` |
| حساب کاربری | `login` |
| عامل‌های هوش مصنوعی | `setup-mcp` · `setup-skills` · `configure` |
| ویرایشگر | `open` · `close` · `wait-for-ready` · `status` · `dialog` |
| ابزارها | `tools` · `rql` · `run-tool` · `run-system-tool` · `exec` |
| ساخت، تست | `compile` · `test` |
| کشف | `commands` |

همه گزینه‌ها، کدهای خروج و خروجی JSON: **[مرجع دستورها](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/commands.md)**، یا `unreal-mcp-cli <command> --help`.

## کتابخانه

دستورها یک کتابخانه تایپ‌شده و بدون اثر جانبی هم هستند: بدون پردازش argv، بدون stdout، بدون خروج از پروسه؛ هر نتیجه `kind: 'success' | 'failure'` است.

</div>

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

<div dir="rtl">

## پیوندها

‏[Unreal-MCP](../../docs/README.fa.md) · [ابزارها](../../docs/TOOLS.fa.md) · [پیکربندی](../../docs/CONFIGURATION.fa.md) · [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G)

## مجوز

‏[Apache-2.0](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)

</div>
