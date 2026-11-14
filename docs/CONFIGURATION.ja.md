<b>[English](CONFIGURATION.md) | [中文](CONFIGURATION.zh-CN.md) | [日本語](CONFIGURATION.ja.md) | [Español](CONFIGURATION.es.md) | [فارسی](CONFIGURATION.fa.md) | [हिन्दी](CONFIGURATION.hi.md) | [العربية](CONFIGURATION.ar.md)</b>

# 設定、更新、トラブルシューティング

エディターの接続方法、プラグインを最新に保つ方法、環境変数での調整、よくある問題の解決方法をまとめます。インストール手順は [README](README.ja.md) に、これらすべての背後にある設計は [ARCHITECTURE.md](ARCHITECTURE.md) にあります。

- [初回の起動](#初回の起動)
- [プラグインの更新](#プラグインの更新)
- [環境変数](#環境変数)
- [トラブルシューティング](#トラブルシューティング)

## 初回の起動

1. エディターの **Tools** メニューから **AI Game Developer** メインウィンドウを開きます（このタブは Tools メニューのカテゴリに登録されています）。
2. **接続モードを選びます:**
   - **Cloud**（既定）— [ai-game.dev](https://ai-game.dev) に接続します。**Authorize**（認可）をクリックすると OAuth の**デバイスコードフロー**が始まります。ウィンドウに確認用 URL と短いユーザーコードが表示されるので、URL を開いてコードを入力し、承認すると、エディターが認可を完了します。保存されたクラウドトークンを消すには **Revoke**（取り消し）を使います。既定が Cloud なのは、ポートを開ける必要がなく、他のプログラムとのポート競合も起きないためで、接続は無料です。リレーはリクエストやレスポンスの内容を保存せず、メタデータのみを 90 日間保持します（[README](README.ja.md#what-the-cloud-relay-sees)）。
   - **Custom** — 自分で動かしているローカルの `gamedev-mcp-server`（または互換サーバー）に接続します。アカウント不要で、オフラインやエアギャップ環境のマシンでも動きます。サーバーの URL を入力し、AI クライアントの接続先をそこに向けます。サーバーは自分で起動するか、エディターに起動と停止を任せます（`UNREAL_MCP_START_SERVER`、Custom モード + http のみ。[環境変数](#環境変数)を参照）。
3. **Connection** セクションには、状態を示す点、状態ラベル、Connect / Disconnect / Stop ボタンがあります。bridge の状態は `Running (restarts: N)` または `Stopped` と表示されます。これらでサイドカーが動いていることを確認してください。
4. AI クライアント（Claude Code、Cursor、AI Game Developer アプリなど）の接続先をサーバーに向けます。**AI agents** セクションには現在接続中のエージェントが一覧表示されます。MCP クライアントの設定を書き出すには `unreal-mcp-cli setup-mcp` を使います。

接続設定は `<Project>/Saved/Config/UnrealMcp/ai-game-developer-config.json` に保存されます（`Saved/` はどの UE テンプレートでも gitignore されているので、既定ではトークンが VCS に入ることはありません）。

> **固定された MCP クライアント URL。** `unreal-mcp-cli setup-mcp <agent>` が書き出す MCP クライアント設定は、**プロジェクトごとに固定された**クラウド URL `<base>/mcp/p/<pin>` を指します。そのため、アカウントで複数のプロジェクトを操作していても、エージェントは**この**プロジェクトのエディターにルーティングされます。代わりに固定なしの `<base>/mcp` URL を書き出すには `--no-pin` を渡します。pin はルーティング用のパスセグメントにすぎず、OAuth のリソースは `<base>/mcp` のままです。

> **プロジェクトキー。** このマシンがサインイン済み（`unreal-mcp-cli login`、またはエディターの Sign in）なら、Cloud の設定にはすべてのエージェント向けに `Authorization: Bearer agd_pk_…` も含まれます。これはこのプロジェクトの pin に結びついた、期限切れのない、取り消し可能なキーなので、どのエージェントも個別の OAuth を必要としません。キーは `~/.ai-game-dev/project-keys.json` にキャッシュされ、再利用されます。URL だけの設定（エージェントが自分でサインインする）には `--oauth` を、新しいキーを発行して古いキーを取り消すには `--regenerate-key` を渡します（エディターの AI エージェントパネルには **Regenerate key** ボタンがあります）。サインインしていない、またはサーバーがキーを発行できない場合 ⇒ URL だけの設定になります。

## プラグインの更新

その場で更新したときは、必ず**新しい**コードで動いている状態にならなければなりません。リスクは UE のインクリメンタルコンパイラーです。プラグインのソースが変わった（新しい `.cpp` ファイル、新しいモジュール）のに古い `UnrealMCP/Intermediate/` のビルドキャッシュが残っていると、UE は古いモジュールのファイル一覧をもとに部分的な再コンパイルを行い、気づかないうちに古い/不完全なコードのままになることがあります。チャネルごとに扱いが異なります:

- **Fab / Epic Marketplace→ 自動。** Epic Games Launcher がプリコンパイル済みのバイナリをその場で置き換えます。コンパイルも、キャッシュの削除も不要です。bridge はプラグインのバージョンごとに保持されるため、更新後は **Get bridge** をもう一度クリックしてください。
- **`unreal-mcp-cli update` → 自動でクリーンビルド。** `update` はプラグインのソースをコピーし直し、**既定で**インストール済みプラグインの古い `Intermediate/` と C++ の `Binaries/` を削除するので、次にエディターを起動したとき UE がクリーンにコンパイルします。手作業は不要です。`Binaries/ThirdParty/UnrealMcpBridge/<rid>/` にある同梱のサイドカー bridge はそのまま残ります。リリースソースからのインストールでは `Source/ThirdParty/...` から更新され、リポジトリ/開発用のインストールでは必要に応じて以前同梱したコピーが保持されます。開発用の **junction** インストールは決して削除されません（削除すると、作業中のソースツリーの出力が消えてしまうためです）。キャッシュの削除を行わないようにするには `--no-clean` を渡します。

  ```bash
  unreal-mcp-cli update <YourProject>            # default: clean rebuild on version change
  unreal-mcp-cli update <YourProject> --force    # re-copy even when versions match
  unreal-mcp-cli update <YourProject> --no-clean # keep the existing build cache
  unreal-mcp-cli update <YourProject> --no-extensions # leave the installed extensions as they are
  unreal-mcp-cli update <YourProject> --no-self  # leave the CLI as it is
  unreal-mcp-cli update <YourProject> --check    # only report; exit code 2 when something is outdated
  ```

- **手動コピー → キャッシュは自分で削除。** `<YourProject>/Plugins/UnrealMCP/` を手で上書きする場合は、**先にエディターを閉じ**、`<YourProject>/Plugins/UnrealMCP/Intermediate/` と C++ の `Binaries/` を削除してから（同梱の bridge がある場合は `Binaries/ThirdParty/` は残します）、再起動して UE にクリーンに再コンパイルさせます。

## 環境変数

プラグインは **プロセスの環境変数 → `<Project>/.env` → 設定ファイル → 組み込みの既定値** の優先順位で設定を読み込みます。認識される `UNREAL_MCP_*` 変数（`UnrealMCP/Source/UnrealMcpRuntime/Private/Config/UnrealMcpConfig.cpp` で定義。`UNREAL_MCP_CREDENTIALS_DIR` は bridge が、`UNREAL_MCP_SERVER_PATH` は CLI が読み込みます）:

| 変数 | 用途 |
| --- | --- |
| `UNREAL_MCP_CONNECTION_MODE` | `Cloud` または `Custom`。**既定は `Cloud`。** |
| `UNREAL_MCP_HOST` | サーバーの URL（Custom モード）。例: `http://localhost:<port>` |
| `UNREAL_MCP_CLOUD_URL` | クラウドバックエンドの URL（既定は ai-game.dev） |
| `UNREAL_MCP_TOKEN` | 認証トークン（サイドカーの IPC トークン / サーバートークン）。**シークレット。絶対にコミットしないこと。** |
| `UNREAL_MCP_AUTH_OPTION` | `none` または `oauth`（ローカルサーバーの認証） |
| `UNREAL_MCP_KEEP_CONNECTED` | 接続状態を保存/復元 |
| `UNREAL_MCP_TOOLS` | 有効なツールの上書き指定（ホワイトリスト。空 = フィルターなし） |
| `UNREAL_MCP_START_SERVER` | `true`: bridge がプロジェクトのポートを報告した時点で、エディター自身がローカルの `gamedev-mcp-server` を起動し（Custom モード + http のみ。`startServer` として保存）、終了時に停止します。`unreal-mcp-cli exec` / `open --headless` がこれを設定します。 |
| `UNREAL_MCP_TRANSPORT` | `stdio` または `http` |
| `UNREAL_MCP_LOG_LEVEL` | ログの詳細度 |
| `UNREAL_MCP_TOOL_TIMEOUT` | 1 回のツール呼び出しをエディターで実行できる時間（ミリ秒。既定 `300000`。`toolTimeoutMs` として保存）。長い `python-execute` スニペットやアセット処理では大きくしてください。エディターは、自分が起動するローカルサーバーにこの値に 10 秒を足したものを `tool-timeout` として渡します。古いサーバー（McpPlugin 8.11.0 より前）は、この値にかかわらずツール呼び出しを 5 分で打ち切ります（[ARCHITECTURE §4](ARCHITECTURE.md)）。エディターが開始する前にタイムアウトまたはキャンセルされた呼び出しはスキップされ、後から実行されることはありません。 |
| `UNREAL_MCP_BRIDGE_PATH` | サイドカーのバイナリへのパス。**開発/CI 用の上書き指定**で、同梱のバイナリより優先されます（§6）。パッケージ化されたリリースは同梱のサイドカーを自動で見つけるので、エンドユーザーが設定することはありません。ソースからのビルドで使います。 |
| `UNREAL_MCP_CREDENTIALS_DIR` | bridge が `~/.ai-game-dev` の代わりに読み込む（そして更新する）マシンの認証情報ストアの絶対パスのフォルダー。エディターのプロセス環境で設定します（`.env` からは読み込まれません）。値はパスであり、トークンではありません。相対パスはログに記録されて無視されます。ランチャーが別のプロファイルでサインインする開発/テスト用エディター（AI Game Dev App のローカル開発環境）向けです。Godot の `GODOT_MCP_CREDENTIALS_DIR` に相当します。 |
| `UNREAL_MCP_SERVER_PATH` | ローカルの `gamedev-mcp-server` バイナリへのパス（プラグインではなく `unreal-mcp-cli` が読み込みます）。サーバーのダウンロードとバージョン確認を省略します（§6）。 |
| `AIGD_SESSION_IDLE_SECONDS` | `unreal-mcp-cli` が自分の起動したエディターに設定します。この秒数のあいだツール呼び出しがないと（実行中の呼び出し、Play In Editor、アセットのコンパイル、ユーザー入力は活動とみなされます）、エディターはパスを持つ未保存のパッケージを保存して終了します。`AIGD_SESSION_OWNER=cli` と nonce が揃い、かつプロジェクトのリースがまだこのエディターを指している場合にだけ有効になります。未設定 = 終了しません。 |
| `AIGD_SESSION_OWNER` / `AIGD_SESSION_NONCE` | `cli` と、`<project>/.ai-game-dev/session.json` の nonce。そのエディターが CLI セッションによって起動されたことを示します。`editor-quit` を受け付けるのはそうしたエディターだけで、終了時にリースがまだ自分の nonce を持っていれば、リースを削除します。 |

> **`.env` は絶対にコミットしないこと。** プロジェクトのルートにある `.env` には `UNREAL_MCP_TOKEN` が入っていることがあり、UE のプロジェクトテンプレートには `.gitignore` が付いていません。`unreal-mcp-cli configure` は対象プロジェクトの `.gitignore` に `.env` を追記します。このリポジトリ自身の雛形では、すでに gitignore されています。サイドカーの IPC トークンは（argv ではなく）stdin で渡され、ログにも記録されません。

## トラブルシューティング

- **`No connected clients. Retrying [1..10]` のあと、ローカルサーバーから HTTP 500。** 接続モードが既定の `Cloud` になっていたため、サイドカーがローカルサーバーではなく ai-game.dev に接続しようとしました。`UNREAL_MCP_CONNECTION_MODE=Custom` を設定してください（環境変数、`.env`、または UI の切り替え）。
- **起動時に `[Unreal-MCP] plugin loaded` の行が出ない。** エディターモジュールの読み込みに失敗しています。Output Log で `StartupModule` のエラーや、不正な `.uplugin` がないか確認してください。
- **REST パススルーで呼び出すと、ツールが `'x' is required.` を返す。** `-H "Content-Type: application/json"` を付けて送ってください。これがないと、サーバーは JSON の本文を捨ててしまいます。
- **スクリーンショットツールが `Screenshot tools are unavailable: this editor runs with the null RHI (-nullrhi)` を返す。** `-nullrhi` では何もレンダリングされません。これを外してエディターを再起動してください: ウィンドウモード、またはスクリーンショットをレンダリングできる `-RenderOffscreen` 付きのヘッドレス（`unreal-mcp-cli` では `--nullrhi` を付けない）。
- **サイドカーがない / サイドカーが再起動を繰り返す。** **Connection** セクションで bridge の状態（`Running (restarts: N)` / `Stopped`）を確認してください。ログに `no sidecar binary resolved for rid <rid> …` と出ている場合は、同梱のバイナリ、ダウンロードしたバイナリ、`UNREAL_MCP_BRIDGE_PATH` のいずれも見つからなかったということです。Fab でインストールした場合は bridge を手動でインストールする必要があります: **Get bridge** がリリースページを開くので、自分のプラットフォーム用の zip を **Open bridge folder** が示すフォルダーに展開し、**Connect** をクリックします（Fab 版がプログラムを自分でダウンロードしたり起動したりすることはありません）。CLI / パッケージ化されたリリースには bridge が同梱されているので、そこで起きた場合は、プラグインがあなたのプラットフォームの bridge なしでパッケージ化されたことを意味します（正しいビルドを再インストールしてください）。ソースからのビルドでは、`UNREAL_MCP_BRIDGE_PATH` を設定するか、`unreal-mcp-cli bootstrap-local` を実行してソースからビルドしてください（[ARCHITECTURE](ARCHITECTURE.md) §6.3、§6.8）。
- **ポート。** IPC は `30000–39999` の範囲で、プロジェクトごとに決まったポートを使い、衝突したときは先へ順に試します（その後はエフェメラルポート）。ローカルサーバーは `20000–29999` の範囲でハッシュから決まるポートを使い、試行はしません。CLI は設定を読まずに同じ番号を求められ、サーバーは要求されたポートにそのままバインドします。具体的な番号はデバッグの助けになるだけで、必須ではありません。
- **ログ。** メインウィンドウの **Open log file** 操作、RQL クエリ（`console | where Verbosity == "Error"`）、またはエディターの Output Log（`LogUnrealMcp` カテゴリ）を使ってください。
