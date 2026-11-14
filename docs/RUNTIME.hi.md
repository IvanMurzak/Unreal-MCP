<b>[English](RUNTIME.md) | [中文](RUNTIME.zh-CN.md) | [日本語](RUNTIME.ja.md) | [Español](RUNTIME.es.md) | [فارسی](RUNTIME.fa.md) | [हिन्दी](RUNTIME.hi.md) | [العربية](RUNTIME.ar.md)</b>

# Runtime उपयोग (in-game)

Default रूप से Unreal-MCP **editor** को चलाता है। यह **चलते हुए game के अंदर** भी चल सकता है — PIE, Standalone, या packaged **Development** build — ताकि AI assistant आपके game को live चला सके। यह [Unity-MCP के runtime (in-game) support](https://github.com/IvanMurzak/Unity-MCP#runtime-usage-in-game) का Unreal रूप है: Unity के `UnityMcpPluginRuntime.Initialize().Build().Connect()` और उसके `[AiTool]` Chess-bot sample का UE समकक्ष।

Runtime entry point एक `UGameInstanceSubsystem` है, [`UUnrealMcpRuntimeSubsystem`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpRuntimeSubsystem.h) (plugin के `UnrealMcpRuntime` **runtime module** में)। यह हर `UGameInstance` के लिए अपने-आप एक बार बनता है, लेकिन **कभी अपने-आप connect नहीं होता** — connection हमेशा एक स्पष्ट, opt-in call होता है (नीचे [security contract](#runtime-security-contract) देखें)।

## तीन तरीकों से connect करें

तीनों एक ही `UUnrealMcpRuntimeSubsystem::Connect(Host, Token, Mode, bAllowRemoteHost)` तक पहुँचते हैं; connection mode का default **Custom** है (developer द्वारा दिया गया loopback server)।

**1. C++ से** (जैसे आपके `GameMode::BeginPlay` में):

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

`Get(WorldContext)` एक static `BlueprintPure` helper है जो context के game instance का subsystem लौटाता है (या null)। अगर कोई security gate मना करे तो `Connect` `false` लौटाता है (और कुछ connect नहीं करता) — नीचे देखें।

**2. Blueprint से** — `Get Unreal MCP Runtime Subsystem` (`Get` node, `WorldContext`-aware) → **`Connect`** (**Unreal MCP** category के तहत एक `BlueprintCallable` node; `Token` / `Mode` / `bAllowRemoteHost` advanced pins हैं)। Status के लिए इसे **`Disconnect`** node और **`Is Connected`** pure node के साथ इस्तेमाल करें।

**3. Console से** (QA की सुविधा, recompile की ज़रूरत नहीं) — subsystem के जीवित रहने तक registered:

```
UnrealMcp.Connect <host> [token]
UnrealMcp.Disconnect
```

Console वाला रास्ता हमेशा loopback + Custom mode इस्तेमाल करता है।

## आपके अपने in-game tools, prompts और resources

Game अपने **खुद के** gameplay tools (और ज़रूरत हो तो prompts और resources) ship करता है और AI उन्हें live चलाता है — Unity के `WithToolsFromAssembly` / `[AiTool]` Chess-bot का UE समकक्ष। तीनों को आप बिल्कुल editor extension की तरह लिखते हैं ([EXTENSIONS.md](EXTENSIONS.md)), game module के लिए दो बदलावों के साथ: उसे **`Type=Runtime`** बनाएँ और **`UnrealMcpRuntime`** पर depend करें (`UnrealMcpEditor` पर नहीं)। अपने providers module startup पर register करें, या subsystem के discoverable wrappers इस्तेमाल करें — हर प्रकार के लिए एक register/unregister जोड़ी:

```cpp
if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
{
    Mcp->RegisterToolProvider(MyToolProvider);          // tools merge in; manifest re-pushed
    Mcp->RegisterPromptProvider(MyPromptProvider);      // prompts merge in; manifest re-pushed
    Mcp->RegisterResourceProvider(MyResourceProvider);  // resources merge in; manifest re-pushed
}
// ... before destroying the providers, call the matching Unregister*Provider(...) for each.
```

पूरा, build होने वाला उदाहरण [`samples/UnrealAIRuntimeSample/`](../samples/UnrealAIRuntimeSample) है — एक `Type=Runtime` plugin, जिसका **`game-time-dilation`** tool live world का `AWorldSettings::TimeDilation` पढ़ता/set करता है (slow-motion / fast-forward), और जिसे runtime MCP connection पर चलते game में call किया जा सकता है। (यह tool वाला रास्ता दिखाता है; prompts और resources ऊपर दिखाए गए उसी तीन-wrapper API से register होते हैं, और ship होने वाली core families चलने लायक reference हैं।) Author की तरफ़ के विवरण (contract, lifecycle, ordering, isolation) [`docs/EXTENSIONS.md` → Runtime usage](EXTENSIONS.md#runtime-usage-in-game-extensions) में हैं।

## Runtime tool set बनाम editor-only

Runtime connection ठीक **एक** built-in tool के साथ आता है: **`ping`** (liveness probe)। `ping` एक [system tool](TOOLS.hi.md) है, इसलिए यह `POST /api/system-tools/ping` पर जवाब देता है और AI agent को advertise नहीं होता — यानी जो packaged game अपना कुछ register नहीं करता, वह **खाली** tool list दिखाता है, जो उसका ईमानदार विवरण है। Runtime AI agent जो कुछ भी कर सकता है, वह **आपको खुद लाना होता है**: ऊपर वाले extension bus (`RegisterToolProvider`) से अपने tools register करें और वे सामान्य रूप से दिखेंगे।

सभी engine-development families — RQL data layer (actors, components, assets, levels, console और editor state पर `rql-query` / `rql-mutate` / `rql-schema`), हर screenshot tool, साथ ही Blueprint authoring, C++ source edit और compile, `console-run-command`, `unreal-tool-list`, `python-execute` और Fab tools — **सिर्फ़ editor के लिए** हैं ([30 editor tools](TOOLS.hi.md))। ये editor को चलाते हैं और इनमें से कई RCE-class हैं (जैसे `rql-mutate`, `console-run-command`, `python-execute`), इसलिए default रूप से ये shipped game में compile नहीं होते। Packaged game में कोई editor नहीं होता, इसलिए runtime built-in surface जान-बूझकर सिर्फ़ `ping` + आपके game के registered tools है।

## <a id="editor-only-exclude-from-packaged-games"></a>Editor-only — packaged games से Unreal-MCP को बाहर रखें

अगर आप Unreal-MCP सिर्फ़ **Unreal Editor के अंदर AI tooling** के लिए चाहते हैं और packaged game को **Unreal-MCP के बिल्कुल बिना** ship करना चाहते हैं — न runtime module, न bundled `.NET` sidecar binary — तो **अपने consumer project की `.uproject`** में (plugin की `.uplugin` में नहीं) **`TargetDenyList`** से plugin को editor तक सीमित करें:

```jsonc
// <YourProject>.uproject — "Plugins" array
{
    "Name": "UnrealMCP",
    "Enabled": true,
    "TargetDenyList": [ "Game", "Client", "Server" ]
}
```

UE plugin reference पर `TargetDenyList` / `TargetAllowList` को मानता है (`PluginReferenceDescriptor::IsEnabledForTarget`): plugin **Editor** target में active रहता है, लेकिन **packaged `Game` / `Client` / `Server` builds से बाहर** रहता है — इसलिए न runtime infra module और न bundled `unreal-mcp-bridge` sidecar आपके shipped product में stage या compile होता है। Editor-type modules (`UnrealMcpEditor`) game build से वैसे भी हटा दिए जाते हैं; deny-list `UnrealMcpRuntime` infra module और उसकी sidecar `RuntimeDependencies` (§12.5) को भी हटा देती है, जिससे आपको **शून्य compiled footprint** मिलता है।

> **सावधानी — सीधी module dependency न बनाएँ।** `TargetDenyList` सिर्फ़ plugin के *अपने* modules को बाहर रखती है। अगर आपका कोई **game** module अपनी `*.Build.cs` की `PublicDependencyModuleNames` / `PrivateDependencyModuleNames` में `UnrealMcpRuntime` (या कोई भी `UnrealMcp*` module) जोड़ता है, या किसी `UnrealMcp*` header को `#include` करता है, तो वह सीधी dependency **deny-list को override कर देती है** और UBT फिर भी runtime module को आपके game में compile कर देता है। एक शुद्ध editor-only project plugin से कुछ reference नहीं करता, इसलिए वह अपनी बनावट से ही साफ़ रहता है — module dependency तभी चुनें जब आपको सच में [runtime (in-game) उपयोग](#runtime-उपयोग-in-game) चाहिए।

इसके बजाय **runtime (in-game)** उपयोग के लिए deny-list **बंद** रखें: `UnrealMcpRuntime` infra module + `ping` built-in packaged build में ship होते हैं (§12.5), और आप अपने gameplay tools `IUnrealMcpToolProvider` से register करते हैं — देखें [तीन तरीकों से connect करें](#तीन-तरीकों-से-connect-करें) और [EXTENSIONS.md](EXTENSIONS.md)। दोनों एक-दूसरे को छोड़ते हैं: editor-only project के लिए deny-list, live MCP connection host करने वाले game के लिए कोई deny-list नहीं।

## <a id="runtime-security-contract"></a>Security contract

Runtime connection **चलते हुए game का remote control** है (actors spawn करना, कोई भी reflected property लिखना, `console-run-command` से कोई भी CVar, `UFunction`s call करना) — shipped product में पहुँच योग्य होने पर यह RCE-class होगा। इसलिए runtime surface **पाँच परतों वाली सुरक्षा** से बंद है ([`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §12.8), जो सब `Connect()` के अंदर लागू होती हैं:

1. **सिर्फ़ opt-in।** Subsystem अपने-आप बनता है लेकिन **कभी अपने-आप connect नहीं होता** — load पर कोई auto-dial नहीं और config-asset से कोई auto-connect नहीं। Connection सिर्फ़ स्पष्ट `Connect()` call से ही होता है।
2. **Kill switch, default OFF।** `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled` (Project Settings → Plugins → **Unreal MCP (Runtime)**) का default **`false`** है। जब तक यह बंद है, हर `Connect()` मना होता है और कोई sidecar spawn नहीं होता। यह एक **Game** config setting है (`DefaultGame.ini`, section `[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]`), इसलिए यह packaged build में भी जाती है, जहाँ gate को असर करना है। इसे चालू करने से auto-connect **नहीं** होता — स्पष्ट `Connect()` फिर भी ज़रूरी है।
3. **Shipping gate।** `bUnrealMcpAllowShipping` `*.Build.cs` flag का default **`false`** है (→ `UNREAL_MCP_ALLOW_SHIPPING=0`); **Shipping** build में `Connect()` log करता है और `false` लौटाता है, जब तक वह flag जान-बूझकर compile न किया गया हो। Development / PIE builds पर असर नहीं होता।
4. **Loopback-host default।** `Connect()` हर non-loopback `Host` को मना करता है, जब तक caller स्पष्ट रूप से `bAllowRemoteHost = true` न दे। Default connection को `localhost` / `127.0.0.0/8` / `::1` पर रखता है।
5. **Loopback IPC + एक बार का stdin token।** Plugin ⇄ sidecar IPC सिर्फ़ loopback पर है और एक बार के token से authenticated है, जो **stdin से दिया जाता है, argv से कभी नहीं**, और कभी log नहीं होता (editor वाला ही model, ARCHITECTURE §1.4)।

इसके अलावा, runtime रास्ता **सिर्फ़ Desktop** (Win64 / Mac / Linux) के लिए है: console या mobile platforms पर .NET `unreal-mcp-bridge` sidecar process spawn करना संभव नहीं, इसलिए उन platforms पर runtime MCP surface है ही नहीं।

> **End-to-end runbook।** पूरी operator walkthrough — kill switch चालू करना, PIE और packaged Development build से connect करना, और live connection पर tools चलाना — [`docs/RUNTIME-E2E.md`](RUNTIME-E2E.md) में है। Deterministic हिस्सा (security gates, world-resolver switching, connect/disconnect/orphan-safety) headless Automation specs से पक्का किया गया है।
