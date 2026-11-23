# 長い会話のキャッシュ読み取り率73%を測る

前回の変更を比較元 A とした。比較元のソースは `6c7935a62f547b6a4192e5fe4b985f43e73d3e12` で、モデル実装は前回計測した `1a787d5bc6882ffc64da31e81171f18838e0ad7e` と同じ。変更版 B の計測対象は `18bbd95950e1fbe4c175d9dae37394a7c945d916`。 [前回の実測](model_context_cache.md)

## 計測条件

- Flock の ChatGPT Direct OAuth、`gpt-6.2-sol`、同じユーザー依頼と固定ツールデータを使用する。各ケースを新しい一時データベースから開始する。
- `near-limit` は1ターンで22ページを順に取り、両端のチケットを伝える。モデル呼び出しは23回。
- `multi-turn` は5ターンで35ページを取り、前半の根拠と更新された指示を使う。Compaction のあとに古い返信と省略したツール結果を復元する。モデル呼び出しは43回。
- 主指標は2ケース合算の読み取りトークンを入力トークンで割った値。未読入力は入力トークンから読み取りトークンを引く。呼び出し数、再取得数、正答、所要時間、HTTP 本文のバイト数も確認する。

## 認証済みの結果

2026年11月24日（JST）に、2ケースずつ B1 → A → B2 の順で実行した。プロバイダーのキャッシュ状態はそのまま使った。

| 実行 | 入力トークン | キャッシュ読み取り | 読み取り率 | 未読入力 | 呼び出し | 再取得 | HTTP 本文 B | 所要秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| A | 687,115 | 432,640 | 62.96% | 254,475 | 66 | 2 | 4,055,679 | 217.2 |
| B1 | 795,529 | 605,440 | 76.11% | 190,089 | 66 | 2 | 4,643,181 | 217.8 |
| B2 | 795,919 | 582,272 | 73.16% | 213,647 | 66 | 2 | 4,645,431 | 223.3 |

合算の未読入力は A から B1 で25.3%、B2 で16.0%減った。HTTP 本文は両実行で約14.5%増えた。単一ターンの B1 は比較元より未読入力が2,661トークン多く、5ターンの B2 は68.28%だった。

### ケース別と各ターンの初回・後続

初回は各ターンに1回。後続は同じターン内の2回目以降。率は対象呼び出しの合算値。

| 実行と原記録 | ケース | 読み取り率 | 初回の率 | 後続の率 | 未読入力 | 呼び出し / 再取得 | 所要秒 | 成功 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| [A](model_context_73_runs/a-near-limit.jsonl) | `near-limit` | 68.73% | 70.7% | 68.7% | 77,697 | 23 / 0 | 78.0 | 1/1 |
| [A](model_context_73_runs/a-multi-turn.jsonl) | `multi-turn` | 59.70% | 52.1% | 60.4% | 176,778 | 43 / 2 | 139.2 | 5/5 |
| [B1](model_context_73_runs/b1-near-limit.jsonl) | `near-limit` | 73.31% | 0.0% | 75.0% | 80,358 | 23 / 0 | 73.7 | 1/1 |
| [B1](model_context_73_runs/b1-multi-turn.jsonl) | `multi-turn` | 77.81% | 51.8% | 79.9% | 109,731 | 43 / 2 | 144.1 | 5/5 |
| [B2](model_context_73_runs/b2-near-limit.jsonl) | `near-limit` | 81.17% | 70.3% | 81.4% | 56,678 | 23 / 0 | 65.8 | 1/1 |
| [B2](model_context_73_runs/b2-multi-turn.jsonl) | `multi-turn` | 68.28% | 64.8% | 68.6% | 156,969 | 43 / 2 | 157.5 | 5/5 |

### メッセージ境界が一致した呼び出し

`previousInputMessagesArePrefix` で分類した。各会話の最初の呼び出しは直前のメッセージがないので表から外した。

| 実行 | 直前の全メッセージが先頭に残った呼び出し | その読み取り率 | 先頭が変わった呼び出し | その読み取り率 |
| --- | ---: | ---: | ---: | ---: |
| B1 | 49 | 81.7% | 15 | 56.4% |
| B2 | 49 | 74.6% | 15 | 67.4% |

### 回帰確認

固定応答6件と `./scripts/verify.sh` の54件が成功した。最新の権限でツールの再生と再取得を制限するテスト、通信の一時失敗後に完了済みの処理を重ねず続行するテストを含む。変更版はプロバイダーを使った短い依頼、最新指示を使う後続ターン、古い根拠の復元にも成功した。古い根拠の単独ケースは3呼び出し・再取得2回で、前回の変更版の実測と同じだった。 [固定応答](model_context_73_runs/window-fixed.jsonl) · [認証済みの回帰](model_context_73_runs/window-live-regression.jsonl)

## 入力の変更

以前は新しい大きな結果を取得するたびに、古い全文が省略形へ変わる場合が多かった。以前の結果が書き換わった呼び出しに、変更前の最良実行でも単一ターンの未読入力の92%、5ターンの66%が集まっていた。

通常のテキストターンでは、現在の依頼と最新の状態に続け、ツール結果をステップごとのメッセージとして追加する。更新される時計は、Flock が生成した整数値を Responses のリクエスト単位の `instructions` に置く。大きな `web_fetch` 結果の全文は、コンパクト形式との差が合計40 KiBに収まる間、同じ順序で保持する。上限を越えたときは、それまでのまとまりを保存済み参照付きの省略形に切り替える。最初の根拠と最新ステップの結果は優先して保持する。ツール履歴の全体は768 KiBが上限。 [実際のメッセージを組み立てるコード](../app/model.cpp) · [保持範囲の決定](../src/conversation_projection.cpp)

権限と秘匿値は呼び出しごとに再確認し、その時点で許可された根拠から入力を再構築して全体を送る。元の資料を読み直すときも、保存済み参照から現在の権限で復元する。

## 再現

次のコマンドはそれぞれの SHA からバイナリを作る。2つのビルドでテストデータと通常の ChatGPT プロファイルを共有する。

```sh
base_sha=6c7935a62f547b6a4192e5fe4b985f43e73d3e12
changed_sha=18bbd95950e1fbe4c175d9dae37394a7c945d916
root=$(mktemp -d)
git worktree add --detach "$root/a" "$base_sha"
git worktree add --detach "$root/b" "$changed_sha"
for version in a b; do
    cmake -S "$root/$version" -B "$root/$version-build" -DBUILD_TESTING=ON
    cmake --build "$root/$version-build" --target flock_model_context --parallel 8
done

profile=${FLOCK_CONTEXT_PROFILE_DIR:-"$HOME/.config/flock/credentials/chatgpt"}
mkdir "$root/results"
for run in b1 a1 b2; do
    version=${run%?}
    for scenario in near-limit multi-turn; do
        "$root/$version-build/flock_model_context" --live "$profile" gpt-6.2-sol "$scenario" \
            > "$root/results/$run-$scenario.jsonl"
    done
done
python3 tests/model_context_summary.py --per-turn "$root/results/"*.jsonl
CMAKE_BUILD_PARALLEL_LEVEL=8 CTEST_PARALLEL_LEVEL=4 ./scripts/verify.sh
```

`previousInputMessagesArePrefix` は直前の `input` メッセージ列全体が、今回の先頭に残ったかを判定する。プロバイダー側の読み取りは `providerCachedInputTokens` で確認する。前回の基準版では、この追加項目以外の先頭一致とプロバイダー指標が記録される。
