<div align="center">

<img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/readme/hero.svg" alt="Unreal MCP — دع أي وكيل ذكاء اصطناعي يفحص محرر Unreal ويتحكم فيه" width="100%"/>

<br/>

[![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Node.js](https://img.shields.io/badge/Node.js-22.14%2B-0E7490?logo=nodedotjs&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://nodejs.org/) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE) [![Docs](https://img.shields.io/badge/docs-ai--game.dev-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

  <p>
    <a href="https://claude.ai/download"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/claude-64.png" alt="Claude" title="Claude" height="36"></a>&nbsp;&nbsp;
    <a href="https://openai.com/index/introducing-codex/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/codex-64.png" alt="Codex" title="Codex" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.cursor.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/cursor-64.png" alt="Cursor" title="Cursor" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/docs/copilot/overview"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/github-copilot-64.png" alt="GitHub Copilot" title="GitHub Copilot" height="36"></a>&nbsp;&nbsp;
    <a href="https://gemini.google.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/gemini-64.png" alt="Gemini" title="Gemini" height="36"></a>&nbsp;&nbsp;
    <a href="https://antigravity.google/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/antigravity-64.png" alt="Antigravity" title="Antigravity" height="36"></a>&nbsp;&nbsp;
    <a href="https://code.visualstudio.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/vs-code-64.png" alt="VS Code" title="VS Code" height="36"></a>&nbsp;&nbsp;
    <a href="https://www.jetbrains.com/rider/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/rider-64.png" alt="Rider" title="Rider" height="36"></a>&nbsp;&nbsp;
    <a href="https://visualstudio.microsoft.com/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/visual-studio-64.png" alt="Visual Studio" title="Visual Studio" height="36"></a>&nbsp;&nbsp;
    <a href="https://opencode.ai/"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/open-code-64.png" alt="OpenCode" title="OpenCode" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/cline/cline"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/cline-64.png" alt="Cline" title="Cline" height="36"></a>&nbsp;&nbsp;
    <a href="https://github.com/Kilo-Org/kilocode"><img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/mcp-clients/kilo-code-64.png" alt="Kilo Code" title="Kilo Code" height="36"></a>
  </p>

  <b>[English](../README.md) | [中文](README.zh-CN.md) | [日本語](README.ja.md) | [Español](README.es.md) | [فارسی](README.fa.md) | [हिन्दी](README.hi.md) | [العربية](README.ar.md)</b>

</div>

<div dir="rtl">

‏**`unreal-mcp-cli`** هي أداة سطر الأوامر الخاصة بـ [Unreal-MCP](../../docs/README.ar.md)، ونظيرة Unreal لكل من [`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) و[`godot-cli`](https://www.npmjs.com/package/godot-cli). تثبّت إضافة UnrealMCP، وتسجّل دخولك إلى [ai-game.dev](https://ai-game.dev)، وتهيّئ وكيل الذكاء الاصطناعي الخاص بك، وتفتح محرر Unreal بإعدادات الاتصال الصحيحة، وتستدعي أدوات الإضافة من الطرفية.

</div>

<p align="center">
  <img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/readme/terminal.svg" alt="الطرفية: npm install -g unreal-mcp-cli، ثم install-plugin، ثم login، ثم setup-mcp claude-code، ثم فتح المحرر" width="100%"/>
</p>

<div dir="rtl">

## الميزات

‏✅ ‏**تثبيت الإضافة** — نسخ إضافة UnrealMCP (أو ربطها بـ junction) إلى مجلد `Plugins/` في المشروع\
‏✅ ‏**الامتدادات** — إضافة امتدادات اختيارية للمحرك (GAS، Niagara، PCG، …)\
‏✅ ‏**تسجيل الدخول** — دخول عبر OAuth device-code إلى `ai-game.dev`، مرة واحدة لكل جهاز\
‏✅ ‏**إعداد الوكلاء** — إعدادات MCP لـ Claude Code وCursor وCopilot وCodex وغيرها\
‏✅ ‏**الفتح والإغلاق** — فتح المحرر بنافذة أو دون واجهة؛ ولا يُغلق إلا ما شغّلته CLI\
‏✅ ‏**استدعاء الأدوات** — `rql` و`exec` و`run-tool`؛ يبدأ محرر headless عند الحاجة\
‏✅ ‏**البناء والاختبار** — تجميع C++ عبر UBT وتشغيل اختبارات Automation دون محرر\
‏✅ ‏**ملائمة للوكلاء** — `--format json`، ورموز خروج ثابتة، وبيان `commands`\
‏✅ ‏**متعددة المنصات** — Windows وmacOS وLinux

## البدء السريع

تتطلب [Node.js](https://nodejs.org/) الإصدار 22.14 أو أحدث.

</div>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # plugin + bundled bridge into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

<div dir="rtl">

‏`install-plugin` و`update` يستخدمان نسخة `UnrealMCP/` محلية إن وُجدت؛ وإلا ينزّلان ملف مصدر الإضافة من إصدار GitHub Release المطابق لإصدار CLI ويتحققان من توقيعه (minisign / Ed25519) قبل فك ضغطه. الخيار `--plugin-source <dir>` هو البديل للعمل دون اتصال / في CI / أثناء التطوير.

## الاستخدام

</div>

```bash
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"
unreal-mcp-cli compile ./MyGame --strict
unreal-mcp-cli test ./MyGame --junit-output junit.xml
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

<div dir="rtl">

‏`rql` و`exec` و`run-tool` تعيد استخدام محرر قيد التشغيل أو تشغّل محررًا دون واجهة؛ أما `compile` و`test` فتعملان دون محرر.

## الأوامر

| المجال | الأوامر |
| --- | --- |
| الإعداد | `create-project` · `install-engine` · `install-plugin` · `bootstrap-local` |
| الحزم | `install-extension` · `update` · `remove-plugin` |
| الحساب | `login` |
| وكلاء الذكاء الاصطناعي | `setup-mcp` · `setup-skills` · `configure` |
| المحرر | `open` · `close` · `wait-for-ready` · `status` · `dialog` |
| الأدوات | `tools` · `rql` · `run-tool` · `run-system-tool` · `exec` |
| البناء والاختبار | `compile` · `test` |
| الاستكشاف | `commands` |

كل الخيارات ورموز الخروج ومخرجات JSON: **[مرجع الأوامر](https://github.com/IvanMurzak/Unreal-MCP/blob/docs/readme-refresh/cli/docs/commands.md)**، أو `unreal-mcp-cli <command> --help`.

## المكتبة

الأوامر متاحة أيضًا كمكتبة ذات أنواع محددة وبلا آثار جانبية: لا تحليل لـ argv، ولا stdout، ولا إنهاء للعملية؛ وكل نتيجة هي `kind: 'success' | 'failure'`.

</div>

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

<div dir="rtl">

## روابط

‏[Unreal-MCP](../../docs/README.ar.md) · [الأدوات](../../docs/TOOLS.ar.md) · [الإعدادات](../../docs/CONFIGURATION.ar.md) · [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G)

## الترخيص

‏[Apache-2.0](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)

</div>
