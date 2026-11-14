<div align="center">

<img src="img/readme/hero.svg" alt="Unreal MCP — deja que Claude, Cursor, Copilot o cualquier agente MCP inspeccione y controle el Editor de Unreal" width="100%"/>

<br/>

[![Release](https://img.shields.io/github/v/release/IvanMurzak/Unreal-MCP?label=release&color=2F6F9F&logo=github&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/releases) [![CLI](https://img.shields.io/npm/v/unreal-mcp-cli?label=CLI&color=BE185D&logo=npm&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.npmjs.com/package/unreal-mcp-cli) [![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.5%E2%80%935.8-0E7490?logo=unrealengine&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://www.unrealengine.com/) [![Release build](https://img.shields.io/github/actions/workflow/status/IvanMurzak/Unreal-MCP/release.yml?label=release%20build&logo=github&color=15803D&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://github.com/IvanMurzak/Unreal-MCP/actions/workflows/release.yml)<br/>[![Docs](https://img.shields.io/badge/docs-ai--game.dev-2F6F9F?style=for-the-badge&labelColor=0F172A&logoColor=white)](https://ai-game.dev) [![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&style=for-the-badge&labelColor=0F172A&logoColor=white)](https://discord.gg/cfbdMZX99G) [![License](https://img.shields.io/github/license/IvanMurzak/Unreal-MCP?label=license&color=047857&style=for-the-badge&labelColor=0F172A&logoColor=white)](../LICENSE) [![Stand with Ukraine](https://raw.githubusercontent.com/IvanMurzak/IvanMurzak/main/images/ukraine/flag-badge.svg)](https://stand-with-ukraine.pp.ua)

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

**Unreal MCP** (AI Game Developer para Unreal) conecta cualquier agente de IA compatible con MCP al **Editor de Unreal**. Pide lo que quieras con palabras sencillas y el agente crea actores, edita niveles, crea y compila Blueprints, edita y compila C++, gestiona assets y comprueba su trabajo con capturas de pantalla. Es el hermano de [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) y [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) para Unreal, y usa el mismo backend en la nube ([ai-game.dev](https://ai-game.dev)) o tu propio [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server).

<p align="center">
  <img src="img/readme/architecture.svg" alt="Un agente de IA habla MCP con el servidor; el servidor retransmite por SignalR al sidecar unreal-mcp-bridge, que llega al plugin C++ UnrealMCP en el Editor de Unreal por TCP loopback" width="100%"/>
</p>

El editor de Unreal está escrito en C++, así que el host MCP de .NET se ejecuta a su lado como un pequeño **sidecar** (`unreal-mcp-bridge`) que el plugin inicia, vigila y reinicia por ti. Detalles del diseño: [docs/ARCHITECTURE.md](ARCHITECTURE.md).

## Características

✅ **Capa de datos RQL** — consulta y cambia actores, assets y niveles con un solo lenguaje ([RQL](rql.md))\
✅ **Blueprints** — crea, edita y compila, con errores estructurados que el agente puede corregir\
✅ **C++** — genera, edita y compila el código del proyecto (Live Coding o UBT)\
✅ **Feedback visual** — capturas del viewport, de la vista de juego, de cámara y de actores aislados\
✅ **Python** — ejecuta Python del editor con toda la API `unreal`\
✅ **Fab** — busca en Fab, lista tu biblioteca, añade contenido que posees\
✅ **Extensiones** — GAS, Niagara, PCG, Landscape, Control Rig, Enhanced Input\
✅ **Tus propias herramientas** — añade herramientas, prompts y recursos desde cualquier plugin ([guía](EXTENSIONS.md))\
✅ **Modo runtime** — actívalo para controlar un juego en ejecución ([docs/RUNTIME.md](RUNTIME.es.md))\
✅ **Nube o local** — `ai-game.dev` listo para usar, o un `gamedev-mcp-server` autoalojado

30 herramientas integradas en 8 familias; la lista completa está en [docs/TOOLS.md](TOOLS.es.md).

## Requisitos

- **Unreal Engine 5.5 – 5.8** (el pipeline de release valida cada una) y un **proyecto C++**: el plugin compila módulos de editor en tu proyecto.
- **Node.js 22.14+** para `unreal-mcp-cli`.
- Windows, macOS o Linux. No hace falta instalar .NET: el sidecar viene dentro del plugin. (El SDK de .NET 9 solo hace falta para compilar el bridge desde el código fuente.)

## Inicio rápido

<p align="center">
  <img src="img/readme/terminal.svg" alt="Terminal: npm install -g unreal-mcp-cli, install-plugin, login, setup-mcp claude-code, abrir el editor" width="100%"/>
</p>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` descarga la release firmada del código fuente del plugin que coincide con la versión de la CLI. En el editor, abre **Tools → AI Game Developer** para ver la conexión, el estado del bridge y los agentes conectados. Después pide a tu agente:

> *"Crea tres cubos en fila y una luz puntual encima de ellos, y luego toma una captura de pantalla."*

Otras formas de instalar:

- **Fab / Epic Marketplace**: próximamente. La build de Fab no puede incluir ejecutables, así que allí el botón **Get bridge** de la ventana te guía para instalar el sidecar una sola vez.
- **Manual**: copia [`UnrealMCP/`](../UnrealMCP/) en `<YourProject>/Plugins/UnrealMCP/`. Una copia del código fuente no incluye el sidecar: ejecuta `unreal-mcp-cli bootstrap-local` o define `UNREAL_MCP_BRIDGE_PATH`.

Actualización (`unreal-mcp-cli update`), modos de conexión, variables de entorno y solución de problemas: [docs/CONFIGURATION.md](CONFIGURATION.es.md).

## Uso

Casi siempre basta con hablar con tu agente. La CLI también llama directamente a las mismas herramientas e inicia un editor headless cuando no hay ninguno en ejecución:

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

Todos los comandos y opciones: [cli/README.md](../cli/docs/README.es.md) o `unreal-mcp-cli commands`.

## Extensiones

Las extensiones son plugins opcionales que amplían RQL; no añaden herramientas propias.

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

| Extensión | Añade | Requiere |
| --- | --- | --- |
| [AI&nbsp;ControlRig](../extensions/UnrealAIControlRig/README.md) | Rigs de animación procedural y su conexión a Anim Blueprints | `ControlRig`, `RigVM`, UE 5.7+ |
| [AI&nbsp;EnhancedInput](../extensions/UnrealAIEnhancedInput/README.md) | Acciones de entrada, contextos de mapeo, triggers y modificadores | `EnhancedInput`, UE 5.7+ |
| [AI&nbsp;GAS](../extensions/UnrealAIGAS/README.md) | Gameplay Effects, Blueprints de habilidades y conjuntos de atributos | `GameplayAbilities`, `EnhancedInput`, `GameplayTagsEditor`, UE 5.7+ |
| [AI&nbsp;Landscape](../extensions/UnrealAILandscape/README.md) | Paisajes, océanos, lagos y ríos | `Water`, UE 5.7+ |
| [AI&nbsp;Niagara](../extensions/UnrealAINiagara/README.md) | Sistemas, emisores, módulos y colisión de Niagara | `Niagara`, `ChaosNiagara`, UE 5.7+ |
| [AI&nbsp;PCG](../extensions/UnrealAIPCG/README.md) | Grafos PCG, volúmenes y dispersiones generadas | `PCG`, UE 5.7+ |

Las extensiones se compilan desde el código fuente en tu proyecto (la próxima vez que abras el editor, o al momento con `--build`); el instalador activa los plugins del motor que necesitan.

## Documentación

| Página | Qué contiene |
|---|---|
| [docs/TOOLS.md](TOOLS.es.md) | Todas las herramientas integradas y de sistema, extensiones, visibilidad por herramienta |
| [docs/rql.md](rql.md) | El lenguaje de consultas RQL: fuentes, miembros, mutaciones, pruebas de juego |
| [docs/CONFIGURATION.md](CONFIGURATION.es.md) | Primer arranque, actualización, variables de entorno, solución de problemas |
| [docs/EXTENSIONS.md](EXTENSIONS.md) | Escribe tus propios módulos RQL, herramientas, prompts y recursos |
| [docs/RUNTIME.md](RUNTIME.es.md) | Modo runtime (dentro del juego), sus controles de seguridad, cómo dejar el plugin fuera de los juegos publicados |
| [cli/README.md](../cli/docs/README.es.md) | Referencia de comandos de `unreal-mcp-cli` y códigos de salida |
| [docs/ARCHITECTURE.md](ARCHITECTURE.md) | El diseño de referencia (IPC, registro, sidecar, UI, configuración) |
| [docs/RELEASING.md](RELEASING.md) | Mantenedores: releases, runners de CI, secretos |
| [docs/EXTENSION-MONOREPO.md](EXTENSION-MONOREPO.md) | Mantenedores: pruebas y releases de las extensiones |

## Estructura del repositorio

| Ruta | Qué es |
|---|---|
| [`UnrealMCP/`](../UnrealMCP/) | El plugin de Unreal (C++): `UnrealMcpRuntime`, `UnrealMcpRql`, `UnrealMcpEditor` |
| [`bridge/`](../bridge/) | `unreal-mcp-bridge`, el sidecar de .NET 9 |
| [`cli/`](../cli/) | `unreal-mcp-cli`, el paquete npm |
| [`extensions/`](../extensions/) | Seis extensiones de motor opcionales con sus propias releases |
| [`samples/`](../samples/) | Una plantilla de extensión de editor y un ejemplo de herramienta en runtime (dentro del juego) |

## Enlaces

- [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G) · [Issues](https://github.com/IvanMurzak/Unreal-MCP/issues)
- Proyectos hermanos: [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) · [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP)
- Piezas compartidas: [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) · [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) · [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet)
- [Model Context Protocol](https://modelcontextprotocol.io/)

## Licencia

[Apache-2.0](../LICENSE) © Ivan Murzak
