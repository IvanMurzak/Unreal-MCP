<b>[English](TOOLS.md) | [中文](TOOLS.zh-CN.md) | [日本語](TOOLS.ja.md) | [Español](TOOLS.es.md) | [فارسی](TOOLS.fa.md) | [हिन्दी](TOOLS.hi.md) | [العربية](TOOLS.ar.md)</b>

# Herramientas de Unreal-MCP

El inventario canónico de las herramientas que registra el plugin de editor **UnrealMCP**. Unreal-MCP incluye **herramientas integradas, agrupadas en familias,** que tu agente de IA puede llamar, más **herramientas de sistema** que no puede llamar (infraestructura del host para la CLI y la app de escritorio). Los ids de herramienta usan kebab-case (`rql-query`, `blueprint-compile`), la misma convención que Unity-MCP y Godot-MCP. Las [extensiones](#extensiones) y [tus propios plugins](EXTENSIONS.md) añaden más.

| Familia | Herramientas |
| --- | --- |
| Capa de datos RQL | `rql-query`, `rql-mutate`, `rql-schema` |
| Blueprint | `blueprint-*`: create, get, add-component, remove-component, add-variable, modify-variable, set-default, add-function, add-event, compile, spawn |
| Código fuente / C++ | `source-*`: read, create-class, update, delete, list, compile |
| Captura de pantalla | `screenshot-*`: viewport, game-view, camera, isolated |
| Fab | `fab-search`, `fab-library`, `fab-add` |
| Consola | `console-run-command` |
| Python | `python-execute` |
| Lista de herramientas | `unreal-tool-list` |
| **Sistema** (no se muestran a la IA) | `ping`, `unreal-skill-create`, `unreal-skill-generate`, `editor-quit` |

> **Fuente de verdad.** Herramientas estándar: `UnrealMcp*Tools.cpp` en `UnrealMCP/Source/UnrealMcpEditor/Private/Tools/` (excepto `UnrealMcpSkillTools.cpp`) y `UnrealMcpRqlTools.cpp` en `UnrealMcpRql`. Herramientas de sistema: `UnrealMcpPingTool.cpp`, `UnrealMcpSkillTools.cpp`, `UnrealMcpEditorSession.cpp` y `SkillGenerateTool.cs` del sidecar. Actualiza esta página cuando cambie un registro.

Todas las herramientas estándar son **solo de editor**. Una conexión en runtime (dentro del juego) solo ofrece `ping` más las herramientas que registre tu juego: consulta [RUNTIME.md](RUNTIME.es.md).

## Familias en detalle

<details>
  <summary><b>RQL — la capa de datos</b></summary>

Un solo lenguaje de consultas para todo el editor en lugar de una herramienta por operación ([`docs/rql.md`](rql.md)): busca actores, componentes y assets, lee miembros, filtra, agrega y cámbialos en una sola llamada. Sustituye a las antiguas herramientas de actores, objetos, assets, niveles, estado del editor, selección, log de consola y reflexión.

| Id de herramienta | Qué hace |
| --- | --- |
| `rql-query` | Consulta RQL de solo lectura sobre el editor en vivo (`actors | where $ is PointLight | select $ref Label Intensity`); resultados en tabla compacta; varias consultas separadas por `;` en una sola llamada |
| `rql-mutate` | Consulta RQL que termina en `set` / `call` (`... | set Location = "(0, 0, 100)"`, `editor | call SpawnActor("PointLight")`): todo o nada, un solo paso de deshacer, `dry_run` muestra una vista previa; varias separadas por `;` son un cambio cada una |
| `rql-schema` | Fuentes, funciones y las clases del mundo del editor; `type` lista los miembros y métodos de un tipo |

Además de las propiedades reflejadas, RQL llega a filas de DataTable (`dataTables`, `Rows["Sword"].Damage`), gameplay tags como texto (`gameplayTags`, `set Tags = ["Ability.Fire"]`), Project Settings guardados en su ini (`unreal.settings("Maps & Modes")`), grafos de Blueprint (`EventGraph | call AddCallFunction("PrintString")`, conexiones de pines, `call Compile()`), árboles de Widget Blueprint (`call AddWidget("Button", "Ok", parent)`) y Level Sequences (bindings, pistas, `call AddKey("Location.X", 30, 100)`).

La skill `unreal-rql` (escrita por `unreal-skill-generate`) enseña el lenguaje y lista lo que añaden las extensiones instaladas.

</details>

<details>
  <summary><b>Familia Blueprint — la superficie estrella de Unreal</b></summary>

| Id de herramienta | Qué hace |
| --- | --- |
| `blueprint-create` | Crea una nueva clase Blueprint a partir de la ruta de un `UClass` padre |
| `blueprint-get` | Resumen del grafo para que lo inspeccione el LLM (variables, componentes, funciones/eventos, cadena de padres) |
| `blueprint-add-component` | Añade un componente mediante el Simple Construction Script |
| `blueprint-remove-component` | Elimina un componente del SCS |
| `blueprint-add-variable` | Añade una variable miembro con tipo |
| `blueprint-modify-variable` | Modifica una variable miembro |
| `blueprint-set-default` | Edita una propiedad del CDO (valor por defecto de la clase) |
| `blueprint-add-function` | Añade el esqueleto de una función (nodos de entrada/resultado conectados) |
| `blueprint-add-event` | Añade/vincula el esqueleto de un evento (BeginPlay, Tick, input, …) |
| `blueprint-compile` | Compila el Blueprint y devuelve una **lista estructurada de errores/avisos** (el ciclo de feedback de la IA) |
| `blueprint-spawn` | Instancia el Blueprint en el nivel actual |

</details>

<details>
  <summary><b>Familia Consola</b></summary>

| Id de herramienta | Qué hace |
| --- | --- |
| `console-run-command` | Ejecuta un comando de consola / CVar |

Los logs, el estado de PIE y la selección son RQL: `console | where Verbosity == "Error"`, `editor | set IsPlaying = true`, `editor | set Selection = [...]`. Las pruebas de juego simulan el juego dentro de la llamada (de forma determinista, sin depender de que el editor avance entre llamadas): `editor | call StartPlay()`, `editor | call AdvancePlay(1.5, 0, "W, LeftShift")` (mantiene las teclas pulsadas 1,5 s y devuelve la posición del pawn, su velocidad, cuánto lo movió la llamada, el modo de movimiento y los mapping contexts aplicados), `editor | call StopPlay()`; la extensión Enhanced Input añade `enhancedInput | call InjectInput(action, value, seconds)` ([docs/rql.md](rql.md#play-testing)).

</details>

<details>
  <summary><b>Familia Lista de herramientas</b></summary>

| Id de herramienta | Qué hace |
| --- | --- |
| `unreal-tool-list` | Lista todas las herramientas, incluidas las ocultas en `tools/list` (marcadas como ocultas; siguen funcionando si se llaman por nombre); `filter` con regex, `includeDescription`, `includeInputs` |

</details>

<details>
  <summary><b>Familia Python</b></summary>

| Id de herramienta | Qué hace |
| --- | --- |
| `python-execute` | Ejecuta Python en el editor (toda la API `unreal`, ya importada): sentencias simples, la última expresión es el resultado, `print` vuelve como logs, los errores señalan líneas de `snippet.py`; `include` añade archivos/carpetas auxiliares de `<Project>/.ai-game-dev/snippets`; `timeout` (segundos, por defecto 30) detiene un snippet descontrolado |

Usa el PythonScriptPlugin del motor (lo activa Unreal-MCP; solo de editor, nunca en un juego empaquetado) y el runtime [Code-Exec](https://github.com/IvanMurzak/Code-Exec) incluido en `UnrealMCP/Content/Python`. El cableado de grafos de Blueprint no se expone a Python: usa las herramientas de Blueprint para eso.

</details>

<details>
  <summary><b>Familia Fab — busca y añade contenido de Fab</b></summary>

| Id de herramienta | Qué hace |
| --- | --- |
| `fab-search` | Busca en el marketplace de [Fab](https://www.fab.com) (`query`, `listingType`, `freeOnly`, `sort`, paginación); devuelve el uid, título, tipo, precio y formatos de cada listing |
| `fab-library` | Lista los assets de Fab que posee el usuario con sesión iniciada, con dónde está importado ya cada uno; `filter` busca en toda la biblioteca |
| `fab-add` | Descarga un listing que posee el usuario y lo importa como el *Add to Project* del panel de Fab (packs de Unreal Engine, Megascans con `quality`, glTF/FBX); devuelve la carpeta de contenido y omite los listings que ya están en el proyecto |

Controlan el propio plugin Fab del motor (activado por defecto desde UE 5.5): su inicio de sesión de Epic y sus flujos de importación. El primer `fab-library` / `fab-add` sin sesión iniciada abre la página de inicio de sesión de Epic en el navegador (abrir Fab desde la barra de herramientas del editor también inicia sesión, a partir de un login de Epic recordado). Comprar, o añadir un listing gratuito a la biblioteca, se hace en fab.com. Los proyectos completos y los plugins de código del motor se instalan desde el Epic Games Launcher, no se añaden a un proyecto.

</details>

<details>
  <summary><b>Familia Código fuente / C++</b></summary>

| Id de herramienta | Qué hace |
| --- | --- |
| `source-read` | Lee un archivo fuente C++ del proyecto (por fragmentos) |
| `source-create-class` | Genera una nueva clase C++ (header + cpp a partir de plantillas) |
| `source-update` | Edita un archivo fuente |
| `source-delete` | Elimina un archivo fuente |
| `source-list` | Lista los archivos fuente del módulo |
| `source-compile` | Compila el C++ del proyecto (Live Coding si está activo, si no UBT) con un informe de errores estructurado; para clases nuevas o declaraciones reflejadas cambiadas (`restartRequired`) indica cómo recompilar y reiniciar el editor |

Todas las operaciones de archivo están confinadas a `<Project>/Source/`.

</details>

<details>
  <summary><b>Familia Captura de pantalla / captura del viewport</b></summary>

| Id de herramienta | Qué hace |
| --- | --- |
| `screenshot-viewport` | Captura el viewport activo del editor; el `savePath` opcional también escribe el PNG dentro del proyecto |
| `screenshot-game-view` | Captura la vista de PIE / del juego; `savePath` opcional como arriba |
| `screenshot-camera` | Renderiza desde un actor de cámara resuelto mediante `USceneCaptureComponent2D`; `savePath` opcional como arriba |
| `screenshot-isolated` | Renderiza un actor aislado (SceneCapture2D transitorio + lista show-only); `savePath` opcional como arriba |

Las capturas devuelven un **PNG en base64 como contenido de imagen MCP** para que el LLM pueda inspeccionar el render directamente. Cada captura renderiza un frame nuevo en el momento de la llamada, también con la ventana del editor en segundo plano o minimizada, y su texto indica si el frame es reciente y si los efectos y la animación estaban avanzando. Mientras hay un agente de IA conectado, el plugin mantiene el editor actualizándose y renderizando a pleno ritmo en segundo plano (también minimizado) ("Use Less CPU when in Background" se anula solo durante esa sesión; tu preferencia guardada no cambia). Las dimensiones están limitadas (por defecto 1024, máximo absoluto 2048 por lado). La captura de píxeles necesita un editor que renderice: con ventana, o headless con `-RenderOffscreen` (el valor por defecto de `unreal-mcp-cli`). Con `-nullrhi` no se renderiza nada, así que estas herramientas devuelven un error que menciona `-nullrhi` y la solución.

</details>

<details>
  <summary><b>Herramientas de sistema — NO expuestas a tu IA</b></summary>

Son **herramientas de sistema**: infraestructura del host que `unreal-mcp-cli` y la app de escritorio llaman directamente por `POST /api/system-tools/<name>`. No aparecen a propósito en el `tools/list` de MCP, así que nunca se muestran en una sesión de IA (ni gastan tokens en ella), la misma separación que usan Unity y Godot para sus herramientas de sistema.

| Id de herramienta | Qué hace |
| --- | --- |
| `ping` | Sonda de actividad: hace el recorrido de ida y vuelta por la cadena plugin ⇄ sidecar ⇄ servidor |
| `unreal-skill-create` | Genera una nueva herramienta MCP como archivo C++ en el módulo de editor del plugin. **Requiere recompilar el editor** antes de poder llamar a la nueva herramienta (Live Coding, o recompilar el proyecto); el resultado lo indica de forma explícita. Necesita una instalación del plugin desde el código fuente |
| `unreal-skill-generate` | Regenera cada `SKILL.md` a partir de las herramientas registradas en una carpeta de skills relativa al proyecto (por defecto `.claude/skills`) |
| `editor-quit` | Guarda los paquetes modificados que tienen ruta y cierra un editor que inició una sesión de la CLI (`unreal-mcp-cli close --owned-only`). Se niega si `nonce` no coincide con el lease de la sesión, así que nunca cierra un editor que abriste tú |

</details>

## Extensiones

Instala una extensión cuando tu proyecto use uno de los sistemas del motor de abajo, o [crea la tuya](EXTENSIONS.md). Cada extensión es un módulo RQL: amplía las herramientas principales `rql-query` / `rql-mutate` / `rql-schema` y añade su guía a la skill `unreal-rql`, así que no aparecen herramientas MCP separadas en `tools/list`.

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas "<UEProject>"
```

Las extensiones requieren Unreal Engine 5.7+, Unreal MCP 0.26.0+ en el mismo proyecto y una toolchain de C++: las releases se distribuyen como código fuente y se compilan para tu proyecto (al abrir el editor la próxima vez, o en el momento con `--build`). El id de instalación es `com.ivanmurzak.unreal-ai-<id>` con el id de la tabla de abajo. Opciones de instalación (`--version`, `--build`, `--source` offline): [Installing an extension into a project](EXTENSIONS.md#installing-an-extension-into-a-project).

| Extensión (id) | Descripción |
| --- | --- |
| **[AI&nbsp;Control&nbsp;Rig](../extensions/UnrealAIControlRig/README.md)**<br>`control-rig` | Crea rigs con huesos, controles y un solver IK de dos huesos; consulta blueprints y componentes de Control Rig |
| **[AI&nbsp;Enhanced&nbsp;Input](../extensions/UnrealAIEnhancedInput/README.md)**<br>`enhanced-input` | Consulta Input Actions y Mapping Contexts, crea acciones y contextos, asigna teclas |
| **[AI GAS](../extensions/UnrealAIGAS/README.md)**<br>`gas` | Creación con Gameplay Ability System sin C++: efectos (coste, cooldown, daño, regeneración), Blueprints de habilidades, componentes de sistema de habilidades con un attribute set listo de Health / Mana / Stamina, bindings de input, comprobaciones en Play In Editor |
| **[AI Landscape](../extensions/UnrealAILandscape/README.md)**<br>`landscape` | Landscape y Water: crea, esculpe y pinta landscapes; crea océanos, lagos y ríos; consulta landscapes, water bodies y water zones |
| **[AI Niagara](../extensions/UnrealAINiagara/README.md)**<br>`niagara` | VFX de Niagara: consulta sistemas y sus emisores, coloca sistemas en el nivel |
| **[AI PCG](../extensions/UnrealAIPCG/README.md)**<br>`pcg` | Procedural Content Generation: crea grafos PCG (nodos, conexiones, ajustes, parámetros), colócalos en volúmenes, genera dispersiones (bosques, rocas, props) y lee los recuentos |

## Visibilidad por herramienta

Cada herramienta tiene una casilla **Visible** en la ventana **MCP Tools**: la pestaña independiente **MCP Tools** (registrada en el menú **Tools** del editor). La ventana muestra el título, la familia y la descripción de cada herramienta, más una línea de resumen "N / M tools visible". **Visible** solo decide si la herramienta **aparece en el `tools/list` de MCP**, es decir, si los agentes de IA la ven. No es un control de acceso: una herramienta oculta sigue funcionando cuando un cliente la llama por nombre (`tools/call`), y `unreal-tool-list` también lista las herramientas ocultas (marcadas como ocultas) para que un agente pueda encontrarlas.

Se aplican dos ajustes (ver [ARCHITECTURE](ARCHITECTURE.md) §7/§8):

- la **lista blanca `enabledTools`** (se puede sobrescribir con `UNREAL_MCP_TOOLS`) decide si una herramienta **se sirve en absoluto**: si no está vacía, solo se sirven las herramientas listadas, y cualquier otra ni se lista ni se puede llamar (`Execute()` la rechaza); vacía significa "sin filtro";
- la **lista `hiddenTools`** (las casillas Visible que cambias en la UI) oculta una herramienta servida de `tools/list`.

Ambas se conservan entre sesiones del editor y sobreviven a una recarga en caliente de una extensión (una herramienta registrada de nuevo hereda el ajuste guardado, así que una recompilación nunca muestra una herramienta que ocultaste). Un archivo de configuración de una versión anterior del plugin sigue funcionando: su lista `disabledTools` se lee como `hiddenTools`, y el siguiente guardado escribe `hiddenTools`.

Todos los ajustes de conexión están en la sección Connection de la única ventana principal **AI Game Developer** (no hay una pestaña Settings aparte ni una página en Project Settings, igual que en Unity-MCP). Las ventanas **MCP Prompts** y **MCP Resources** están conectadas pero llegan vacías en esta release: cada una muestra un mensaje discreto de estado vacío (el resumen "N / M visible" es exclusivo de la ventana Tools).
