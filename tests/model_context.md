# モデル入力の比較と再現

[PR #289 の head からの長時間キャッシュ比較と再現](model_context_cache.md)

[85%を目標とした追加比較と再現](model_context_85.md)

比較元は `origin/main` の `2fbc64f66092794752c2c7c7b7409d42ca106b4d`。比較の両側で同じ履歴、依頼、時計の進め方、ツール応答、モデル設定を使う。依頼の受け付けから返信までを測り、事前の履歴投入は計測区間の外に置く。

## 成功条件

| ケース | 成功条件 |
| --- | --- |
| `short` | 最新の依頼に従い `MARIGOLD` だけを返す。 |
| `long-followup` | 新しい対象範囲 `staging only`、古い開始コード `ORCHID-45`、取得した状態 `PASS`、未解決の質問 `Who signs the audit?` を返す。質問は未解決のまま残る。 |
| `large-results` | 6ページをそれぞれ一度読み、最初と最後のチケット `NORTH-413 / SOUTH-982` を返す。最初の結果が省略された場合は参照から読み、参照と本文が元の結果に一致することを確認する。 |
| `old-reference` | 以前の累積要約にある書式を適用し、`expand_context` で元のコードを読み、`LEDGER VERIFIED: NZ-7184-Q` を返す。 |

## 再現コマンド

この文書の再現手順は PR #289 の head `f2f42eb053dc23ef235a55de8baa50ab69a07dbd` の worktree で実行する。

```sh
base_sha=2fbc64f66092794752c2c7c7b7409d42ca106b4d
base_dir=$(mktemp -d)
head_build=$(mktemp -d)
git archive "$base_sha" | tar -x -C "$base_dir"
cp CMakeLists.txt "$base_dir/"
cp tests/model_context.cpp "$base_dir/tests/"

cmake -S "$base_dir" -B "$base_dir/build" -DBUILD_TESTING=ON
cmake --build "$base_dir/build" --target flock_model_context --parallel 3
cmake -S . -B "$head_build" -DBUILD_TESTING=ON
cmake --build "$head_build" --target flock_model_context --parallel 3
"$base_dir/build/flock_model_context" --measure-baseline > baseline-fixed.jsonl
"$head_build/flock_model_context" > changed-fixed.jsonl
CMAKE_BUILD_PARALLEL_LEVEL=3 CTEST_PARALLEL_LEVEL=3 ./scripts/verify.sh
```

実モデルを測る場合は、比較元にも `tokenUsage` の計測変更だけを持ち込む。以下のコマンドは、比較元が送るリクエスト内容を保ったまま、プロバイダーのキャッシュ読み取りと書き込みを記録できるようにする。`FLOCK_CONTEXT_PROFILE_DIR` には有効な Flock の ChatGPT 認証ディレクトリを指定する。実測では既存認証の短期アクセストークンを使い、更新を無効にした一時プロファイルを利用した。履歴とツール応答は一時データベースと固定データを使う。

```sh
python3 - "$base_dir/app/model.cpp" app/model.cpp <<'PY'
from pathlib import Path
import sys

baseline, changed = map(Path, sys.argv[1:])
def usage_span(source):
    start = source.index("Json tokenUsage(const Json& response) {")
    return start, source.index("\n}\n}", start) + 2

text = baseline.read_text()
new = changed.read_text()
start, end = usage_span(text)
new_start, new_end = usage_span(new)
baseline.write_text(text[:start] + new[new_start:new_end] + text[end:])
PY
cmake --build "$base_dir/build" --target flock_model_context --parallel 3
"$base_dir/build/flock_model_context" --live "$FLOCK_CONTEXT_PROFILE_DIR" gpt-6.2-sol all > baseline-live.jsonl
"$head_build/flock_model_context" --live "$FLOCK_CONTEXT_PROFILE_DIR" gpt-6.2-sol all > changed-live.jsonl
```

`localRequestBytes` はローカルで直列化したリクエストの UTF-8 バイト数。`providerInputTokens`、`providerCachedInputTokens`、`providerCacheWriteTokens` はプロバイダーが返した値で、固定応答では `null`。時刻はミリ秒で、`localCallMs` は各モデル呼び出し、`wallMs` は依頼全体を測る。再取得の判断とその後の返信は呼び出し数と合計に入る。この変更による追加の要約生成はない。

## 固定応答

| ケース | 基準 → 変更後 バイト | モデル呼び出し | 依頼全体 ms | 成否 |
| --- | ---: | ---: | ---: | --- |
| Short | 42,097 → 42,412 | 1 → 1 | 5.52 → 5.21 | 成功 → 成功 |
| Long follow-up | 128,401 → 115,973 | 2 → 2 | 6.65 → 4.54 | 成功 → 成功 |
| Large results | 483,892 → 461,162 | 7 → 8 | 22.50 → 17.10 | 成功 → 成功 |
| Old reference | 127,305 → 116,743 | 2 → 2 | 5.14 → 3.77 | 成功 → 成功 |

各呼び出しの値は実行順。整数はリクエストのバイト数、括弧内は呼び出し時間 ms。

| ケース | 基準 | 変更後 |
| --- | --- | --- |
| Short | 42,097 (0.66) | 42,412 (0.58) |
| Long follow-up | 64,107 (1.01), 64,294 (1.06) | 57,893 (0.76), 58,080 (0.76) |
| Large results | 42,364 (0.63), 51,284 (0.72), 60,208 (0.83), 69,132 (0.99), 78,056 (1.03), 86,980 (1.30), 95,868 (1.29) | 42,679 (0.53), 51,599 (0.60), 60,523 (0.69), 60,767 (0.68), 61,008 (0.68), 61,249 (0.72), 61,454 (0.72), 61,883 (0.70) |
| Old reference | 63,507 (0.90), 63,798 (0.88) | 58,226 (0.74), 58,517 (0.72) |

Long follow-up は各呼び出しの履歴が 21,404 B から 14,875 B に減った。Large results の最終呼び出しに含まれるツール履歴は 53,506 B から 19,206 B に減った。最初のチケットは省略された結果に含まれるため、保存済みの結果を開く呼び出しが1回増えた。再取得を含む依頼全体の送信量は4.7%減った。4ケース合計の送信量は781,695 Bから736,290 Bに減った。

連続する呼び出しのユーザー入力 JSON で一致する先頭部分を測った。長さは Long follow-up で 50 B から 14,482 B、Large results で 50 B から 85 B、Old reference で 50 B から 15,302 B に伸びた。固定応答の計測はローカル処理に限る。プロバイダーのキャッシュ読み取り数は取得しない。

3並列で `./scripts/verify.sh --keep-build` を実行し、54件が成功した。追加修正ではモデル文脈、ワークフロー、ツール履歴投影の対象テストが成功した。レビュー修正前の投影処理に差し替えると、6,000タブずつを読む回帰テストは失敗した。

## レビュー修正前の認証済み `gpt-6.2-sol`

比較元とレビュー修正前の `f2f42eb` で、当時の依頼文を使って各ケースを1回実行した履歴。修正後の入力形式と保持規則での入力トークン数、キャッシュ率、所要時間は未測定。

| ケース | 入力トークン | キャッシュ読み取りトークン | ローカル バイト | 呼び出し | 依頼全体 ms | 成否 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Short | 6,562 → 6,631 | 4,736 → 0 | 42,097 → 42,522 | 1 → 1 | 2,873 → 3,341 | 成功 → 成功 |
| Long follow-up | 21,670 → 19,354 | 0 → 4,736 | 128,401 → 116,193 | 2 → 2 | 7,871 → 6,503 | 成功 → 成功 |
| Large results | 82,422 → 66,595 | 4,736 → 4,736 | 484,305 → 401,340 | 7 → 7 | 26,020 → 27,618 | 成功 → 成功 |
| Old reference | 21,407 → 19,439 | 4,736 → 9,472 | 127,317 → 116,963 | 2 → 2 | 7,637 → 6,776 | 成功 → 成功 |
| 合計 | 132,061 → 112,019 | 14,208 → 18,944 | 782,120 → 677,018 | 12 → 12 | 44,401 → 44,238 | 4/4 → 4/4 |

実モデルの入力トークンは15.2%減り、キャッシュされなかった入力トークンは117,853から93,075に減った。キャッシュ読み取り率は10.8%から16.9%に上がった。ヒット時は一律4,736トークンで、すべての呼び出しでキャッシュ書き込みは0だった。

次の表は `ローカル B / 入力トークン / キャッシュ読み取りトークン / 呼び出し ms` の順。

| ケース | 版 | 各呼び出し |
| --- | --- | --- |
| Short | 基準 | 42,097 / 6,562 / 4,736 / 2,863 |
| Short | 変更後 | 42,522 / 6,631 / 0 / 3,336 |
| Long follow-up | 基準 | 64,107 / 10,812 / 0 / 3,788<br>64,294 / 10,858 / 0 / 4,074 |
| Long follow-up | 変更後 | 58,003 / 9,654 / 0 / 3,293<br>58,190 / 9,700 / 4,736 / 3,202 |
| Large results | 基準 | 42,423 / 6,628 / 0 / 3,747<br>51,343 / 8,346 / 4,736 / 4,893<br>60,267 / 10,061 / 0 / 3,001<br>69,191 / 11,776 / 0 / 3,025<br>78,115 / 13,491 / 0 / 3,993<br>87,039 / 15,206 / 0 / 3,725<br>95,927 / 16,914 / 0 / 3,608 |
| Large results | 変更後 | 42,848 / 6,697 / 0 / 3,861<br>51,768 / 8,415 / 0 / 3,274<br>60,692 / 10,130 / 0 / 3,542<br>61,022 / 10,214 / 0 / 3,956<br>61,352 / 10,298 / 0 / 3,903<br>61,682 / 10,382 / 0 / 5,233<br>61,976 / 10,459 / 4,736 / 3,829 |
| Old reference | 基準 | 63,507 / 10,664 / 0 / 3,565<br>63,810 / 10,743 / 4,736 / 4,068 |
| Old reference | 変更後 | 58,336 / 9,682 / 4,736 / 3,188<br>58,627 / 9,757 / 4,736 / 3,582 |

プロバイダーのキャッシュ状態は制御していない。配置の変更がキャッシュ率に与えた効果は、この試験からは分からない。固定の4,097トークンを使った追加試験では、同じ先頭部分を再送しても読み取りと書き込みは0だった。Flock の既存認証経路は明示キャッシュ設定を HTTP 400 で拒否した。受け付けた `prompt_cache_key` では再利用を観測しなかった。 [OpenAI のキャッシュ仕様](https://developers.openai.com/api/docs/guides/prompt-caching)

## ツール結果の保持と再取得

最新バッチと、その直前に取得した結果の本文を残す。明示的に再取得した本文は、途中で別のツールを使った場合も直近2件を保持対象にする。それより古い本文は省略版との差分で最大8 KiB追加する。元の呼び出しと再取得が同じ結果を参照する場合は、最新の行だけが本文の保持候補になる。省略版も含め、ツール履歴全体をリクエストのエスケープ後で768 KiB以内に収める。

本文の省略時は状態を残す。保存済みツール結果を再取得できる行は `resultRef` を示す。ツールが既知のページング情報を返した場合は `continuation` に残す。回帰テストは、タブ6,000個を含む2件の `read_result` を同時に参照できることを確認する。両方を再取得して別のツールを呼んだ後も、元の参照と本文がそろうことを確認する。

## 参照した設計

- Pi は直近の履歴を原文で残し、既存の要約を更新する。差分送信では、設定と前回の入力に対する先頭部分の一致を確認する。 [Pi のコンパクション](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/coding-agent/docs/compaction.md) · [Pi の差分送信](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/ai/src/api/openai-codex-responses.ts#L1439-L1477)
- dsh はツール定義の順序を固定し、変動する環境情報を後ろに追記する。大きなツール結果を省くときは、先頭部分の一致が失われる位置にも注意する。Flock はユーザー文脈を1つの JSON として送り、履歴、SOUL、MEMORY を変動するフィールドより前に置く。 [dsh のリクエスト構築](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/core/agent-loop/src/agent.ts#L645-L733) · [dsh のツール結果削減](https://github.com/deepseek-ai/deepseek-harness/blob/master/packages/compaction/compaction-tool-result-pruner/README.md#kv-cache-effect)
- AlphaXiv の研究では、ツール結果を参照に置き換えると入力が減っても、再取得で時間が増えたり、応答の品質が落ちたりした。依頼全体の呼び出しと成否を比べる根拠にした。 [可逆なツール結果の省略](https://www.alphaxiv.org/abs/2610.10590) · [入力削減とキャッシュ](https://www.alphaxiv.org/abs/2611.22238)
