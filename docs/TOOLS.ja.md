<b>[English](TOOLS.md) | [中文](TOOLS.zh-CN.md) | [日本語](TOOLS.ja.md) | [Español](TOOLS.es.md) | [فارسی](TOOLS.fa.md) | [हिन्दी](TOOLS.hi.md) | [العربية](TOOLS.ar.md)</b>

# Unreal-MCP のツール

**UnrealMCP** エディタープラグインが登録するツールの正式な一覧です。Unreal-MCP には、AI エージェントが呼び出せる **ファミリーごとに分かれた組み込みツール**と、エージェントからは呼び出せない **システムツール**（CLI とデスクトップアプリ用のホスト側の仕組み）があります。ツール id は kebab-case（`rql-query`、`blueprint-compile`）で、Unity-MCP や Godot-MCP と同じ規則です。[拡張機能](#拡張機能)や[独自のプラグイン](EXTENSIONS.md)でツールを追加できます。

| ファミリー | ツール |
| --- | --- |
| RQL データレイヤー | `rql-query`、`rql-mutate`、`rql-schema` |
| Blueprint | `blueprint-*`: create、get、add-component、remove-component、add-variable、modify-variable、set-default、add-function、add-event、compile、spawn |
| ソース / C++ | `source-*`: read、create-class、update、delete、list、compile |
| スクリーンショット | `screenshot-*`: viewport、game-view、camera、isolated |
| Fab | `fab-search`、`fab-library`、`fab-add` |
| コンソール | `console-run-command` |
| Python | `python-execute` |
| ツール一覧 | `unreal-tool-list` |
| **システム**（AI には表示されない） | `ping`、`unreal-skill-create`、`unreal-skill-generate`、`editor-quit` |

> **正式なソース。** 標準ツール: `UnrealMCP/Source/UnrealMcpEditor/Private/Tools/` の `UnrealMcp*Tools.cpp`（`UnrealMcpSkillTools.cpp` を除く）と、`UnrealMcpRql` の `UnrealMcpRqlTools.cpp`。システムツール: `UnrealMcpPingTool.cpp`、`UnrealMcpSkillTools.cpp`、`UnrealMcpEditorSession.cpp` と、サイドカーの `SkillGenerateTool.cs`。登録が変わったらこのページを更新してください。

標準ツールはすべて**エディター専用**です。ランタイム（ゲーム内）接続で使えるのは `ping` と、ゲームが登録したツールだけです: [RUNTIME.md](RUNTIME.ja.md) を参照してください。

## ファミリーの詳細

<details>
  <summary><b>RQL — データレイヤー</b></summary>

操作ごとにツールを用意する代わりに、ひとつのクエリ言語でエディター全体を扱います（[`docs/rql.md`](rql.md)）。アクター、コンポーネント、アセットを探し、メンバーを読み、絞り込み、集計し、1 回の呼び出しで変更します。以前の actor、object、asset、level、editor-state、selection、console-log、reflection の各ツールを置き換えます。

| ツール id | 内容 |
| --- | --- |
| `rql-query` | 動作中のエディターに対する読み取り専用の RQL クエリ（`actors | where $ is PointLight | select $ref Label Intensity`）。結果はコンパクトな表。`;` で区切った複数のクエリを 1 回の呼び出しで実行 |
| `rql-mutate` | `set` / `call` で終わる RQL クエリ（`... | set Location = "(0, 0, 100)"`、`editor | call SpawnActor("PointLight")`）。すべて成功するか何も変えないかのどちらかで、取り消しは 1 ステップ。`dry_run` でプレビュー。`;` で区切った複数のクエリはそれぞれ 1 つの変更 |
| `rql-schema` | ソース、関数、エディターのワールドにあるクラス。`type` でひとつの型のメンバーとメソッドを一覧表示 |

リフレクションされたプロパティ以外にも、RQL は DataTable の行（`dataTables`、`Rows["Sword"].Damage`）、テキストとしての Gameplay Tag（`gameplayTags`、`set Tags = ["Ability.Fire"]`）、ini に保存される Project Settings（`unreal.settings("Maps & Modes")`）、Blueprint グラフ（`EventGraph | call AddCallFunction("PrintString")`、ピンの接続、`call Compile()`）、Widget Blueprint のツリー（`call AddWidget("Button", "Ok", parent)`）、Level Sequence（バインディング、トラック、`call AddKey("Location.X", 30, 100)`）にも届きます。

`unreal-rql` スキル（`unreal-skill-generate` が書き出します）は、この言語の使い方と、インストール済みの拡張機能が追加するものを説明します。

</details>

<details>
  <summary><b>Blueprint ファミリー— Unreal の中心となる機能</b></summary>

| ツール id | 内容 |
| --- | --- |
| `blueprint-create` | 親 `UClass` のパスから新しい Blueprint クラスを作成 |
| `blueprint-get` | LLM が調べるためのグラフの要約（変数、コンポーネント、関数/イベント、親クラスの連鎖） |
| `blueprint-add-component` | Simple Construction Script でコンポーネントを追加 |
| `blueprint-remove-component` | SCS コンポーネントを削除 |
| `blueprint-add-variable` | 型付きのメンバー変数を追加 |
| `blueprint-modify-variable` | メンバー変数を変更 |
| `blueprint-set-default` | CDO（クラスのデフォルト）プロパティを編集 |
| `blueprint-add-function` | 関数のスタブを追加（エントリ/リザルトノードは接続済み） |
| `blueprint-add-event` | イベントのスタブを追加/バインド（BeginPlay、Tick、入力など） |
| `blueprint-compile` | Blueprint をコンパイルし、**構造化されたエラー/警告の一覧**を返す（AI のフィードバックループ） |
| `blueprint-spawn` | Blueprint を現在のレベルにインスタンス化 |

</details>

<details>
  <summary><b>コンソールファミリー</b></summary>

| ツール id | 内容 |
| --- | --- |
| `console-run-command` | コンソールコマンド / CVar を実行 |

ログ、PIE の状態、選択は RQL で扱います: `console | where Verbosity == "Error"`、`editor | set IsPlaying = true`、`editor | set Selection = [...]`。プレイテストは呼び出しの中でゲームをシミュレートします（決定的で、呼び出しの間にエディターが tick するかどうかに左右されません）: `editor | call StartPlay()`、`editor | call AdvancePlay(1.5, 0, "W, LeftShift")`（キーを 1.5 秒押し続け、ポーンの位置、速度、その呼び出しで移動した距離、移動モード、適用されたマッピングコンテキストを返します）、`editor | call StopPlay()`。Enhanced Input 拡張機能は `enhancedInput | call InjectInput(action, value, seconds)` を追加します（[docs/rql.md](rql.md#play-testing)）。

</details>

<details>
  <summary><b>ツール一覧ファミリー</b></summary>

| ツール id | 内容 |
| --- | --- |
| `unreal-tool-list` | すべてのツールを一覧表示。`tools/list` で非表示のものも含む（hidden と表示。名前で呼べば実行される）。正規表現の `filter`、`includeDescription`、`includeInputs` |

</details>

<details>
  <summary><b>Python ファミリー</b></summary>

| ツール id | 内容 |
| --- | --- |
| `python-execute` | エディターで Python を実行（完全な `unreal` API をインポート済み）。普通の文を書けば、最後の式が結果になり、`print` はログとして返り、エラーは `snippet.py` の行を指す。`include` で `<Project>/.ai-game-dev/snippets` のヘルパーファイル/フォルダーを追加。`timeout`（秒、既定 30）で暴走したスニペットを止める |

エンジンの PythonScriptPlugin（Unreal-MCP が有効にします。エディター専用で、パッケージ化したゲームには含まれません）と、`UnrealMCP/Content/Python` に同梱された [Code-Exec](https://github.com/IvanMurzak/Code-Exec) ランタイムを使います。Blueprint グラフの配線は Python には公開されていません。それには Blueprint ツールを使ってください。

</details>

<details>
  <summary><b>Fab ファミリー— Fab コンテンツの検索と追加</b></summary>

| ツール id | 内容 |
| --- | --- |
| `fab-search` | [Fab](https://www.fab.com) マーケットプレイスを検索（`query`、`listingType`、`freeOnly`、`sort`、ページング）。各リスティングの uid、タイトル、種類、価格、形式を返す |
| `fab-library` | サインイン中のユーザーが所有する Fab アセットと、それぞれのインポート先を一覧表示。`filter` でライブラリ全体を検索 |
| `fab-add` | ユーザーが所有するリスティングをダウンロードし、Fab パネルの *Add to Project* と同じようにインポート（Unreal Engine パック、`quality` 付きの Megascans、glTF/FBX）。コンテンツフォルダーを返し、プロジェクトにすでにあるものはスキップ |

これらはエンジン自身の Fab プラグイン（UE 5.5 以降は既定で有効）を動かし、その Epic サインインとインポート処理を使います。サインインしていない状態で最初に `fab-library` / `fab-add` を呼ぶと、ブラウザで Epic のサインインページが開きます（エディターのツールバーから Fab を開いても、記憶された Epic ログインでサインインします）。購入や、無料リスティングのライブラリへの追加は fab.com で行います。完成したプロジェクトとエンジンのコードプラグインは、プロジェクトに追加するのではなく Epic Games Launcher からインストールします。

</details>

<details>
  <summary><b>ソース / C++ ファミリー</b></summary>

| ツール id | 内容 |
| --- | --- |
| `source-read` | プロジェクトの C++ ソースファイルを読む（範囲指定） |
| `source-create-class` | 新しい C++ クラスの雛形を作成（テンプレートからヘッダー + cpp） |
| `source-update` | ソースファイルを編集 |
| `source-delete` | ソースファイルを削除 |
| `source-list` | モジュールのソースファイルを一覧表示 |
| `source-compile` | プロジェクトの C++ をコンパイル（Live Coding が有効ならそれを、そうでなければ UBT を使用）し、構造化されたエラーレポートを返す。新しいクラスや変更されたリフレクション宣言（`restartRequired`）では、再ビルドとエディターの再起動の方法を示す |

ファイル操作はすべて `<Project>/Source/` の中に制限されます。

</details>

<details>
  <summary><b>スクリーンショット / ビューポートキャプチャファミリー</b></summary>

| ツール id | 内容 |
| --- | --- |
| `screenshot-viewport` | アクティブなエディタービューポートをキャプチャ。`savePath` を指定するとプロジェクト内に PNG も書き出す |
| `screenshot-game-view` | PIE / ゲームビューをキャプチャ。`savePath` は上と同じ |
| `screenshot-camera` | 解決したカメラアクターから `USceneCaptureComponent2D` でレンダリング。`savePath` は上と同じ |
| `screenshot-isolated` | アクターを単独でレンダリング（一時的な SceneCapture2D + show-only リスト）。`savePath` は上と同じ |

キャプチャは base64 の **PNG を MCP の画像コンテンツとして**返すので、LLM はレンダリング結果を直接確認できます。キャプチャのたびに呼び出し時点の新しいフレームをレンダリングします。エディターのウィンドウが背面にあっても、最小化されていても同じです。返されるテキストには、そのフレームが新しいかどうか、エフェクトとアニメーションが tick していたかどうかが書かれています。AI エージェントが接続している間、プラグインはエディターを背面でも（最小化時も）フルレートで tick・レンダリングさせ続けます（"Use Less CPU when in Background" はそのセッションの間だけ上書きされ、保存された設定は変わりません）。サイズには上限があります（既定 1024、1 辺の上限 2048）。ピクセルのキャプチャには、レンダリングするエディターが必要です: ウィンドウモード、または `-RenderOffscreen` 付きのヘッドレス（`unreal-mcp-cli` の既定）。`-nullrhi` では何もレンダリングされないため、これらのツールは `-nullrhi` と対処法を示すエラーを返します。

</details>

<details>
  <summary><b>システムツール— AI には公開されない</b></summary>

これらは**システムツール**です。`unreal-mcp-cli` とデスクトップアプリが `POST /api/system-tools/<name>` で直接呼び出す、ホスト側の仕組みです。意図的に MCP の `tools/list` から外してあるので、AI セッションに現れることも（トークンを消費することも）ありません。Unity と Godot もシステムツールを同じように分けています。

| ツール id | 内容 |
| --- | --- |
| `ping` | 死活確認 — プラグイン ⇄ サイドカー ⇄ サーバーの経路を往復 |
| `unreal-skill-create` | プラグインのエディターモジュールに、新しい MCP ツールを C++ ファイルとして生成。新しいツールを呼び出す前に**エディターの再ビルドが必要**（Live Coding、またはプロジェクトの再ビルド）で、結果にもそう明記される。プラグインのソースインストールが必要 |
| `unreal-skill-generate` | 登録済みのツールから、すべての `SKILL.md` をプロジェクト相対の skills フォルダー（既定 `.claude/skills`）に再生成 |
| `editor-quit` | パスを持つ未保存のパッケージを保存し、CLI セッションが起動したエディターを終了（`unreal-mcp-cli close --owned-only`）。`nonce` がセッションのリースと一致しなければ拒否するので、あなたが開いたエディターを閉じることはない |

</details>

## 拡張機能

プロジェクトで下のエンジンシステムのいずれかを使うときは、その拡張機能をインストールするか、[独自に作成](EXTENSIONS.md)してください。各拡張機能は RQL モジュールです。コアの `rql-query` / `rql-mutate` / `rql-schema` ツールを拡張し、自分のガイドを `unreal-rql` スキルに追加するので、`tools/list` に別の MCP ツールは現れません。

```bash
unreal-mcp-cli install-extension com.ivanmurzak.unreal-ai-gas "<UEProject>"
```

拡張機能には Unreal Engine 5.7+、同じプロジェクト内の Unreal MCP 0.26.0+、C++ ツールチェーンが必要です。リリースはソースで配布され、あなたのプロジェクト向けにコンパイルされます（次にエディターを開いたとき、または `--build` で今すぐ）。インストール id は `com.ivanmurzak.unreal-ai-<id>` で、id は下の表のものです。インストールオプション（`--version`、`--build`、オフラインの `--source`）: [Installing an extension into a project](EXTENSIONS.md#installing-an-extension-into-a-project)。

| 拡張機能（id） | 説明 |
| --- | --- |
| **[AI&nbsp;Control&nbsp;Rig](../extensions/UnrealAIControlRig/README.md)**<br>`control-rig` | ボーン、コントロール、2 ボーン IK ソルバーを持つリグを作成。Control Rig の Blueprint とコンポーネントを検索 |
| **[AI&nbsp;Enhanced&nbsp;Input](../extensions/UnrealAIEnhancedInput/README.md)**<br>`enhanced-input` | Input Action と Mapping Context を検索、アクションとコンテキストを作成、キーを割り当て |
| **[AI GAS](../extensions/UnrealAIGAS/README.md)**<br>`gas` | C++ なしで Gameplay Ability System を作成: エフェクト（コスト、クールダウン、ダメージ、回復）、アビリティの Blueprint、Health / Mana / Stamina のアトリビュートセットを備えたアビリティシステムコンポーネント、入力バインディング、Play In Editor での確認 |
| **[AI Landscape](../extensions/UnrealAILandscape/README.md)**<br>`landscape` | Landscape と Water: ランドスケープの作成、スカルプト、ペイント。海、湖、川の作成。ランドスケープ、ウォーターボディ、ウォーターゾーンの検索 |
| **[AI Niagara](../extensions/UnrealAINiagara/README.md)**<br>`niagara` | Niagara VFX: システムとそのエミッターを検索、システムをレベルに配置 |
| **[AI PCG](../extensions/UnrealAIPCG/README.md)**<br>`pcg` | プロシージャルコンテンツ生成: PCG グラフ（ノード、接続、設定、パラメーター）を作成し、ボリュームに配置し、散布（森、岩、小物）を生成して数を読み取る |

## ツールごとの表示設定

すべてのツールには、**MCP Tools** ウィンドウに **Visible**（表示）チェックボックスがあります。これは独立した **MCP Tools** タブです（エディターの **Tools** メニューに登録されています）。ウィンドウには各ツールのタイトル、ファミリー、説明と、"N / M tools visible" という要約行が表示されます。**Visible** が決めるのは、ツールが **MCP の `tools/list` に載るかどうか**、つまり AI エージェントから見えるかどうかだけです。アクセス制御ではありません。非表示のツールも、クライアントが名前で呼べば（`tools/call`）実行されます。また、エージェントが見つけられるよう、`unreal-tool-list` は非表示のツールも（hidden と表示して）一覧に出します。

関係する設定は 2 つです（[ARCHITECTURE](ARCHITECTURE.md) §7/§8 を参照）:

- **`enabledTools` ホワイトリスト**（`UNREAL_MCP_TOOLS` で上書き可能）は、ツールが**そもそも提供されるかどうか**を決めます。空でなければ、載っているツールだけが提供され、それ以外のツールは一覧にも出ず、呼び出すこともできません（`Execute()` が拒否します）。空なら「フィルターなし」です。
- **`hiddenTools` リスト**（UI で切り替える Visible チェックボックス）は、提供中のツールを `tools/list` から隠します。

どちらもエディターのセッションをまたいで保存され、拡張機能のホットリロード後も残ります（再登録されたツールは保持された設定を引き継ぐので、再ビルドしても隠したツールが表示されることはありません）。古いバージョンのプラグインの設定ファイルもそのまま使えます。その `disabledTools` リストは `hiddenTools` として読み込まれ、次の保存で `hiddenTools` として書き込まれます。

接続設定はすべて、ひとつの **AI Game Developer** メインウィンドウの Connection セクションにあります（別の Settings タブや Project Settings のページはありません。Unity-MCP と同じです）。**MCP Prompts** と **MCP Resources** ウィンドウは接続済みですが、このリリースでは中身が空です。それぞれ控えめな空状態のメッセージを表示します（"N / M visible" の要約行は Tools ウィンドウだけのものです）。
