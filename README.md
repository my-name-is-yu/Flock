# Flock v0

Flock はローカルで複数の Bot を動かす C++20 Runtime です。Bot は依頼を担当し、実作業を T3 の Session に委ねます。CLI と Web UI は同じ SQLite を使います。

## ビルド

C++20 コンパイラ、CMake 3.24 以降、SQLite、libcurl、OpenSSL の開発ライブラリが必要です。実モデルの接続には Node.js 22 以降、接続のテストには Python 3 を使います。

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Bot を設定する

状態ディレクトリの既定は `~/.config/flock` です。環境変数 `FLOCK_HOME` または CLI の `--home` で変更できます。その下の `bots/release/config.json` に Bot を設定します。

```json
{
  "name": "Release",
  "modelProvider": "chatgpt",
  "model": "gpt-6.2-sol",
  "environmentId": "<T3 の環境 ID>",
  "projectId": "<T3 のプロジェクト ID>",
  "allowedBots": ["verifier"],
  "runTimeoutSeconds": 1800,
  "session": {
    "runtimeMode": "approval-required"
  },
  "cron": [
    {
      "id": "release-notes",
      "schedule": "0 10 * * 1",
      "timezone": "Asia/Tokyo",
      "request": "次のリリースに向けた未決事項をまとめてください。"
    }
  ]
}
```

同じディレクトリの `SOUL.md` に役割と判断基準、`MEMORY.md` にタスクをまたぐ前提を書きます。Session のモデルとプロバイダを指定する場合は、`session.model` と `session.providerInstanceId` を設定します。作業場所の指定には `session.workspaceStrategy` を使います。

`allowedBots` には依頼できる Bot のディレクトリ名を指定します。この例では、`bots/verifier/` に Verifier を設定すると Release から依頼できます。Verifier は同じやり取りに質問や結果を返せます。Verifier から Release に新しく依頼する場合は、Verifier 側の設定にも `release` を追加します。

## 実モデルと T3 に接続する

```sh
build/app/flock model login
build/app/flock model models
build/app/flock t3 login <environment-id> <MCP-URL>
build/app/flock run
```

それぞれの OAuth をブラウザで承認します。ChatGPT では Flock にプランの利用を許可します。T3 では Flock の Session 設定をカバーする権限を選びます。CLI が表示したローカル URL から Web UI を開けます。会話画面を閉じても、起動中の Runtime は cron と Session を監視します。

認証なしで試すときはテスト用の状態ディレクトリに Bot を設定し、`run --fixed` を使います。2体の Bot に異なる `projectId` を設定すると独立して進みます。`ask:対象ブランチは？` と依頼すると、最初の委譲前に確認待ちになります。

```sh
build/app/flock --help
build/app/flock tasks
build/app/flock status <task-id>
build/app/flock history <task-id>
build/app/flock answer <task-id> <wait-id> 'main'
```

実モデルが異なる完了条件を使い分けるかは、同意した ChatGPT 接続で `build/app/flock model acceptance <model-slug>` を実行すると確認できます。各試行の判断と理由を JSON Lines に出力します。

通常は Session の最終報告で判断します。作業記録を根拠にする比較では `run --session-read on_demand` で Bot に履歴参照を許可できます。1イベントにつき3回、各回8 KiBまでの抜粋をSQLiteに保存します。
