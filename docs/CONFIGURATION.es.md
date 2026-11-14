<b>[English](CONFIGURATION.md) | [中文](CONFIGURATION.zh-CN.md) | [日本語](CONFIGURATION.ja.md) | [Español](CONFIGURATION.es.md) | [فارسی](CONFIGURATION.fa.md) | [हिन्दी](CONFIGURATION.hi.md) | [العربية](CONFIGURATION.ar.md)</b>

# Configuración, actualizaciones y solución de problemas

Cómo conectar el editor, mantener el plugin al día, ajustarlo con variables de entorno y resolver los problemas habituales. Los pasos de instalación están en el [README](README.es.md); el diseño detrás de todo esto está en [ARCHITECTURE.md](ARCHITECTURE.md).

- [Primer arranque](#primer-arranque)
- [Actualizar el plugin](#actualizar-el-plugin)
- [Variables de entorno](#variables-de-entorno)
- [Solución de problemas](#solución-de-problemas)

## Primer arranque

1. Abre la ventana principal **AI Game Developer** desde el menú **Tools** del editor (la pestaña está registrada en la categoría del menú Tools).
2. **Elige un modo de conexión:**
   - **Cloud** (por defecto): se conecta a [ai-game.dev](https://ai-game.dev). Pulsa **Authorize** para iniciar el **flujo de device code** de OAuth: la ventana muestra una URL de verificación y un código de usuario corto; abre la URL, introduce el código, aprueba, y el editor termina la autorización. Usa **Revoke** para borrar el token de la nube guardado.
   - **Custom**: se conecta a un `gamedev-mcp-server` local que ejecutas tú (o a cualquier servidor compatible). Introduce la URL del servidor y apunta tu cliente de IA a él. Ejecuta el servidor tú mismo, o deja que el editor lo inicie y lo detenga (`UNREAL_MCP_START_SERVER`, solo modo Custom + http; consulta [Variables de entorno](#variables-de-entorno)).
3. La sección **Connection** muestra un punto de estado, una etiqueta de estado y un botón Connect / Disconnect / Stop; el estado del bridge indica `Running (restarts: N)` o `Stopped`. Úsalos para confirmar que el sidecar está activo.
4. Apunta tu cliente de IA (Claude Code, Cursor, la app AI Game Developer, …) al servidor. La sección **AI agents** lista los agentes conectados en ese momento; para escribir la configuración de un cliente MCP usa `unreal-mcp-cli setup-mcp`.

Los ajustes de conexión se guardan en `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` (todas las plantillas de UE ignoran `Saved/` en git, así que por defecto los tokens nunca acaban en el control de versiones).

> **URL fijada del cliente MCP.** `unreal-mcp-cli setup-mcp <agent>` escribe una configuración de cliente MCP que apunta a la URL de la nube **fijada al proyecto** `<base>/mcp/p/<pin>`, para que el agente se dirija al editor de **este** proyecto aunque tu cuenta controle varios. Pasa `--no-pin` para escribir en su lugar la URL simple `<base>/mcp`. El pin es solo un segmento de ruta para el enrutamiento: el recurso OAuth sigue siendo `<base>/mcp`.

> **Clave de proyecto.** Cuando esta máquina tiene la sesión iniciada (`unreal-mcp-cli login`, o el Sign in del editor), una configuración Cloud también lleva `Authorization: Bearer agd_pk_…` (una clave que no caduca, revocable y vinculada al pin de este proyecto) para cada agente, así que ningún agente necesita su propio OAuth. La clave se guarda en caché en `~/.ai-game-dev/project-keys.json` y se reutiliza. Pasa `--oauth` para una configuración solo con URL (el agente inicia sesión por sí mismo), o `--regenerate-key` para generar una clave nueva y revocar la anterior (el panel de agentes de IA del editor tiene un botón **Regenerate key**). Sin sesión iniciada, o si el servidor no puede generar una clave ⇒ la configuración solo con URL.

## Actualizar el plugin

Actualizar en el sitio siempre debe dejarte ejecutando el código **nuevo**. El riesgo es el compilador incremental de UE: si el código fuente del plugin cambia (archivos `.cpp` nuevos, un módulo nuevo) pero sobrevive la antigua caché de compilación `UnrealMCP/Intermediate/`, UE puede hacer una recompilación parcial con una lista de archivos del módulo obsoleta y dejarte en silencio con código antiguo o parcial. Cada canal lo resuelve de forma distinta:

- **Fab / Epic Marketplace (cuando el listing esté publicado) → automático.** El Epic Games Launcher sustituye los binarios precompilados en el sitio; no hay nada que compilar ni caché que limpiar.
- **`unreal-mcp-cli update` → recompilación limpia automática.** `update` vuelve a copiar el código fuente del plugin y, **por defecto**, borra el `Intermediate/` obsoleto y los `Binaries/` de C++ del plugin instalado para que UE haga una compilación limpia en el siguiente arranque del editor, sin pasos manuales. El bridge sidecar incluido en `Binaries/ThirdParty/UnrealMcpBridge/<rid>/` se conserva intacto: las instalaciones desde el código fuente de una release lo renuevan desde `Source/ThirdParty/...`, mientras que las instalaciones de repo/desarrollo conservan la copia incluida anterior cuando hace falta. Las instalaciones de desarrollo con **junction** nunca se limpian (eso borraría los resultados de compilación de tu árbol de código en vivo). Pasa `--no-clean` para no borrar la caché.

  ```bash
  unreal-mcp-cli update <YourProject>            # default: clean rebuild on version change
  unreal-mcp-cli update <YourProject> --force    # re-copy even when versions match
  unreal-mcp-cli update <YourProject> --no-clean # keep the existing build cache
  unreal-mcp-cli update <YourProject> --no-extensions # leave the installed extensions as they are
  unreal-mcp-cli update <YourProject> --no-self  # leave the CLI as it is
  unreal-mcp-cli update <YourProject> --check    # only report; exit code 2 when something is outdated
  ```

- **Copia manual → limpia la caché tú mismo.** Si sobrescribes `<YourProject>/Plugins/UnrealMCP/` a mano, **cierra primero el editor**, borra `<YourProject>/Plugins/UnrealMCP/Intermediate/` y los `Binaries/` de C++ (conserva `Binaries/ThirdParty/` si hay un bridge incluido) y vuelve a abrirlo para que UE recompile desde cero.

## Variables de entorno

El plugin lee la configuración con esta precedencia: **entorno del proceso → `<Project>/.env` → archivo de configuración → valores por defecto integrados**. Las variables `UNREAL_MCP_*` reconocidas (definidas en `UnrealMCP/Source/UnrealMcpRuntime/Private/Config/UnrealMcpConfig.cpp`; `UNREAL_MCP_CREDENTIALS_DIR` la lee el bridge, `UNREAL_MCP_SERVER_PATH` la CLI):

| Variable | Propósito |
| --- | --- |
| `UNREAL_MCP_CONNECTION_MODE` | `Cloud` o `Custom`. **Por defecto `Cloud`.** |
| `UNREAL_MCP_HOST` | URL del servidor (modo Custom), por ejemplo `http://localhost:<port>` |
| `UNREAL_MCP_CLOUD_URL` | URL del backend en la nube (por defecto ai-game.dev) |
| `UNREAL_MCP_TOKEN` | Token de autenticación (token IPC del sidecar / token del servidor). **Secreto: nunca lo subas al repositorio.** |
| `UNREAL_MCP_AUTH_OPTION` | `none` u `oauth` (autenticación del servidor local) |
| `UNREAL_MCP_KEEP_CONNECTED` | Guardar/restaurar el estado de conexión |
| `UNREAL_MCP_TOOLS` | Sobrescritura de las herramientas activadas (lista blanca; vacía = sin filtro) |
| `UNREAL_MCP_START_SERVER` | `true`: el editor inicia por sí mismo el `gamedev-mcp-server` local en cuanto el bridge informa del puerto del proyecto (solo modo Custom + http; se guarda como `startServer`) y lo detiene al salir. `unreal-mcp-cli exec` / `open --headless` la definen. |
| `UNREAL_MCP_TRANSPORT` | `stdio` o `http` |
| `UNREAL_MCP_LOG_LEVEL` | Nivel de detalle del log |
| `UNREAL_MCP_TOOL_TIMEOUT` | Cuánto puede durar una llamada a herramienta en el editor, en milisegundos (por defecto `300000`; se guarda como `toolTimeoutMs`). Súbelo para snippets largos de `python-execute` o trabajo con assets. El editor lo pasa, más 10 s, como `tool-timeout` al servidor local que inicia; un servidor anterior (antes de McpPlugin 8.11.0) limita las llamadas a 5 min diga lo que diga este valor ([ARCHITECTURE §4](ARCHITECTURE.md)). Una llamada que agota el tiempo o se cancela antes de que el editor la empiece se omite, no se ejecuta más tarde. |
| `UNREAL_MCP_BRIDGE_PATH` | Ruta a un binario del sidecar: la **alternativa para desarrollo/CI**; tiene prioridad sobre el binario incluido (§6). Una release empaquetada resuelve sola el sidecar incluido, así que los usuarios finales nunca la definen; las builds desde el código fuente la usan. |
| `UNREAL_MCP_CREDENTIALS_DIR` | Carpeta absoluta del almacén de credenciales de la máquina que el bridge lee (y renueva) en lugar de `~/.ai-game-dev`, definida en el entorno del proceso del editor (no se lee de `.env`): una ruta, nunca un token; un valor relativo se registra en el log y se ignora. Para editores de desarrollo/pruebas cuyo lanzador inicia sesión con otro perfil (el entorno de desarrollo local de la AI Game Dev App); la gemela de `GODOT_MCP_CREDENTIALS_DIR` de Godot. |
| `UNREAL_MCP_SERVER_PATH` | Ruta a un binario local de `gamedev-mcp-server` (la lee `unreal-mcp-cli`, no el plugin): omite la descarga del servidor y la comprobación de versión (§6). |
| `AIGD_SESSION_IDLE_SECONDS` | La define `unreal-mcp-cli` en un editor que inicia: tras este número de segundos sin llamadas a herramientas (las llamadas en curso, play-in-editor, la compilación de assets y la entrada del usuario cuentan como actividad), el editor guarda los paquetes modificados que tienen ruta y se cierra. Solo se activa junto con `AIGD_SESSION_OWNER=cli` y un nonce, y solo mientras el lease del proyecto siga nombrando a este editor; sin definir = nunca. |
| `AIGD_SESSION_OWNER` / `AIGD_SESSION_NONCE` | `cli` y el nonce de `<project>/.ai-game-dev/session.json`: marcan un editor que inició una sesión de la CLI. Solo un editor así acepta `editor-quit`, y al salir elimina el lease si todavía lleva su nonce. |

> **Nunca subas `.env` al repositorio.** Un `.env` en la raíz del proyecto puede contener `UNREAL_MCP_TOKEN`, y las plantillas de proyecto de UE no incluyen `.gitignore`. `unreal-mcp-cli configure` añade `.env` al `.gitignore` del proyecto de destino; la estructura base de este repositorio ya lo ignora. El token IPC del sidecar viaja por stdin (nunca por argv) y nunca se escribe en el log.

## Solución de problemas

- **`No connected clients. Retrying [1..10]` y después HTTP 500 de un servidor local.** El modo de conexión quedó en `Cloud` por defecto, así que el sidecar llamó a ai-game.dev en lugar de a tu servidor local. Define `UNREAL_MCP_CONNECTION_MODE=Custom` (entorno, `.env` o el interruptor de la UI).
- **No aparece la línea `[Unreal-MCP] plugin loaded` al arrancar.** El módulo de editor no se pudo cargar: revisa el Output Log en busca de un error de `StartupModule` o de un `.uplugin` mal formado.
- **El cuerpo de una herramienta devuelve `'x' is required.` cuando se invoca por el passthrough REST.** Envía `-H "Content-Type: application/json"`; sin él, el servidor descarta el cuerpo JSON.
- **Las herramientas de captura devuelven `Screenshot tools are unavailable: this editor runs with the null RHI (-nullrhi)`.** Con `-nullrhi` no se renderiza nada. Reinicia el editor sin esa opción: con ventana, o headless con `-RenderOffscreen`, que sí renderiza capturas (con `unreal-mcp-cli`, omite `--nullrhi`).
- **No hay sidecar / el sidecar se reinicia una y otra vez.** Revisa el estado del bridge en la sección **Connection** (`Running (restarts: N)` / `Stopped`). Si el log muestra `no sidecar binary resolved for rid <rid> …`, no se resolvió ni un binario incluido, ni uno descargado, ni `UNREAL_MCP_BRIDGE_PATH`. Una instalación desde Fab necesita instalar el bridge a mano: **Get bridge** abre la página de la release; extrae el zip de tu plataforma en la carpeta que muestra **Open bridge folder** y luego pulsa **Connect** (la build de Fab nunca descarga ni inicia un programa por sí misma). Una release de la CLI / empaquetada lo incluye, así que ahí significa que el plugin se empaquetó sin el bridge de tu plataforma (reinstala la build correcta). En una build desde el código fuente, define `UNREAL_MCP_BRIDGE_PATH` o ejecuta `unreal-mcp-cli bootstrap-local` para compilar uno desde el código fuente ([ARCHITECTURE](ARCHITECTURE.md) §6.3, §6.8).
- **Puertos.** El IPC usa un puerto determinista por proyecto en `30000–39999` y, si hay colisión, prueba los siguientes (y después un puerto efímero); el servidor local usa un puerto determinista por hash en `20000–29999` sin probar otros: la CLI calcula el mismo número sin leer ninguna configuración, y el servidor se enlaza exactamente al puerto pedido. El número exacto es una comodidad para depurar, no un requisito.
- **Logs.** Usa la acción **Open log file** de la ventana principal, una consulta RQL (`console | where Verbosity == "Error"`) o el Output Log del editor (categoría `LogUnrealMcp`).
