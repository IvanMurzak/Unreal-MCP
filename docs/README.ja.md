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

## 特長

✅ **RQL データレイヤー** — ひとつの言語でアクター、アセット、レベルを検索・変更（[RQL](rql.md)）\
✅ **Blueprint** — 作成、編集、コンパイル。エージェントが修正できる構造化エラー付き\
✅ **C++** — プロジェクトのソースを雛形作成、編集、コンパイル（Live Coding または UBT）\
✅ **視覚フィードバック** — ビューポート、ゲームビュー、カメラ、単独アクターのスクリーンショット\
✅ **Python** — 完全な `unreal` API でエディターの Python を実行\
✅ **Fab** — Fab を検索、ライブラリを一覧表示、所有コンテンツを追加\
✅ **拡張機能** — GAS、Niagara、PCG、Landscape、Control Rig、Enhanced Input\
✅ **独自ツール** — 任意のプラグインからツール、プロンプト、リソースを追加（[ガイド](EXTENSIONS.md)）\
✅ **ランタイムモード** — オプトインで実行中のゲームを操作（[docs/RUNTIME.md](RUNTIME.ja.md)）\
✅ **クラウドまたはローカル** — すぐ使える `ai-game.dev`、またはセルフホストの `gamedev-mcp-server`

8 つのファミリーに 30 個の組み込みツールがあります。全一覧は [docs/TOOLS.md](TOOLS.ja.md) にあります。

## 必要環境

- **Unreal Engine 5.5 – 5.8**（リリースパイプラインがそれぞれを検証します）と **C++ プロジェクト**: プラグインはあなたのプロジェクト内でエディターモジュールをコンパイルします。
- `unreal-mcp-cli` には **Node.js 22.14+** が必要です。
- Windows、macOS、Linux。.NET のインストールは不要です。サイドカーはプラグインに同梱されています（.NET 9 SDK はソースから bridge をビルドする場合のみ必要）。

## クイックスタート

<p align="center">
  <img src="img/readme/terminal.svg" alt="ターミナル: npm install -g unreal-mcp-cli、install-plugin、login、setup-mcp claude-code、エディターを開く" width="100%"/>
</p>

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # copy the plugin (with its bridge) into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` は CLI のバージョンに合った、署名付きのプラグインソースリリースをダウンロードします。エディターで **Tools → AI Game Developer** を開くと、接続、bridge の状態、接続中のエージェントを確認できます。あとはエージェントに頼んでみてください:

> *「キューブを 3 つ一列に並べて、その上にポイントライトを置いて、スクリーンショットを撮って。」*

その他のインストール方法:

- **Fab / Epic Marketplace**: [Fab の AI Game Developer](https://www.fab.com/listings/d1d5cbef-0aaa-4516-a309-92f39dee0c33)。Fab 版には実行ファイルを同梱できないため、ウィンドウの **Get bridge**（bridge を取得）ボタンが、サイドカーの初回インストールを案内します。
- **手動**: [`UnrealMCP/`](../UnrealMCP/) を `<YourProject>/Plugins/UnrealMCP/` にコピーします。ソースのチェックアウトにはサイドカーが同梱されていません。`unreal-mcp-cli bootstrap-local` を実行するか、`UNREAL_MCP_BRIDGE_PATH` を設定してください。

更新（`unreal-mcp-cli update`）、接続モード、環境変数、トラブルシューティング: [docs/CONFIGURATION.md](CONFIGURATION.ja.md)。

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

[Apache-2.0](../LICENSE) © Ivan Murzak
