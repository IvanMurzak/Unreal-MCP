<b>[English](RUNTIME.md) | [中文](RUNTIME.zh-CN.md) | [日本語](RUNTIME.ja.md) | [Español](RUNTIME.es.md) | [فارسی](RUNTIME.fa.md) | [हिन्दी](RUNTIME.hi.md) | [العربية](RUNTIME.ar.md)</b>

# ランタイムでの使用（ゲーム内）

既定では、Unreal-MCP が操作するのは**エディター**です。**実行中のゲームの中**（PIE、Standalone、パッケージ化した **Development** ビルド）でも動かせるので、AI アシスタントがゲームをライブで操作できます。これは [Unity-MCP のランタイム（ゲーム内）サポート](https://github.com/IvanMurzak/Unity-MCP#runtime-usage-in-game)に相当する Unreal 版で、Unity の `UnityMcpPluginRuntime.Initialize().Build().Connect()` と `[AiTool]` のチェスボットサンプルに対応します。

ランタイムの入口は `UGameInstanceSubsystem` である [`UUnrealMcpRuntimeSubsystem`](../UnrealMCP/Source/UnrealMcpRuntime/Public/UnrealMcpRuntimeSubsystem.h) です（プラグインの `UnrealMcpRuntime` **ランタイムモジュール**にあります）。`UGameInstance` ごとに 1 回自動でインスタンス化されますが、**自動で接続することはありません**。接続は常に、明示的なオプトインの呼び出しです（下の[セキュリティ契約](#runtime-security-contract)を参照）。

## 3つの接続方法

3 つとも同じ `UUnrealMcpRuntimeSubsystem::Connect(Host, Token, Mode, bAllowRemoteHost)` に行き着きます。接続モードの既定は **Custom**（開発者が用意したループバックのサーバー）です。

**1. C++ から**（例: `GameMode::BeginPlay`）:

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

`Get(WorldContext)` は静的な `BlueprintPure` ヘルパーで、コンテキストのゲームインスタンスに対応するサブシステム（または null）を返します。いずれかのセキュリティゲートが拒否すると、`Connect` は `false` を返します（何も接続しません）。詳しくは下を参照してください。

**2. Blueprint から** — `Get Unreal MCP Runtime Subsystem`（`Get` ノード、`WorldContext` 対応）→ **`Connect`**（**Unreal MCP** カテゴリの `BlueprintCallable` ノード。`Token` / `Mode` / `bAllowRemoteHost` は詳細ピン）。**`Disconnect`** ノードと、状態確認用の純粋ノード **`Is Connected`** と組み合わせて使います。

**3. コンソールから**（QA 向けの手軽な方法で、再コンパイル不要）— サブシステムが生きている間に登録されます:

```
UnrealMcp.Connect <host> [token]
UnrealMcp.Disconnect
```

コンソールからの接続は、常にループバック + Custom モードです。

## ゲーム内の独自ツール、プロンプト、リソース

ゲームは**独自の**ゲームプレイ用ツール（必要ならプロンプトとリソースも）を持ち、AI がそれをライブで操作できます。Unity の `WithToolsFromAssembly` / `[AiTool]` チェスボットに相当します。3 つともエディター拡張機能とまったく同じ方法で作成します（[EXTENSIONS.md](EXTENSIONS.md)）。ゲームモジュールで変わるのは 2 点だけです: **`Type=Runtime`** にすることと、（`UnrealMcpEditor` ではなく）**`UnrealMcpRuntime`** に依存することです。プロバイダーはモジュールの起動時に登録するか、サブシステムの見つけやすいラッパーを使います。種類ごとに登録/登録解除のペアがひとつずつあります:

```cpp
if (UUnrealMcpRuntimeSubsystem* Mcp = UUnrealMcpRuntimeSubsystem::Get(this))
{
    Mcp->RegisterToolProvider(MyToolProvider);          // tools merge in; manifest re-pushed
    Mcp->RegisterPromptProvider(MyPromptProvider);      // prompts merge in; manifest re-pushed
    Mcp->RegisterResourceProvider(MyResourceProvider);  // resources merge in; manifest re-pushed
}
// ... before destroying the providers, call the matching Unregister*Provider(...) for each.
```

ビルドできる完全な例は [`samples/UnrealAIRuntimeSample/`](../samples/UnrealAIRuntimeSample) です。`Type=Runtime` のプラグインで、その **`game-time-dilation`** ツールは動作中のワールドの `AWorldSettings::TimeDilation`（スローモーション / 早送り）を読み書きし、実行中のゲームでランタイム MCP 接続を通じて呼び出せます。（示しているのはツールの経路です。プロンプトとリソースは上の 3 つのラッパーと同じ API で登録し、同梱のコアファミリーが動く参考例になります。）作成者向けの詳細（契約、ライフサイクル、順序、分離）は [`docs/EXTENSIONS.md` → Runtime usage](EXTENSIONS.md#runtime-usage-in-game-extensions) にあります。

## ランタイムのツールセットとエディター専用ツール

ランタイム接続に含まれる組み込みツールは**ひとつ**だけです: **`ping`**（死活確認）。`ping` は[システムツール](TOOLS.ja.md)なので、`POST /api/system-tools/ping` で応答し、AI エージェントには公開されません。つまり、独自のものを何も登録していないパッケージ化ゲームのツール一覧は**空**になり、それがそのゲームの正直な姿です。ランタイムの AI エージェントにできることはすべて**持ち込み**です。上の拡張バス（`RegisterToolProvider`）で独自のツールを登録すれば、普通に現れます。

エンジン開発用のファミリーはすべて**エディター専用**です（[エディターツール](TOOLS.ja.md)）。RQL データレイヤー（アクター、コンポーネント、アセット、レベル、コンソール、エディターの状態を扱う `rql-query` / `rql-mutate` / `rql-schema`）、すべてのスクリーンショットツール、さらに Blueprint の作成、C++ ソースの編集とコンパイル、`console-run-command`、`unreal-tool-list`、`python-execute`、Fab ツールがこれにあたります。これらはエディターを操作し、いくつかは RCE クラス（例: `rql-mutate`、`console-run-command`、`python-execute`）なので、既定では出荷するゲームにコンパイルされません。パッケージ化したゲームにはエディターがないため、ランタイムの組み込み機能は意図的に `ping` + ゲームが登録したツールだけになっています。

## <a id="editor-only-exclude-from-packaged-games"></a>エディター専用 — パッケージ化したゲームから Unreal-MCP を除外する

Unreal-MCP を **Unreal Editor 内の AI ツール**としてだけ使い、**Unreal-MCP の痕跡がまったくない**パッケージ化ゲーム（ランタイムモジュールも、同梱の `.NET` サイドカーのバイナリもない）を出荷したい場合は、（プラグインの `.uplugin` ではなく）**利用側プロジェクト自身の `.uproject`** で **`TargetDenyList`** を使い、プラグインをエディターに限定します:

```jsonc
// <YourProject>.uproject — "Plugins" array
{
    "Name": "UnrealMCP",
    "Enabled": true,
    "TargetDenyList": [ "Game", "Client", "Server" ]
}
```

UE はプラグイン参照の `TargetDenyList` / `TargetAllowList` に従います（`PluginReferenceDescriptor::IsEnabledForTarget`）。プラグインは **Editor** ターゲットでは有効なままですが、**パッケージ化した `Game` / `Client` / `Server` ビルドからは除外されます**。そのため、ランタイムの基盤モジュールも同梱の `unreal-mcp-bridge` サイドカーも、出荷する製品にステージ・コンパイルされません。エディター用のモジュール（`UnrealMcpEditor`）はいずれにせよゲームビルドから取り除かれます。拒否リストはさらに `UnrealMcpRuntime` 基盤モジュールとそのサイドカーの `RuntimeDependencies`（§12.5）も外すので、**コンパイル後の痕跡はゼロ**になります。

> **注意 — モジュールへの直接の依存を作らないこと。** `TargetDenyList` が除外するのはプラグイン*自身の*モジュールだけです。あなたの**ゲーム**モジュールが `*.Build.cs` の `PublicDependencyModuleNames` / `PrivateDependencyModuleNames` に `UnrealMcpRuntime`（または任意の `UnrealMcp*` モジュール）を加えたり、`UnrealMcp*` のヘッダーを `#include` したりすると、その直接の依存が**拒否リストより優先され**、UBT はランタイムモジュールをゲームにコンパイルしてしまいます。純粋なエディター専用プロジェクトはプラグインの何も参照しないので、構造上クリーンなままです。モジュールへの依存は、本当に[ランタイム（ゲーム内）で使う](#ランタイムでの使用ゲーム内)場合にだけ追加してください。

逆に**ランタイム（ゲーム内）**で使う場合は、拒否リストを**使いません**。`UnrealMcpRuntime` 基盤モジュール + 組み込みの `ping` がパッケージ化ビルドに含まれ（§12.5）、`IUnrealMcpToolProvider` で独自のゲームプレイ用ツールを登録します。[3つの接続方法](#3つの接続方法)と [EXTENSIONS.md](EXTENSIONS.md) を参照してください。2 つは両立しません: エディター専用プロジェクトには拒否リストを使い、ライブの MCP 接続を持つゲームには使いません。

## <a id="runtime-security-contract"></a>セキュリティ契約

ランタイム接続は**実行中のゲームのリモート操作**です（アクターの生成、任意のリフレクションプロパティの書き込み、`console-run-command` による任意の CVar、`UFunction` の呼び出し）。出荷した製品から到達できれば RCE クラスになります。そのためランタイムの機能は、**5 層の緩和策**で固められています（[`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §12.8）。すべて `Connect()` の中で強制されます:

1. **オプトインのみ。** サブシステムは自動でインスタンス化されますが、**自動で接続することはありません**。読み込み時の自動接続も、設定アセットによる自動接続もありません。接続が起きるのは、明示的な `Connect()` 呼び出しによってだけです。
2. **キルスイッチ、既定はオフ。** `UUnrealMcpRuntimeSettings::bRuntimeMcpEnabled`（Project Settings → Plugins → **Unreal MCP (Runtime)**）の既定値は **`false`** です。オフの間、`Connect()` はすべて拒否され、サイドカーも起動しません。これは **Game** の設定項目（`DefaultGame.ini`、セクション `[/Script/UnrealMcpRuntime.UnrealMcpRuntimeSettings]`）なので、ゲートを効かせる必要があるパッケージ化ビルドにも引き継がれます。オンにしても自動では接続**しません**。明示的な `Connect()` がやはり必要です。
3. **Shipping ゲート。** `*.Build.cs` の `bUnrealMcpAllowShipping` フラグの既定値は **`false`**（→ `UNREAL_MCP_ALLOW_SHIPPING=0`）です。**Shipping** ビルドでは、このフラグを意図的にコンパイルに含めない限り、`Connect()` はログを出して `false` を返します。Development / PIE ビルドには影響しません。
4. **既定はループバックホスト。** 呼び出し側が明示的に `bAllowRemoteHost = true` を渡さない限り、`Connect()` はループバック以外の `Host` を拒否します。既定では接続は `localhost` / `127.0.0.0/8` / `::1` にとどまります。
5. **ループバック IPC + 使い捨ての stdin トークン。** プラグイン ⇄ サイドカーの IPC はループバックのみで、使い捨てのトークンで認証されます。トークンは **argv ではなく stdin** で渡され、ログにも記録されません（エディターと同じ方式、ARCHITECTURE §1.4）。

さらに、ランタイムの経路は**デスクトップ専用**（Win64 / Mac / Linux）です。コンソールやモバイルのプラットフォームでは .NET の `unreal-mcp-bridge` サイドカープロセスを起動できないため、それらのプラットフォームにはランタイム MCP の機能が一切ありません。

> **エンドツーエンドの手順書。** キルスイッチの有効化、PIE とパッケージ化した Development ビルドからの接続、ライブ接続でのツールの実行まで、運用手順の全体は [`docs/RUNTIME-E2E.md`](RUNTIME-E2E.md) にあります。決定的な部分（セキュリティゲート、ワールドリゾルバーの切り替え、接続/切断/孤立時の安全性）は、ヘッドレスの Automation スペックで固定されています。
