<b>[English](RUNTIME.md) | [中文](RUNTIME.zh-CN.md) | [日本語](RUNTIME.ja.md) | [Español](RUNTIME.es.md) | [فارسی](RUNTIME.fa.md) | [हिन्दी](RUNTIME.hi.md) | [العربية](RUNTIME.ar.md)</b>

# Uso en runtime (dentro del juego)

Por defecto Unreal-MCP controla el **editor**. También puede ejecutarse **dentro de un juego en ejecución** (PIE, Standalone o una build **Development** empaquetada) para que un asistente de IA controle tu juego en vivo. Es la contraparte en Unreal del [soporte de runtime (dentro del juego) de Unity-MCP](https://github.com/IvanMurzak/Unity-MCP#runtime-usage-in-game): el equivalente en UE de `UnityMcpPluginRuntime.Initialize().Build().Connect()` de Unity y de su ejemplo de bot de ajedrez con `[AiTool]`.

El punto de entrada en runtime es un `UGameInstanceSubsystem`, [`UUnrealMcpRuntimeSubsystem`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpRuntimeSubsystem.h) (en el **módulo de runtime** `UnrealMcpRuntime` del plugin). Se instancia automáticamente una vez por `UGameInstance`, pero **nunca se conecta automáticamente**: una conexión es siempre una llamada explícita y voluntaria (consulta [el contrato de seguridad](#runtime-security-contract) más abajo).

## Conectar de tres formas

Las tres llegan al mismo `UUnrealMcpRuntimeSubsystem::Connect(Host, Token, Mode, bAllowRemoteHost)`; el modo de conexión por defecto es **Custom** (un servidor loopback que aporta el desarrollador).

**1. Desde C++** (por ejemplo, en tu `GameMode::BeginPlay`):

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

`Get(WorldContext)` es un helper estático `BlueprintPure` que devuelve el subsistema de la game instance del contexto (o null). `Connect` devuelve `false` (y no conecta nada) si algún control de seguridad lo rechaza; ver más abajo.

**2. Desde Blueprint**: `Get Unreal MCP Runtime Subsystem` (el nodo `Get`, que tiene en cuenta el `WorldContext`) → **`Connect`** (un nodo `BlueprintCallable` en la categoría **Unreal MCP**; `Token` / `Mode` / `bAllowRemoteHost` son pines avanzados). Combínalo con el nodo **`Disconnect`** y el nodo puro **`Is Connected`** para conocer el estado.

**3. Desde la consola** (comodidad para QA, sin recompilar), registrados mientras el subsistema está vivo:

```
UnrealMcp.Connect <host> [token]
UnrealMcp.Disconnect
```

La vía de consola siempre usa loopback + modo Custom.

## Tus propias herramientas, prompts y recursos dentro del juego

Un juego incluye sus **propias** herramientas de gameplay (y, si son útiles, prompts y recursos) y la IA las controla en vivo: el equivalente en UE de `WithToolsFromAssembly` / el bot de ajedrez con `[AiTool]` de Unity. Creas los tres exactamente igual que para una extensión de editor ([EXTENSIONS.md](EXTENSIONS.md)), con dos cambios para un módulo de juego: hazlo **`Type=Runtime`** y que dependa de **`UnrealMcpRuntime`** (no de `UnrealMcpEditor`). Registra tus providers al iniciar el módulo, o usa los wrappers del subsistema, fáciles de descubrir: un par register/unregister por tipo:

```cpp
if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
{
    Mcp->RegisterToolProvider(MyToolProvider);          // tools merge in; manifest re-pushed
    Mcp->RegisterPromptProvider(MyPromptProvider);      // prompts merge in; manifest re-pushed
    Mcp->RegisterResourceProvider(MyResourceProvider);  // resources merge in; manifest re-pushed
}
// ... before destroying the providers, call the matching Unregister*Provider(...) for each.
```

El ejemplo completo y compilable es [`samples/UnrealAIRuntimeSample/`](../samples/UnrealAIRuntimeSample): un plugin `Type=Runtime` cuya herramienta **`game-time-dilation`** lee/cambia `AWorldSettings::TimeDilation` del mundo en vivo (cámara lenta / avance rápido), y que se puede llamar en un juego en ejecución a través de una conexión MCP en runtime. (Demuestra la vía de las herramientas; los prompts y recursos se registran con la misma API de tres wrappers mostrada arriba, y las familias principales incluidas son la referencia ejecutable.) Los detalles para autores (el contrato, el ciclo de vida, el orden, el aislamiento) están en [`docs/EXTENSIONS.md` → Runtime usage](EXTENSIONS.md#runtime-usage-in-game-extensions).

## Herramientas en runtime frente a solo de editor

Una conexión en runtime incluye exactamente **una** herramienta integrada: **`ping`** (una sonda de actividad). `ping` es una [herramienta de sistema](TOOLS.es.md), así que responde en `POST /api/system-tools/ping` y no se anuncia a un agente de IA. Es decir, un juego empaquetado que no registra nada propio presenta una lista de herramientas **vacía**, lo cual es una descripción honesta de él. Todo lo que puede hacer un agente de IA en runtime lo **aportas tú**: registra tus propias herramientas mediante el bus de extensiones de arriba (`RegisterToolProvider`) y aparecerán con normalidad.

Todas las familias de desarrollo del motor (la capa de datos RQL —`rql-query` / `rql-mutate` / `rql-schema` sobre actores, componentes, assets, niveles, la consola y el estado del editor—, todas las herramientas de captura, además de la creación de Blueprints, la edición y compilación de código C++, `console-run-command`, `unreal-tool-list`, `python-execute` y las herramientas de Fab) son **solo de editor** (las [30 herramientas de editor](TOOLS.es.md)). Controlan el editor y varias son de clase RCE (por ejemplo, `rql-mutate`, `console-run-command`, `python-execute`), así que por defecto no se compilan en un juego publicado. En un juego empaquetado no hay editor, así que la superficie integrada en runtime es intencionadamente solo `ping` + las herramientas que registre tu juego.

## <a id="editor-only-exclude-from-packaged-games"></a>Solo editor: excluir Unreal-MCP de los juegos empaquetados

Si solo quieres Unreal-MCP para **herramientas de IA dentro del Editor de Unreal** y piensas publicar un juego empaquetado **sin ningún rastro de Unreal-MCP** (sin módulo de runtime, sin el binario del sidecar `.NET` incluido), limita el plugin al editor con una **`TargetDenyList`** en el **`.uproject` de tu propio proyecto** (no en el `.uplugin` del plugin):

```jsonc
// <YourProject>.uproject — "Plugins" array
{
    "Name": "UnrealMCP",
    "Enabled": true,
    "TargetDenyList": [ "Game", "Client", "Server" ]
}
```

UE respeta `TargetDenyList` / `TargetAllowList` en una referencia de plugin (`PluginReferenceDescriptor::IsEnabledForTarget`): el plugin sigue activo en el target **Editor**, pero queda **excluido de las builds empaquetadas `Game` / `Client` / `Server`**, así que ni el módulo de infraestructura de runtime ni el sidecar `unreal-mcp-bridge` incluido se empaquetan ni se compilan en tu producto publicado. Los módulos de tipo editor (`UnrealMcpEditor`) se eliminan de una build de juego en cualquier caso; la deny-list además quita el módulo de infraestructura `UnrealMcpRuntime` y las `RuntimeDependencies` de su sidecar (§12.5), lo que te da **cero huella compilada**.

> **Advertencia: no crees una dependencia directa de módulo.** Una `TargetDenyList` solo excluye los módulos *propios* del plugin. Si uno de tus módulos de **juego** añade `UnrealMcpRuntime` (o cualquier módulo `UnrealMcp*`) a `PublicDependencyModuleNames` / `PrivateDependencyModuleNames` en su `*.Build.cs`, o hace `#include` de un header `UnrealMcp*`, esa dependencia directa **anula la deny-list** y UBT compila igualmente el módulo de runtime en tu juego. Un proyecto puramente de editor no referencia nada del plugin, así que se mantiene limpio por construcción; añade una dependencia de módulo solo cuando de verdad quieras [uso en runtime (dentro del juego)](#uso-en-runtime-dentro-del-juego).

Para uso en **runtime (dentro del juego)**, en cambio, deja la deny-list **desactivada**: el módulo de infraestructura `UnrealMcpRuntime` + el `ping` integrado se incluyen en la build empaquetada (§12.5), y registras tus propias herramientas de gameplay mediante `IUnrealMcpToolProvider`; consulta [Conectar de tres formas](#conectar-de-tres-formas) y [EXTENSIONS.md](EXTENSIONS.md). Las dos opciones son mutuamente excluyentes: deny-list para un proyecto solo de editor, sin deny-list para un juego que aloja una conexión MCP en vivo.

## <a id="runtime-security-contract"></a>Contrato de seguridad

Una conexión en runtime es **control remoto de un juego en ejecución** (crear actores, escribir cualquier propiedad reflejada, CVars arbitrarias mediante `console-run-command`, llamar a `UFunction`s): de clase RCE si fuera accesible en un producto publicado. Por eso la superficie de runtime está blindada con **cinco mitigaciones en capas** ([`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §12.8), todas aplicadas dentro de `Connect()`:

1. **Solo voluntario (opt-in).** El subsistema se instancia automáticamente pero **nunca se conecta automáticamente**: no hay marcado automático al cargar ni conexión automática desde un asset de configuración. Una conexión solo ocurre mediante una llamada explícita a `Connect()`.
2. **Interruptor de seguridad, desactivado por defecto.** `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled` (Project Settings → Plugins → **Unreal MCP (Runtime)**) vale **`false`** por defecto. Mientras está desactivado, se rechaza cada `Connect()` y no se lanza ningún sidecar. Es un ajuste de configuración **Game** (`DefaultGame.ini`, sección `[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]`), así que viaja a la build empaquetada, donde el control debe tener efecto. Activarlo **no** conecta automáticamente: sigue haciendo falta un `Connect()` explícito.
3. **Control de Shipping.** El flag `bUnrealMcpAllowShipping` de `*.Build.cs` vale **`false`** por defecto (→ `UNREAL_MCP_ALLOW_SHIPPING=0`); en una build **Shipping**, `Connect()` escribe en el log y devuelve `false` salvo que ese flag se haya compilado a propósito. Las builds Development / PIE no se ven afectadas.
4. **Host loopback por defecto.** `Connect()` rechaza cualquier `Host` que no sea loopback salvo que quien llama pase explícitamente `bAllowRemoteHost = true`. El valor por defecto mantiene la conexión en `localhost` / `127.0.0.0/8` / `::1`.
5. **IPC loopback + token de un solo uso por stdin.** El IPC plugin ⇄ sidecar es solo loopback y se autentica con un token de un solo uso entregado por **stdin, nunca por argv**, y nunca se escribe en el log (el mismo modelo que el editor, ARCHITECTURE §1.4).

Además, la vía de runtime es **solo de escritorio** (Win64 / Mac / Linux): lanzar el proceso sidecar `unreal-mcp-bridge` de .NET no es posible en consolas ni en plataformas móviles, así que esas plataformas no tienen ninguna superficie MCP en runtime.

> **Guía de extremo a extremo.** El recorrido completo para el operador (activar el interruptor de seguridad, conectar desde PIE y desde una build Development empaquetada, y probar herramientas por la conexión en vivo) está en [`docs/RUNTIME-E2E.md`](RUNTIME-E2E.md). La parte determinista (controles de seguridad, cambio del resolvedor de mundo, conexión/desconexión/seguridad ante huérfanos) está cubierta por specs de Automation headless.
