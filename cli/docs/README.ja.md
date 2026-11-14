<div align="center">

<img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/readme/hero.svg" alt="Unreal MCP — 任意の AI エージェントが Unreal Editor を調べて操作できます" width="100%"/>

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

**`unreal-mcp-cli`** は [Unreal-MCP](../../docs/README.ja.md) のコマンドラインツールで、[`unity-mcp-cli`](https://www.npmjs.com/package/unity-mcp-cli) と [`godot-cli`](https://www.npmjs.com/package/godot-cli) の Unreal 版の兄弟ツールです。UnrealMCP プラグインのインストール、[ai-game.dev](https://ai-game.dev) へのサインイン、AI エージェントの設定、正しい接続設定での Unreal Editor の起動、そしてターミナルからのプラグインのツール呼び出しを行います。

<p align="center">
  <img src="https://raw.githubusercontent.com/IvanMurzak/Unreal-MCP/docs/readme-refresh/docs/img/readme/terminal.svg" alt="ターミナル: npm install -g unreal-mcp-cli、install-plugin、login、setup-mcp claude-code、エディターを開く" width="100%"/>
</p>

## 特長

✅ **プラグインのインストール** — UnrealMCP プラグインをプロジェクトの `Plugins/` にコピー（または junction）\
✅ **拡張機能** — オプションのエンジン拡張機能を追加（GAS、Niagara、PCG など）\
✅ **サインイン** — OAuth デバイスコードで `ai-game.dev` にログイン。マシンごとに 1 回\
✅ **エージェントの設定** — Claude Code、Cursor、Copilot、Codex などの MCP 設定\
✅ **起動と終了** — エディターをウィンドウまたはヘッドレスで起動。終了は CLI が起動したものだけ\
✅ **ツールの呼び出し** — `rql`、`exec`、`run-tool`。必要に応じてヘッドレスのエディターを起動\
✅ **ビルドとテスト** — エディターなしで UBT による C++ コンパイルと Automation テスト\
✅ **エージェント向け** — `--format json`、安定した終了コード、`commands` マニフェスト\
✅ **クロスプラットフォーム** — Windows、macOS、Linux

## クイックスタート

[Node.js](https://nodejs.org/) 22.14 以降が必要です。

```bash
npm install -g unreal-mcp-cli                       # or: npx unreal-mcp-cli@latest <command>

unreal-mcp-cli install-plugin ./MyGame              # plugin + bundled bridge into MyGame/Plugins
unreal-mcp-cli login                                # sign in to ai-game.dev once per machine
unreal-mcp-cli setup-mcp claude-code -p ./MyGame    # write the agent's MCP config
unreal-mcp-cli open ./MyGame                        # build if needed and open the editor
```

`install-plugin` と `update` は、ローカルに `UnrealMCP/` のチェックアウトがあればそれを使います。なければ、CLI のバージョンに合った GitHub Release のプラグインソースアセットをダウンロードし、展開する前に署名（minisign / Ed25519）を検証します。`--plugin-source <dir>` はオフライン / CI / 開発用の上書き指定です。

## 使い方

```bash
unreal-mcp-cli rql -p ./MyGame 'actors | where Label starts-with "Tree" | count'
unreal-mcp-cli exec -p ./MyGame "unreal.SystemLibrary.get_engine_version()"
unreal-mcp-cli compile ./MyGame --strict
unreal-mcp-cli test ./MyGame --junit-output junit.xml
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas ./MyGame
```

`rql`、`exec`、`run-tool` は起動中のエディターを再利用するか、ヘッドレスのエディターを起動します。`compile` と `test` はエディターなしで動きます。

## コマンド

| 分類 | コマンド |
| --- | --- |
| セットアップ | `create-project` · `install-engine` · `install-plugin` · `bootstrap-local` |
| パッケージ | `install-extension` · `update` · `remove-plugin` |
| アカウント | `login` |
| AI エージェント | `setup-mcp` · `setup-skills` · `configure` |
| エディター | `open` · `close` · `wait-for-ready` · `status` · `dialog` |
| ツール | `tools` · `rql` · `run-tool` · `run-system-tool` · `exec` |
| ビルド、テスト | `compile` · `test` |
| 探索 | `commands` |

すべてのオプション、終了コード、JSON 出力: **[コマンドリファレンス](https://github.com/IvanMurzak/Unreal-MCP/blob/docs/readme-refresh/cli/docs/commands.md)**（英語）、または `unreal-mcp-cli <command> --help`。

## ライブラリ

コマンドは、型付きで副作用のないライブラリとしても使えます。argv の解析も、stdout への出力も、プロセスの終了もありません。結果はすべて `kind: 'success' | 'failure'` です。

```ts
import { configure, openProject, runTool, detectInstalledEngines } from 'unreal-mcp-cli';
```

## リンク

[Unreal-MCP](../../docs/README.ja.md) · [ツール](../../docs/TOOLS.ja.md) · [設定](../../docs/CONFIGURATION.ja.md) · [ai-game.dev](https://ai-game.dev) · [Discord](https://discord.gg/cfbdMZX99G)

## ライセンス

[Apache-2.0](https://github.com/IvanMurzak/Unreal-MCP/blob/main/LICENSE)
