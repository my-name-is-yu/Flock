# Flock Bot の会話でキャッシュ再利用を測る

[PR #289](https://github.com/my-name-is-yu/Flock/pull/289) の head `f2f42eb053dc23ef235a55de8baa50ab69a07dbd` を基準版 A に固定した。変更版 B は `1a787d5bc6882ffc64da31e81171f18838e0ad7e`。両版を Flock の ChatGPT Direct OAuth と `gpt-6.2-sol` で実行した。

## ケースと判定

| ケース | 完了条件 |
| --- | --- |
| `near-limit` | 新規会話の1ターンで、順番に22ページを1回ずつ読み、最初と最後のチケットを答える。24モデルステップの上限内で完了する。 |
| `multi-turn` | 5ターンで35ページを1回ずつ読む。第1ターンの結果、第3ターンの書式変更、元の返信とツール結果を使って最終回答を作る。第5ターンで古い返信の省略を観測し、compaction 前後を計測する。 |
| `short` | 指示に従い `MARIGOLD` だけを返す。 |
| `long-followup` | 最新の対象範囲と古い開始コードを使い、未解決の質問を閉じずに回答する。 |
| `large-results` | 6ページを1回ずつ読む。固定応答では、省略された結果の参照を読み直し、保存された本文との一致も確かめる。 |
| `old-reference` | 以前の要約から書式を読み、`expand_context` で元のコードを取り出す。 |

## 認証済み比較

2026年11月24日（JST）に、各ケースで A → B と B → A の組を実行した。各実行で一時データベースから新しい会話を開始した。両版で同じモデル、Flock の認証、ユーザー指示、ツール応答を使った。単一ターンと5ターンは別の認証ロックで実行し、同じケースの実行は重ねていない。プロバイダー側のキャッシュはそのまま使った。

| ケース | 実行順 | 入力トークン A → B | 読み取りトークン A → B | 読み取り率 A → B | 未読入力 A → B | 呼び出し / 再取得 A・B | 所要秒 A → B | 成功 A・B |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 単一ターン | [a1](model_context_runs/a1-near-limit.jsonl) → [b1](model_context_runs/b1-near-limit.jsonl) | 248,541 → 248,449 | 85,248 → 124,160 | 34.3% → 50.0% | 163,293 → 124,289 | 23 / 0 | 139.9 → 72.0 | 1/1 |
| 単一ターン | [b2](model_context_runs/b2-near-limit.jsonl) → [a2](model_context_runs/a2-near-limit.jsonl) | 248,541 → 248,449 | 97,024 → 150,784 | 39.0% → 60.7% | 151,517 → 97,665 | 23 / 0 | 80.7 → 74.3 | 1/1 |
| 5ターン | [a1](model_context_runs/a1-multi-turn.jsonl) → [b1](model_context_runs/b1-multi-turn.jsonl) | 437,548 → 437,670 | 111,232 → 266,496 | 25.4% → 60.9% | 326,316 → 171,174 | 43 / 2 | 208.3 → 156.1 | 5/5 |
| 5ターン | [b3](model_context_runs/b3-multi-turn.jsonl) → [a3](model_context_runs/a3-multi-turn.jsonl) | 436,858 → 437,736 | 171,648 → 254,336 | 39.3% → 58.1% | 265,210 → 183,400 | 43 / 2 | 159.3 → 148.9 | 5/5 |

完了した4組は合計264呼び出しで、すべての組で読み取り率が上がった。未読入力の合計は 906,336 から 576,528 トークンに減った（36.4%減）。所要時間にはプロバイダー側の待ち時間を含む。最初の単一ターン試行は15回目で `server_is_overloaded` を受けて中断した。5ターンの最初の逆順試行でも基準版の第2ターンで同じエラーが出た。逆順の組をもう一度実行し、比較表には両版が依頼を完了した組を載せた。 [単一ターンの中断](model_context_runs/attempt-overload-near-limit.jsonl) · [5ターンの中断](model_context_runs/attempt-overload-a2-multi-turn.jsonl) · [最初の逆順で先に完了した変更版](model_context_runs/b2-multi-turn.jsonl)

HTTP の JSON 本文は、単一ターンの4実行でそれぞれ1,445,237 B。5ターンは1組目が2,604,352 → 2,606,923 B、逆順が2,601,232 → 2,605,282 Bだった。5ターンでは実モデルが生成した結果と要約の長さが実行ごとに異なる。

### 各ターンの初回と後続

各セルは読み取り率と未読入力トークン。初回は1呼び出し、後続はそのターン内で集計した。

| ケース | 回 | ターン | 初回 A → B | 後続 A → B |
| --- | ---: | ---: | ---: | ---: |
| 単一ターン | 1 | 1 | 70.7% / 1,966 → 70.7% / 1,962 | 33.3% / 161,327 → 49.4% / 122,327 |
| 単一ターン | 2 | 1 | 0.0% / 6,702 → 70.7% / 1,962 | 40.1% / 144,815 → 60.4% / 95,703 |
| 5ターン | 1 | 1 | 0.0% / 6,862 → 69.1% / 2,122 | 19.9% / 57,288 → 58.9% / 29,356 |
| 5ターン | 1 | 2 | 0.0% / 7,121 → 66.5% / 2,381 | 25.8% / 54,365 → 53.6% / 33,985 |
| 5ターン | 1 | 3 | 0.0% / 7,290 → 65.0% / 2,550 | 25.4% / 55,548 → 58.8% / 30,688 |
| 5ターン | 1 | 4 | 0.0% / 7,441 → 63.7% / 2,701 | 28.1% / 54,301 → 71.0% / 21,889 |
| 5ターン | 1 | 5 | 61.7% / 2,934 → 61.8% / 2,930 | 31.2% / 73,166 → 60.1% / 42,572 |
| 5ターン | 2 | 1 | 69.0% / 2,126 → 69.1% / 2,122 | 48.0% / 37,192 → 63.9% / 25,772 |
| 5ターン | 2 | 2 | 66.5% / 2,385 → 0.0% / 7,117 | 19.4% / 59,101 → 38.8% / 44,865 |
| 5ターン | 2 | 3 | 65.0% / 2,554 → 65.0% / 2,550 | 38.1% / 46,076 → 54.8% / 33,632 |
| 5ターン | 2 | 4 | 63.6% / 2,705 → 63.7% / 2,701 | 37.6% / 47,133 → 68.6% / 23,681 |
| 5ターン | 2 | 5 | 61.7% / 2,934 → 61.8% / 2,930 | 40.4% / 63,004 → 64.3% / 38,030 |

### 履歴とツール結果が書き換わったとき

5ターンの1回目を抜粋する。33回目に古い返信が抜粋になり、34回目は、その返信を含む範囲を要約に置き換えた直後の呼び出し。35回目は `expand_context` の結果が入り、元のチケットを取得できた。36回目以降は新しいページを読み、必要な省略済み結果を `read_tool_result` で復元した。

| 呼び出し | 基準版で最初に変わった項目 | 変更版で最初に変わった項目 | 一致する文脈のバイト数 A → B | キャッシュ読み取り A → B |
| ---: | --- | --- | ---: | ---: |
| 33 | `event.messageId` | `history` | 66 → 1,302 | 4,736 → 4,736 |
| 34 | `history` | `history` | 1,060 → 36 | 4,736 → 4,736 |
| 35 | `event.currentTime` | `event.turn` | 3,736 → 5,265 | 4,736 → 7,296 |

単一ターンの1回目は、4回目に古いツール結果が初めて1件省略され、3回目と一致する結果は先頭の1件だった。変更版の文脈は9,495 B一致し、4,736トークンを読み取った。5回目も古い結果を省略したが、先頭2件が一致し、文脈の一致は9,812 B、読み取りは8,064トークンになった。 [単一ターンの呼び出し記録](model_context_runs/b1-near-limit.jsonl)

固定応答の単一ターンでは、プロバイダーへ向けた `input` 配列の先頭一致の中央値が7,455 Bから19,596 Bに伸びた。5ターンでは10,317 Bから18,280 B。セッションの初回を中央値から除いた。ツール結果の最初の省略や履歴の compaction で一致が短くなる位置は、各実行の `toolResultsCommonEntries` と `historyCommonEntries` で確認できる。同じ版でも全ての呼び出しでヒットするわけではなく、読み取り数はプロバイダーの使用量に従う。

短い依頼、古い開始コードと最新指示の併用、要約済みの元の参照への復元は、認証済み実行でも両版で成功した。古い参照の復元は基準版が2呼び出し・再取得1回、変更版が3呼び出し・再取得2回だった。 [基準版](model_context_runs/a-live-regression.jsonl) · [変更版](model_context_runs/b-live-regression.jsonl)

## 入力が変わる位置

基準版の JSON はキー名順に直列化される。ターンの変わり目には、履歴より前の `event.messageId` が変わる。同じターンの後続呼び出しでは、ツール結果より前の `event.currentTime` が変わる。

変更版は SOUL、MEMORY、履歴を現在の依頼より先に置き、ツール結果を時刻・状態より先に置く。これで同じターンではツール結果の最初の変更箇所まで、ターン間では履歴の最初の変更箇所まで入力が一致する。履歴の省略、compaction、権限の再確認に伴う書き換えは引き続き適用する。

画像を含まない通常の会話ターンでは、会話スコープとインストール ID の SHA-256 を `session-id` として渡す。アダプターは同じ ChatGPT Direct OAuth の `/v1/responses` に `store:false` で全入力を送る。ツールの提示順と実行前の再認可は従来の処理を使う。

### 参照した実装

- **pi** は同じセッション・認証の WebSocket 接続を使い、前回と今回で入力以外の設定が一致することを確かめる。前回の入力と出力を連結した列が今回の入力の先頭に完全一致する場合、`previous_response_id` と追加項目を送る。設定や先頭が変わると全体を送り、継続 ID が見つからない場合も全体送信に戻る。Compaction は古いエントリを要約に置き換え、`firstKeptEntryId` 以降を残す。Flock では結果や権限の再投影で以前の入力が変わるため、同じ判定をせずに差分を送ると古い情報が残る。今回は pi の `session-id` を同じ既存認証経路で試した。 [入力一致と全体送信](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/ai/src/api/openai-codex-responses.ts#L1426-L1589) · [セッションヘッダー](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/ai/src/api/openai-codex-responses.ts#L1650-L1700) · [Compaction](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/coding-agent/docs/compaction.md#how-it-works)
- **dsh** はツール定義を既定では名前順にし、明示した枠があればその順序を使う。実行時情報が変わったときに末尾へ追記し、最新の情報を優先する。結果の省略はサイズの圧迫やオーバーフロー時に適用し、元の記録への参照を残す。省略した呼び出しの一致位置は最初に書き換えた結果で止まり、次回以降は省略後の形が同じ間だけ伸ばせる。Flock では既存の決定的なツール順を使い、最新情報の位置だけ後ろに移した。既存の結果サイズ上限と再取得方法を維持した。 [ツール順](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/core/system-prompt/src/index.ts#L279-L306) · [実行時情報の追記](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/core/agent-loop/src/runtime-context.ts#L69-L91) · [圧迫時の省略](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/compaction/compaction-basic/src/index.ts#L310-L368) · [キャッシュへの影響](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/compaction/compaction-tool-result-pruner/README.md#kv-cache-effect)

### 変更案の切り分け

- 6ページの結果を読む試行で配置だけを変更すると、終盤の共通先頭が約1万バイトになった。7回の合計は66,567入力トークン、キャッシュ読み取り0だった。ツール結果を別メッセージにして時刻をリクエスト末尾の `instructions` に移した次の試行は、67,107入力トークンのうち9,472トークンを読み取った。ヒット2回はいずれも4,736トークンだった。 [配置のみ](model_context_runs/layout-order-only.jsonl) · [結果の個別メッセージ化](model_context_runs/layout-separated-results.jsonl)
- 明示的な `prompt_cache_options` と `prompt_cache_breakpoint` は HTTP 400。合成入力で順番を反転した短い試行では、ヘッダーなし、`prompt_cache_key` のみ、`x-client-request-id` のみがそれぞれ0/6ヒットだった。`session-id` のみは4/6、3つ併用は2/6だった。 [境界の受付結果](model_context_runs/provider-capabilities.jsonl) · [ヘッダーの比較](model_context_runs/routing-isolation.jsonl)
- 同じ認証の HTTP で `store:false` の応答 ID を使うと HTTP 400 だった。同じ認証の WebSocket で接続を維持した合成試行では、同じ後続入力を復元する送信が全体26,296 B、差分263 Bだった。どちらも入力5,485、キャッシュ読み取り5,248トークンだった。同じプローブを再実行すると全体26,585 B、差分263 Bで、同じ5,513入力トークンのうち全体送信は5,248、差分は0を読み取った。Flock 本体は全体送信を維持し、差分による送信量削減は0。 [最初の継続試行](model_context_runs/provider-capabilities.jsonl) · [再実行](model_context_runs/websocket-reproduction.jsonl) · [WebSocket の継続仕様](https://developers.openai.com/api/docs/guides/websocket-mode#how-continuation-works)

## 再現

変更版の worktree で実行する。基準版には同じ計測ケースと HTTP 本文のバイト計測だけを持ち込む。

```sh
base_sha=f2f42eb053dc23ef235a55de8baa50ab69a07dbd
changed_sha=1a787d5bc6882ffc64da31e81171f18838e0ad7e
scratch=$(mktemp -d)
git worktree add --detach "$scratch/base" "$base_sha"
git worktree add --detach "$scratch/changed" "$changed_sha"
cp "$scratch/changed/tests/model_context.cpp" "$scratch/base/tests/"
python3 - "$scratch/base" <<'PY'
from pathlib import Path
import sys

base = Path(sys.argv[1])
updates = {
    "app/model.cpp": [
        ('    auto usage = tokenUsage(response);\n',
         '    auto usage = tokenUsage(response);\n'
         '    if (const auto bytes = response.find("transportRequestBytes"); bytes != response.end())\n'
         '        usage["transportRequestBytes"] = *bytes;\n'),
    ],
    "app/chatgpt.mjs": [
        ("async function infer(payload) {", "async function infer(payload) {\n  const body = JSON.stringify(payload);"),
        ("    body: JSON.stringify(payload),", "    body,"),
        ("return { output: completed.output, usage: completed.usage };",
         "return { output: completed.output, usage: completed.usage, transportRequestBytes: Buffer.byteLength(body) };"),
    ],
}
for name, changes in updates.items():
    path = base / name
    text = path.read_text()
    for old, new in changes:
        assert text.count(old) == 1, (name, old)
        text = text.replace(old, new)
    path.write_text(text)
PY

for version in base changed; do
    cmake -S "$scratch/$version" -B "$scratch/$version-build" -DBUILD_TESTING=ON
    cmake --build "$scratch/$version-build" --target flock_model_context --parallel 8
    "$scratch/$version-build/flock_model_context" > "$scratch/$version-fixed.jsonl"
done

# Use the profile directory Flock already uses for its ChatGPT sign-in.
profile=${FLOCK_CONTEXT_PROFILE_DIR:-"$HOME/.config/flock/credentials/chatgpt"}
mkdir "$scratch/results"
for scenario in near-limit multi-turn; do
    for run in a1 b1 b2 a2; do
        case "$run" in
            a*) version=base ;;
            b*) version=changed ;;
        esac
        "$scratch/$version-build/flock_model_context" --live "$profile" gpt-6.2-sol "$scenario" \
            > "$scratch/results/$run-$scenario.jsonl"
    done
done
python3 tests/model_context_summary.py --per-turn "$scratch/results/"*.jsonl
CMAKE_BUILD_PARALLEL_LEVEL=8 CTEST_PARALLEL_LEVEL=4 ./scripts/verify.sh
```

短い認証済み確認は同じバイナリの最後の引数を `short`、`long-followup`、`old-reference` にする。プロバイダーが途中で失敗した実行も JSONL を保存し、完了した比較とは別に記録する。

認証経路の小さなプローブには有効期限内の Flock プロファイルを使う。`websocket` には Python の `websockets` パッケージが必要。

```sh
python3 tests/model_context_probe.py "$profile" gpt-6.2-sol features
python3 tests/model_context_probe.py "$profile" gpt-6.2-sol routing
python3 tests/model_context_probe.py "$profile" gpt-6.2-sol websocket
```

## 記録の見方

`calls` の各行に `turnIndex`、`turnCallIndex`、`firstInTurn` を保存する。`commonRequestPrefixBytes`、`commonInputPrefixBytes`、`commonContextPrefixBytes` はそれぞれローカルのリクエスト全体、`input` 配列、ユーザーメッセージ内の JSON テキストが直前と一致する長さ。 `firstChangedField` と `firstChangedSection` は指示、ツール定義、履歴、依頼、結果、状態の論理的な比較位置で、プロバイダー内部の読み取り量は別途 `providerCachedInputTokens` で確認する。 `historyChange` と `toolResultsChange` は末尾追加、置き換え、切り詰めを区別する。

`transportRequestBytes` は実際の HTTP JSON 本文の UTF-8 バイト数。ヘッダーは集計外。`providerInputTokens` と `providerCachedInputTokens` はプロバイダーのレスポンス。入力の合計で読み取りの合計を割り、入力から読み取りを引いた合計を未読入力とする。使用量を受け取れなかった呼び出しは `null`。 [OpenAI のキャッシュ境界](https://developers.openai.com/api/docs/guides/prompt-caching#how-prefix-matching-works)

## 回帰確認

固定応答の6ケースは両版で成功した。単一ターンは23回で完了し、5ターンは43回で完了した。固定データの HTTP 本文に相当するローカル直列化の総バイト数は両版で一致した。 [基準版](model_context_runs/a-fixed.jsonl) · [変更版](model_context_runs/b-fixed.jsonl)

全54件のテストが通過した。権限の取り消し後に以前のツール結果をモデルへ再送しないこと、ユーザーが権限を失ったときにモデル呼び出し前に取り消すこと、通信の一時失敗後に待機時間を守って最後の完了済みステップの続きから実行することを既存テストで確認した。 `session-id` が同じ会話スコープで不変で、別スコープで変わり、スコープの生データが HTTP 本文に出ないこともテストした。 [保存済み結果と権限](bot_tools_sandbox.cpp) · [Web の権限](conversation_runtime.cpp) · [再試行と再起動](conversation_store.cpp) · [認証と HTTP](../app/tests/login.mjs)
