<div align="center">

<img src="img/readme/hero.svg" alt="Unreal MCP — Claude、Cursor、Copilot など任意の MCP エージェントが Unreal Editor を調べて操作できます" width="100%"/>

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

**Unreal MCP**（AI Game Developer for Unreal）は、MCP に対応した任意の AI エージェントを **Unreal Editor** につなぎます。普段の言葉で頼むだけで、エージェントがアクターを配置し、レベルを編集し、Blueprint を作成・コンパイルし、C++ を編集・コンパイルし、アセットを管理し、スクリーンショットで結果を確認します。[Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) と [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) の Unreal 版の兄弟プロジェクトで、同じクラウドバックエンド（[ai-game.dev](https://ai-game.dev)）か、自分で用意した [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) を使います。

<p align="center">
  <img src="img/readme/architecture.svg" alt="AI エージェントが MCP でサーバーと通信し、サーバーが SignalR で unreal-mcp-bridge サイドカーに中継し、サイドカーがループバック TCP で Unreal Editor 内の UnrealMCP C++ プラグインに接続する" width="100%"/>
</p>

Unreal のエディターは C++ なので、.NET の MCP ホストは小さな**サイドカー**（`unreal-mcp-bridge`）としてその隣で動きます。サイドカーの起動、監視、再起動はプラグインが行います。設計の詳細: [docs/ARCHITECTURE.md](ARCHITECTURE.md)。

**ステータス：**[最新リリース](https://github.com/IvanMurzak/Unreal-MCP/releases/latest)（プラグイン、sidecar、CLI は同じバージョン番号）· Unreal Engine 5.5–5.8 · sidecar は Windows x64、macOS（Intel と Apple シリコン）、Linux x64 向けにビルド · 各リリースは Windows（UE 5.5–5.8）と macOS（UE 5.7、5.8）でテスト · [Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33) と [GitHub Releases](https://github.com/IvanMurzak/Unreal-MCP/releases) で入手できます。

## 特長

✅ **C++ の作成** — クラスの雛形作成、ソース編集、コンパイル。エラーはファイル、行、重大度付きで返ります\
✅ **Blueprints** — 作成、編集、コンパイル。エージェントが修正できる構造化エラー付き\
✅ **RQL データレイヤー** — アクター、アセット、レベルを 1 つの言語で照会・変更（[RQL](rql.md)）\
✅ **ビジュアルフィードバック** — ビューポート、ゲームビュー、カメラ、単独アクターのスクリーンショット\
✅ **Python** — `unreal` API をすべて使えるエディター Python を実行\
✅ **Fab** — Fab の検索、ライブラリの一覧、所有コンテンツの追加\
✅ **拡張機能** — GAS、Niagara、PCG、Landscape、Control Rig、Enhanced Input\
✅ **独自ツール** — 任意のプラグインからツール、プロンプト、リソースを追加（[ガイド](EXTENSIONS.md)）\
✅ **ランタイムモード** — オプトインで実行中のゲームを操作（[docs/RUNTIME.md](RUNTIME.ja.md)）\
✅ **クラウドまたはローカル** — すぐ使える `ai-game.dev`、またはセルフホストの `gamedev-mcp-server`

8 ファミリー、30 個の組み込みツール。全一覧は [docs/TOOLS.md](TOOLS.ja.md) にあります。

### 頼めること

| エージェントへの依頼 | 使うツール |
| --- | --- |
| 「C++ で `AHealthPickup` アクタークラスを追加し、コンパイルしてエラーを直して」 | `source-create-class`、`source-update`、`source-compile` |
| 「重なると開くドアの Blueprint を作り、コンパイルしてレベルに配置して」 | `blueprint-*` |
| 「このレベルの木を数えて、ポイントライトをすべて 1 m 上げて」 | `rql-query`、`rql-mutate` |
| 「プレイヤーカメラからスクリーンショットを撮ってライティングを確認して」 | `screenshot-*` |
| 「Fab で無料の岩パックを探してプロジェクトに追加して」 | `fab-search`、`fab-add` |
| 「選択したアセットを Python スクリプトで一括リネームして」 | `python-execute` |

**C++ は第一級のループです。**`source-compile` は Live Coding が有効ならそれを、そうでなければ UnrealBuildTool を使ってビルドし、すべての診断をファイル、行、重大度付きで返すので、エージェントはビルドログを読まずにエラーを直せます。変更にフルビルドとエディター再起動が必要な場合（新しいクラス、`UCLASS` / `UPROPERTY` / `UFUNCTION` 宣言の変更）はそう伝えます。ファイル操作は `<Project>/Source/` の中に限られます。ターミナルからは `unreal-mcp-cli compile` がエディターなしで `file:line:col` 形式の診断を出力します。

**RQL はエディター自身から学べます：**`rql-schema` がすべてのソース、型、メンバーを一覧し、`unreal-rql` スキルがレシピ付きで構文を教えます（`unreal-mcp-cli setup-skills` がエージェント用に書き出します）。

## 必要環境

- **Unreal Engine 5.5 – 5.8**（リリースパイプラインが各バージョンを検証）。
- **C++ プロジェクト**。プラグインはソースで配布され、Unreal があなたのプラットフォームの C++ ツールチェーンでプロジェクト向けにモジュールをコンパイルするためです。Blueprint のみのプロジェクトは 1 ステップで C++ プロジェクトになります：エディターで **Tools → New C++ Class…** から任意のクラスを作成します。新規プロジェクトは `unreal-mcp-cli create-project ./MyGame` で最小の C++ プロジェクトを生成できます。
- **.NET のインストールは不要。**MCP ホストは自己完結型の .NET プログラム（`unreal-mcp-bridge`）で、プラットフォームごとにプラグインに同梱され、プラグインが起動・再起動します。（Fab 版には実行ファイルが含まれず、**Get bridge** ボタンで bridge を一度だけインストールします。）.NET 9 SDK が必要なのは bridge をソースからビルドする場合だけです。
- **Node.js 22.14+**。`unreal-mcp-cli` にのみ必要です（npm パッケージで、22.14 が最低 Node バージョン）。
- **Windows または macOS**（各リリースを両方でテスト）。**Linux x64：**sidecar はビルド・配布されますが、Linux ではリリーステストを実行していません。

## クイックスタート

**1. プラグインをインストール**（いずれか）：

- **[Fab](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)**（ターミナル不要）：**AI Game Developer** をエンジンに追加し、**Edit → Plugins** で有効にしてプロジェクトを開きます。
- **2 つのコマンド**（Node.js 22.14+）：

```bash
npx unreal-mcp-cli install-plugin ./MyGame   # plugin + bundled bridge into MyGame/Plugins
npx unreal-mcp-cli open ./MyGame             # build if needed and open the editor
```

**2. エディターで接続：****Tools → AI Game Developer** を開きます。

- Fab でインストールした場合のみ：**Get bridge** を一度クリックします（Fab 版には実行ファイルがないため、これで `unreal-mcp-bridge` sidecar をインストールします）。
- **Cloud**（既定）：**Authorize** をクリックし、ブラウザーでサインインを承認します。**ローカル**：**Custom** に切り替え、**MCP server** の横の **Start** をクリックします。エディターがチェックサム検証済みの `gamedev-mcp-server` をダウンロードします（Fab 版ではこのサーバーを手動でインストールします。ダウンロード先はログに表示されます）。
- **AI agent** で使うエージェント（Claude Code、Cursor、Codex など）を選び、**Configure** をクリックします。

**3. エージェントに依頼：***「キューブを 3 つ一列に並べ、その上にポイントライトを置いて、スクリーンショットを撮って。」*

ローカルか Cloud か？どちらも完全にサポートされ、後から切り替えられます。**既定は Cloud** です。ポートを開ける必要がなく、他のプログラムとのポート競合も起きないためで、接続は無料です。**ローカル**はアカウントもインターネット接続も不要で、オフラインやエアギャップ環境のマシンに向いています。

| | **ローカル** | **Cloud** |
| --- | --- | --- |
| アカウント | 不要 | ai-game.dev へのサインイン |
| MCP サーバー | 自分のマシン上の `gamedev-mcp-server`（エディターまたはエージェントが起動） | ai-game.dev リレー |
| 通信 | `localhost` 内にとどまる | ai-game.dev を経由（[記録される内容](#what-the-cloud-relay-sees)） |
| 認証 | ループバックでは不要、または共有トークン（フラグ 1 つ） | OAuth またはプロジェクトごとのキー |
| 向いている用途 | オフライン・エアギャップ環境、CI、非公開プロジェクト | ポート開放やポート競合なし。1 つのアカウントで複数のプロジェクトとマシン |

<details>
<summary><b>すべてターミナルから</b>（CLI：Cloud、ローカル、トークン認証）</summary>

<p align="center">
  <img src="img/readme/terminal.svg" alt="ターミナル：npm install -g unreal-mcp-cli、install-plugin、login、setup-mcp claude-code、エディターを開く" width="100%"/>
</p>

**Cloud**（プラグインの既定モード）：

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

**ローカル**（アカウント不要）：

```bash
npm install -g unreal-mcp-cli
unreal-mcp-cli install-plugin ./MyGame
unreal-mcp-cli configure -p ./MyGame --mode Custom                 # plugin: local server
unreal-mcp-cli setup-mcp claude-code -p ./MyGame --transport stdio # agent starts it
unreal-mcp-cli open ./MyGame
```

`setup-mcp --transport stdio` はチェックサム検証済みの `gamedev-mcp-server` リリースをプロジェクトにダウンロードし、エージェントの接続先をそこに向けます。両側とも `localhost` 上の同じプロジェクト固有ポートを使います。

**トークン認証を有効にする（ローカル）：**`configure` の行にフラグを追加し、`setup-mcp` をもう一度実行します。以後、プラグインとサーバーの両方がシークレットを要求します。

```bash
unreal-mcp-cli configure -p ./MyGame --mode Custom --auth required --token <secret>
```

ローカルで既定が認証なしの理由：サーバーはループバックにのみバインドし、ローカル以外のブラウザーオリジンからのリクエストを拒否するため、到達できるのは自分のマシン上のプログラムだけです。そのマシンのすべてのプログラムやユーザーを信頼できない場合はトークンを使ってください。シークレットはプロジェクトの `.env`（`configure` が gitignore に追加）とエージェントの設定ファイルに書き込まれるので、そのファイルはバージョン管理に入れないでください。

</details>

<a id="what-the-cloud-relay-sees"></a>**クラウドリレーが見るもの。**Cloud モードではツール呼び出しが ai-game.dev を経由します。ai-game.dev はそれを中継するだけで、送信内容は保存しません。リクエストやレスポンスの内容は一切保存しないため、ソースコード、プロジェクトファイル、アセットも保存されません。保持するのはメタデータ（ツール名、所要時間、サイズ、IP アドレス、MCP イベント）だけで、期間は 90 日です。[プライバシーポリシー](https://ai-game.dev/legal/privacy)。

`install-plugin` は CLI のバージョンに一致する、署名済みのプラグインソースリリースをダウンロードします。

その他のインストール方法：

- **Fab 形式の zip**：[Fab のリスティング](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)に加えて、各 GitHub リリースには実行ファイルを含まない Fab 形式のビルド（`unreal-mcp-plugin-fab-<version>.zip`）も付いています。どちらの場合も、ウィンドウの **Get bridge** ボタンが sidecar の一度きりのインストールを案内します。
- **手動**：[`UnrealMCP/`](../UnrealMCP/) を `<YourProject>/Plugins/UnrealMCP/` にコピーします。ソースのチェックアウトには sidecar が同梱されていないため、`unreal-mcp-cli bootstrap-local` を実行するか `UNREAL_MCP_BRIDGE_PATH` を設定してください。

更新（`unreal-mcp-cli update`）、接続モード、環境変数、トラブルシューティング：[docs/CONFIGURATION.md](CONFIGURATION.ja.md)。

### エージェントの設定

`unreal-mcp-cli setup-mcp <agent> -p ./MyGame` が設定を書き込みます（`setup-mcp --list` で対応エージェントを一覧できます）。書き込まれる内容：

<details>
<summary><b>Claude Code</b>：<code>.mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp claude-code -p ./MyGame
```

```json
{
  "mcpServers": {
    "unreal-mcp": {
      "type": "http",
      "url": "https://ai-game.dev/mcp/p/<pin>",
      "headers": { "Authorization": "Bearer agd_pk_…" }
    }
  }
}
```

`<pin>` はこのプロジェクトのエディターへのルーティングに使われます。マシンがサインイン済みなら、ヘッダーはこのプロジェクトのキーです。`--oauth` は URL だけの設定を書き込み、エージェントが自分でサインインします。`--transport stdio` ではエントリーがローカルサーバーを実行します：`"command"` は `<project>/Intermediate/UnrealMCP/server/` 内の `gamedev-mcp-server`、`"args"` は `port=<port>`、`client-transport=stdio`、`authorization=none` を設定します。

</details>

<details>
<summary><b>Cursor</b>：<code>.cursor/mcp.json</code></summary>

```bash
unreal-mcp-cli setup-mcp cursor -p ./MyGame
```

Claude Code と同じ `mcpServers` → `unreal-mcp` エントリーです：URL 付きの `"type": "http"`、またはローカルサーバー用の `command` と `args` 付きの `"type": "stdio"`。

</details>

<details>
<summary><b>Codex</b>：<code>.codex/config.toml</code></summary>

```bash
unreal-mcp-cli setup-mcp codex -p ./MyGame
```

```toml
[mcp_servers.unreal-mcp]
enabled = true
url = "https://ai-game.dev/mcp/p/<pin>"
tool_timeout_sec = 300
startup_timeout_sec = 30
```

プロジェクトキーがある場合、このセクションには `Authorization` ヘッダーを含む `http_headers` も入ります。

</details>

## 使い方

ほとんどの場合、エージェントと話すだけです。CLI も同じツールを直接呼び出せます。エディターが起動していなければ、ヘッドレスのエディターを起動します:

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

すべてのコマンドとオプション: [cli/README.md](../cli/docs/README.ja.md) または `unreal-mcp-cli commands`。

## セキュリティ

- **プラグイン ↔ sidecar：**プラグインはループバックの TCP ポートで待ち受け、sidecar は stdin 経由で渡される使い捨てトークンで接続します（コマンドラインには出さず、ログにも残しません）。
- **ローカルサーバー：**`gamedev-mcp-server` は既定でループバックにバインドし、許可されていない `Origin` からのリクエストには `403` を返します（DNS リバインディング対策）。ループバックでは、トークン（`UNREAL_MCP_AUTH_OPTION`、`UNREAL_MCP_TOKEN`）を選ばない限り、プラグインは認証なしで起動します。サーバーのオプション：[ホスティングと認証](https://github.com/IvanMurzak/GameDev-MCP-Server/blob/main/docs/hosting.md)。
- **Cloud：**エージェントは OAuth でサインインするか、`setup-mcp --regenerate-key` で差し替えられるプロジェクトごとのキーを使います。リレーはリクエストやレスポンスの内容を保存せず、メタデータのみを 90 日間保持します（[詳細](#what-the-cloud-relay-sees)）。
- **ツールにできること：**`<Project>/Source/` 内のファイル編集、コンソールコマンドとエディター Python の実行、任意のリフレクションプロパティの変更。
- **ツールの制御：**
  - **ホワイトリスト**（強い制限）：`UNREAL_MCP_TOOLS` は列挙したツールだけを提供します。それ以外のツールは一覧にも出ず、呼び出すこともできません。
  - **非表示**（弱い制限）：**MCP Tools** ウィンドウの **Visible** チェックボックスはツールを `tools/list` から外しますが、名前で呼び出すクライアントは実行できます。
  - **範囲の限られた変更：**C++ ファイルツールは `<Project>/Source/` 内にとどまります。`rql-mutate` の呼び出しはすべて成功するか何も変えないかのどちらかで、取り消しは 1 ステップ、`dry_run` でプレビューでき、対象が 200 を超えると、呼び出しが `max_targets` を上げない限り（最大 1000）拒否されます。`python-execute` は `timeout`（既定 30 秒）後に停止し、すべてのツール呼び出しに時間制限があります（`UNREAL_MCP_TOOL_TIMEOUT`、既定 5 分）。
  - **AI 向けではないもの：**システムツール（`ping`、スキル生成、`editor-quit`）は `tools/list` に現れず、`editor-quit` は CLI セッションの nonce が一致しない限り拒否します。
  - 詳細：[docs/TOOLS.md](TOOLS.ja.md)。
- **ランタイムモードは既定でオフ：**ゲームが勝手に接続することはありません。`bRuntimeMcpEnabled` 設定（既定はオフ）と明示的な `Connect()` 呼び出しが必要で、リモートホストを許可しない限りループバックのホストしか受け付けず、組み込んでコンパイルしない限り Shipping ビルドでは拒否されます。`TargetDenyList` でパッケージ化したゲームからプラグインを外せます（[docs/RUNTIME.md](RUNTIME.ja.md)）。

## 拡張機能

拡張機能は RQL を拡張するオプションのプラグインで、独自のツールは追加しません。

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

| 拡張機能 | 追加されるもの | 必要なもの |
| --- | --- | --- |
| [AI&nbsp;ControlRig](../extensions/UnrealAIControlRig/README.md) | プロシージャルアニメーションリグと Anim Blueprint への接続 | `ControlRig`, `RigVM`, UE 5.7+ |
| [AI&nbsp;EnhancedInput](../extensions/UnrealAIEnhancedInput/README.md) | 入力アクション、マッピングコンテキスト、トリガー、モディファイア | `EnhancedInput`, UE 5.7+ |
| [AI&nbsp;GAS](../extensions/UnrealAIGAS/README.md) | Gameplay Effect、アビリティ Blueprint、アトリビュートセット | `GameplayAbilities`, `EnhancedInput`, `GameplayTagsEditor`, UE 5.7+ |
| [AI&nbsp;Landscape](../extensions/UnrealAILandscape/README.md) | ランドスケープ、海、湖、川 | `Water`, UE 5.7+ |
| [AI&nbsp;Niagara](../extensions/UnrealAINiagara/README.md) | Niagara システム、エミッター、モジュール、コリジョン | `Niagara`, `ChaosNiagara`, UE 5.7+ |
| [AI&nbsp;PCG](../extensions/UnrealAIPCG/README.md) | PCG グラフ、ボリューム、生成された配置 | `PCG`, UE 5.7+ |

拡張機能はあなたのプロジェクト内でソースからコンパイルされます（次にエディターを開いたとき、または `--build` ですぐに）。インストーラーが必要なエンジンプラグインを有効にします。

## ドキュメント

| ページ | 内容 |
|---|---|
| [docs/TOOLS.md](TOOLS.ja.md) | すべての組み込みツールとシステムツール、拡張機能、ツールごとの表示設定 |
| [docs/rql.md](rql.md) | RQL クエリ言語: ソース、メンバー、変更操作、プレイテスト |
| [docs/CONFIGURATION.md](CONFIGURATION.ja.md) | 初回の起動、更新、環境変数、トラブルシューティング |
| [docs/EXTENSIONS.md](EXTENSIONS.md) | 独自の RQL モジュール、ツール、プロンプト、リソースを書く |
| [docs/RUNTIME.md](RUNTIME.ja.md) | ランタイム（ゲーム内）モード、そのセキュリティゲート、出荷するゲームからプラグインを除外する方法 |
| [cli/README.md](../cli/docs/README.ja.md) | `unreal-mcp-cli` のコマンドリファレンスと終了コード |
| [docs/ARCHITECTURE.md](ARCHITECTURE.md) | 正式な設計（IPC、登録、サイドカー、UI、設定） |
| [docs/RELEASING.md](RELEASING.md) | メンテナー向け: リリース、CI ランナー、シークレット |
| [docs/EXTENSION-MONOREPO.md](EXTENSION-MONOREPO.md) | メンテナー向け: 拡張機能のテストとリリース |

## リポジトリ構成

| パス | 内容 |
|---|---|
| [`UnrealMCP/`](../UnrealMCP/) | Unreal プラグイン（C++）: `UnrealMcpRuntime`、`UnrealMcpRql`、`UnrealMcpEditor` |
| [`bridge/`](../bridge/) | `unreal-mcp-bridge`、.NET 9 のサイドカー |
| [`cli/`](../cli/) | `unreal-mcp-cli`、npm パッケージ |
| [`extensions/`](../extensions/) | 個別にリリースされる 6 つのオプションのエンジン拡張機能 |
| [`samples/`](../samples/) | エディター拡張機能のテンプレートと、ランタイム（ゲーム内）ツールのサンプル |

## リンク

- [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G) · [Issues](https://github.com/IvanMurzak/Unreal-MCP/issues)
- 兄弟プロジェクト: [Unity-MCP](https://github.com/IvanMurzak/Unity-MCP) · [Godot-MCP](https://github.com/IvanMurzak/Godot-MCP)
- 共有コンポーネント: [GameDev-MCP-Server](https://github.com/IvanMurzak/GameDev-MCP-Server) · [MCP-Plugin-dotnet](https://github.com/IvanMurzak/MCP-Plugin-dotnet) · [ReflectorNet](https://github.com/IvanMurzak/ReflectorNet)
- [Model Context Protocol](https://modelcontextprotocol.io/)

## ライセンス

**ゲーム開発者は無料で使えます。商用ゲームも含みます。**Unreal MCP は [Apache-2.0](../LICENSE) © Ivan Murzak です。同梱の 2 つのコンポーネント RQL と Code-Exec は [PolyForm Shield 1.0.0](https://polyformproject.org/licenses/shield/1.0.0) ライセンスで、これが制限するのはゲーム開発向けの競合 AI ツールを作る企業だけです。ゲームの制作、出荷、販売にはすべて自由に使えます。通知：[`UnrealMCP/ThirdPartyNotices.txt`](../UnrealMCP/ThirdPartyNotices.txt)。
