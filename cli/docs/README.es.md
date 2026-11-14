<div align="center">

<img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/readme/hero.svg" alt="Unreal MCP — deja que cualquier agente de IA inspeccione y controle el Editor de Unreal" width="100%"/>

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

**`unreal-mcp-cli`** es la herramienta de línea de comandos de [Unreal-MCP](../../docs/README.es.md), el hermano para Unreal de [`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) y [`godot-cli`](https://www.npmjs.com/package/godot-cli). Instala el plugin UnrealMCP, inicia tu sesión en [ai-game.dev](https://ai-game.dev), configura tu agente de IA, abre el Editor de Unreal con los ajustes de conexión correctos y llama a las herramientas del plugin desde la terminal.

<p align="center">
  <img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/main/docs/img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, abrir el editor" width="100%"/>
</p>

## Características

✅ **Instalar el plugin** — copia (o enlaza con junction) el plugin UnrealMCP en `Plugins/` del proyecto\
✅ **Extensiones** — añade extensiones de motor opcionales (GAS, Niagara, PCG, …)\
✅ **Iniciar sesión** — login OAuth con device code en `ai-game.dev`, una vez por máquina\
✅ **Configurar agentes** — configs MCP para Claude Code, Cursor, Copilot, Codex y más\
✅ **Abrir y cerrar** — abre el editor, con ventana o headless; cierra solo los que inició la CLI\
✅ **Llamar herramientas** — `rql`, `exec`, `run-tool`; un editor headless se inicia bajo demanda\
✅ **Compilar y probar** — compila C++ con UBT y ejecuta tests de Automation sin editor\
✅ **Pensada para agentes** — `--format json`, códigos de salida estables, un manifiesto `commands`\
✅ **Multiplataforma** — Windows, macOS y Linux

## Inicio rápido

Requiere [Node.js](https://nodejs.org/) 22.14 o posterior.

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # plugin + bundled bridge into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` y `update` usan una copia local de `UnrealMCP/` si existe; si no, descargan el asset con el código fuente del plugin de la GitHub Release que coincide con la versión de la CLI y verifican su firma (minisign / Ed25519) antes de extraerlo. `--plugin-source <dir>` es la alternativa para uso offline, CI o desarrollo.

## Uso

```bash
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"
unreal-mcp-cli compile ./MyGame --strict
unreal-mcp-cli test ./MyGame --junit-output junit.xml
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

`rql`, `exec` y `run-tool` reutilizan un editor en ejecución o inician uno headless; `compile` y `test` se ejecutan sin editor.

## Comandos

| Área | Comandos |
| --- | --- |
| Configuración inicial | `create-project` · `install-engine` · `install-plugin` · `bootstrap-local` |
| Paquetes | `install-extension` · `update` · `remove-plugin` |
| Cuenta | `login` |
| Agentes de IA | `setup-mcp` · `setup-skills` · `configure` |
| Editor | `open` · `close` · `wait-for-ready` · `status` · `dialog` |
| Herramientas | `tools` · `rql` · `run-tool` · `run-system-tool` · `exec` |
| Compilación, tests | `compile` · `test` |
| Descubrimiento | `commands` |

Todas las opciones, códigos de salida y salida JSON: **[referencia de comandos](https://github.com/IvanMurzak/Unreal-MCP/blob/main/cli/docs/commands.md)**, o `unreal-mcp-cli <command> --help`.

## Biblioteca

Los comandos también forman una biblioteca tipada y sin efectos secundarios: sin parseo de argv, sin stdout, sin salida del proceso; cada resultado es `kind: 'success' | 'failure'`.

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

## Enlaces

[Unreal-MCP](../../docs/README.es.md) · [Herramientas](../../docs/TOOLS.es.md) · [Configuración](../../docs/CONFIGURATION.es.md) · [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G)

## Licencia

[Apache-2.0](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)
