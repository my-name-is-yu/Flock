# キャッシュ読み取り率85%と会話の判断品質を測る

比較元は `3253574e3d725eb4b5dd6923153d64a8ac92945f`。比較元の本体は `18bbd95950e1fbe4c175d9dae37394a7c945d916`、当初の PR #289 の head は `f2f42eb053dc23ef235a55de8baa50ab69a07dbd`。比較元と同じ ChatGPT Direct OAuth、`gpt-6.2-sol`、23回の単一ターンと5ターンの長い会話を使う。 [比較元の計測](model_context_73.md) · [PR #289 からの比較](model_context_cache.md)

計測版 `93a3201d7e0106ef43e35b3986c3721b0f12bf29` は、合算 **86.96%**、1,152,381入力トークン中1,002,112トークンを読み取った。6ターンすべてに正答し、モデル呼び出しは66回から65回、根拠の復元は2回から1回に減った。未読入力150,269トークンは比較元 B1 より39,820少ない。HTTP 本文は6.55 MBで約41%増え、実行時間は217.8秒から409.5秒に延びた。同じ本体の通常実行では83.36〜86.96%が観測された。 [単一ターン](model_context_85_runs/typed-role-recheck-near-limit.jsonl) · [5ターン](model_context_85_runs/typed-role-recheck-multi-turn.jsonl)

レビュー用のブランチには、計測後に PR #289 に入った再取得と省略処理の修正も取り込んだ。上記の読み取り率は次の再現手順で使う計測版の値。 [PR #289 の取り込み元](https://github.com/my-name-is-yu/Flock/commit/190ebd413add102578245fde29c42cf9e4cdfe74)

## 認証済みの結果

各ケースは新規の一時データベースで開始した。読み取り率は2ケースの Σ cached_tokens / Σ input_tokens。比較元 B1・B2 は前回の順序を反転した比較から引用した。今回の入力配置の比較では単一ターンから始めた実行と、5ターンから始めた実行を取得した。プロバイダー側に残るキャッシュはそのまま使った。

| 実行 | 入力 | キャッシュ読み取り | 読み取り率 | 未読入力 | HTTP 本文 B | 呼び出し / 復元 | 所要秒 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| [比較元 B1](model_context_73_runs/b1-multi-turn.jsonl) | 795,529 | 605,440 | 76.11% | 190,089 | 4,643,181 | 66 / 2 | 217.8 |
| [比較元 B2](model_context_73_runs/b2-multi-turn.jsonl) | 795,919 | 582,272 | 73.16% | 213,647 | 4,645,431 | 66 / 2 | 223.3 |
| [96 KiBと役割つき履歴](model_context_85_runs/typed96-multi-turn.jsonl) | 971,363 | 794,112 | 81.75% | 177,251 | 5,590,465 | 65 / 1 | 228.8 |
| [192 KiBと失敗結果の対応](model_context_85_runs/typed192-multi-turn.jsonl) | 1,151,947 | 937,984 | 81.43% | 213,963 | 6,544,298 | 65 / 1 | 266.7 |
| [役割つき履歴 初回](model_context_85_runs/final-multi-turn.jsonl) | 1,151,922 | 966,144 | 83.87% | 185,778 | 6,544,318 | 65 / 1 | 222.5 |
| [役割つき履歴 再計測](model_context_85_runs/final-retry-multi-turn.jsonl) | 1,152,408 | 976,384 | 84.73% | 176,024 | 6,546,505 | 65 / 1 | 224.4 |
| [役割つき履歴 確認](model_context_85_runs/final-confirm-multi-turn.jsonl) | 1,151,733 | 960,128 | 83.36% | 191,605 | 6,543,940 | 65 / 1 | 244.6 |
| [同一形式のユーザー発言 初回](model_context_85_runs/canonical-multi-turn.jsonl) | 1,154,255 | 972,160 | 84.22% | 182,095 | 6,557,816 | 65 / 1 | 221.7 |
| [同一形式のユーザー発言 再計測](model_context_85_runs/canonical-repeat-multi-turn.jsonl) | 1,154,741 | 933,888 | 80.87% | 220,853 | 6,560,138 | 65 / 1 | 231.5 |
| [同一形式のユーザー発言 4.5秒間隔](model_context_85_runs/paced-multi-turn.jsonl) | 1,155,659 | 931,840 | 80.63% | 223,819 | 6,564,728 | 65 / 1 | 323.4 |
| [ツール名前空間に会話識別子](model_context_85_runs/prefix-route-multi-turn.jsonl) | 1,155,038 | 971,776 | 84.13% | 183,262 | 6,563,908 | 65 / 1 | 217.1 |
| [先頭ツールに会話識別子](model_context_85_runs/function-route-multi-turn.jsonl) | 1,171,562 | 957,312 | 81.71% | 214,250 | 6,654,456 | 66 / 1 | 230.8 |
| [HTTP/SSE 接続を再利用](model_context_85_runs/persistent-sse-multi-turn.jsonl) | 1,154,525 | 952,960 | 82.54% | 201,565 | 6,558,977 | 65 / 1 | 217.0 |
| [役割つき履歴 追加計測](model_context_85_runs/typed-role-recheck-multi-turn.jsonl) | 1,152,381 | 1,002,112 | 86.96% | 150,269 | 6,546,478 | 65 / 1 | 409.5 |
| [HTTP に prompt_cache_key を追加](model_context_85_runs/cache-key-multi-turn.jsonl) | 1,152,246 | 901,120 | 78.21% | 251,126 | 6,551,636 | 65 / 1 | 236.8 |

### ケース別の呼び出し

初回と後続は各ユーザーターンの一回目と二回目以降。未読入力には再取得や最終返信に使った呼び出しも含む。

| 実行と原記録 | ケース | 読み取り率 | ターン初回 | ターン後続 | 未読入力 | 呼び出し / 復元 | 所要秒 | 成功 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| [比較元 B1](model_context_73_runs/b1-near-limit.jsonl) | `near-limit` | 73.31% | 0.0% | 75.0% | 80,358 | 23 / 0 | 73.7 | 1/1 |
| [比較元 B1](model_context_73_runs/b1-multi-turn.jsonl) | `multi-turn` | 77.81% | 51.8% | 79.9% | 109,731 | 43 / 2 | 144.1 | 5/5 |
| [比較元 B2](model_context_73_runs/b2-near-limit.jsonl) | `near-limit` | 81.17% | 70.3% | 81.4% | 56,678 | 23 / 0 | 65.8 | 1/1 |
| [比較元 B2](model_context_73_runs/b2-multi-turn.jsonl) | `multi-turn` | 68.28% | 64.8% | 68.6% | 156,969 | 43 / 2 | 157.5 | 5/5 |
| [役割つき履歴 初回](model_context_85_runs/final-near-limit.jsonl) | `near-limit` | 91.05% | 0.0% | 92.1% | 53,480 | 23 / 0 | 75.2 | 1/1 |
| [役割つき履歴 初回](model_context_85_runs/final-multi-turn.jsonl) | `multi-turn` | 76.15% | 56.6% | 77.6% | 132,298 | 42 / 1 | 147.3 | 5/5 |
| [役割つき履歴 再計測](model_context_85_runs/replay-near-limit.jsonl) | `near-limit` | 91.05% | 0.0% | 92.1% | 53,480 | 23 / 0 | 72.5 | 1/1 |
| [役割つき履歴 再計測](model_context_85_runs/final-retry-multi-turn.jsonl) | `multi-turn` | 77.93% | 62.1% | 79.1% | 122,544 | 42 / 1 | 151.9 | 5/5 |
| [役割つき履歴 確認](model_context_85_runs/replay-near-limit.jsonl) | `near-limit` | 91.05% | 0.0% | 92.1% | 53,480 | 23 / 0 | 72.5 | 1/1 |
| [役割つき履歴 確認](model_context_85_runs/final-confirm-multi-turn.jsonl) | `multi-turn` | 75.09% | 62.1% | 76.0% | 138,125 | 42 / 1 | 172.1 | 5/5 |
| [同一形式のユーザー発言 初回](model_context_85_runs/canonical-near-limit.jsonl) | `near-limit` | 90.15% | 69.1% | 90.4% | 58,903 | 23 / 0 | 78.2 | 1/1 |
| [同一形式のユーザー発言 初回](model_context_85_runs/canonical-multi-turn.jsonl) | `multi-turn` | 77.85% | 63.1% | 78.9% | 123,192 | 42 / 1 | 143.5 | 5/5 |
| [同一形式のユーザー発言 再計測](model_context_85_runs/canonical-repeat-near-limit.jsonl) | `near-limit` | 83.41% | 0.0% | 84.4% | 99,223 | 23 / 0 | 91.5 | 1/1 |
| [同一形式のユーザー発言 再計測](model_context_85_runs/canonical-repeat-multi-turn.jsonl) | `multi-turn` | 78.15% | 57.0% | 79.7% | 121,630 | 42 / 1 | 140.0 | 5/5 |
| [同一形式のユーザー発言 4.5秒間隔](model_context_85_runs/paced-near-limit.jsonl) | `near-limit` | 89.17% | 0.0% | 90.2% | 64,791 | 23 / 0 | 103.6 | 1/1 |
| [同一形式のユーザー発言 4.5秒間隔](model_context_85_runs/paced-multi-turn.jsonl) | `multi-turn` | 71.47% | 63.1% | 72.1% | 159,028 | 42 / 1 | 219.9 | 5/5 |
| [ツール名前空間に会話識別子](model_context_85_runs/prefix-route-near-limit.jsonl) | `near-limit` | 86.26% | 0.0% | 87.3% | 82,199 | 23 / 0 | 76.5 | 1/1 |
| [ツール名前空間に会話識別子](model_context_85_runs/prefix-route-multi-turn.jsonl) | `multi-turn` | 81.85% | 80.6% | 81.9% | 101,063 | 42 / 1 | 140.6 | 5/5 |
| [先頭ツールに会話識別子](model_context_85_runs/function-route-near-limit.jsonl) | `near-limit` | 90.03% | 0.0% | 91.1% | 59,691 | 23 / 0 | 75.2 | 1/1 |
| [先頭ツールに会話識別子](model_context_85_runs/function-route-multi-turn.jsonl) | `multi-turn` | 73.03% | 68.5% | 73.3% | 154,559 | 43 / 1 | 155.6 | 5/5 |
| [HTTP/SSE 接続を再利用](model_context_85_runs/persistent-sse-near-limit.jsonl) | `near-limit` | 92.76% | 95.3% | 92.7% | 43,287 | 23 / 0 | 75.0 | 1/1 |
| [HTTP/SSE 接続を再利用](model_context_85_runs/persistent-sse-multi-turn.jsonl) | `multi-turn` | 71.55% | 63.1% | 72.2% | 158,278 | 42 / 1 | 141.9 | 5/5 |
| [役割つき履歴 追加計測](model_context_85_runs/typed-role-recheck-near-limit.jsonl) | `near-limit` | 91.05% | 0.0% | 92.1% | 53,480 | 23 / 0 | 119.3 | 1/1 |
| [役割つき履歴 追加計測](model_context_85_runs/typed-role-recheck-multi-turn.jsonl) | `multi-turn` | 82.57% | 62.1% | 84.0% | 96,789 | 42 / 1 | 290.2 | 5/5 |
| [HTTP に prompt_cache_key を追加](model_context_85_runs/cache-key-near-limit.jsonl) | `near-limit` | 83.87% | 0.0% | 84.8% | 96,360 | 23 / 0 | 74.9 | 1/1 |
| [HTTP に prompt_cache_key を追加](model_context_85_runs/cache-key-multi-turn.jsonl) | `multi-turn` | 72.12% | 62.1% | 72.8% | 154,766 | 42 / 1 | 162.0 | 5/5 |

### 達成版の各ユーザーターン

各欄は読み取りトークン / 入力トークン。初回と後続を同じターン内で分けた。

| ケース | ターン | 比較元 B1 初回 | 比較元 B1 後続 | 達成版 初回 | 達成版 後続 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `near-limit` | 1 | 0 / 6,739 | 220,672 / 294,291 | 0 / 6,809 | 543,744 / 590,415 |
| `multi-turn` | 1 | 4,736 / 6,899 | 66,688 / 83,270 | 0 / 6,969 | 80,256 / 96,768 |
| `multi-turn` | 2 | 0 / 7,158 | 70,272 / 85,083 | 4,736 / 7,245 | 82,304 / 98,700 |
| `multi-turn` | 3 | 4,736 / 7,327 | 71,296 / 86,266 | 6,784 / 7,432 | 85,376 / 100,009 |
| `multi-turn` | 4 | 4,736 / 7,478 | 68,224 / 87,323 | 6,784 / 7,601 | 85,120 / 101,192 |
| `multi-turn` | 5 | 4,736 / 7,707 | 89,344 / 115,988 | 4,736 / 7,848 | 102,272 / 121,393 |

達成版では、直前の入力がある63回のうち58回で、その入力の全メッセージが先頭に一致した。その58回では毎回キャッシュを読み取った。5ターンの33回目で古い返信が省略され、34回目で要約が反映された。 [単一ターンの全呼び出し](model_context_85_runs/typed-role-recheck-near-limit.jsonl) · [5ターンの全呼び出し](model_context_85_runs/typed-role-recheck-multi-turn.jsonl)

## 実装

- 保存済みの Bot 応答を `assistant` のメッセージで渡し、ユーザーの発言や過去の作業記録は種類を添えて時系列に置く。この本体変更の SHA は `ff3c0cddd55d043302e143d5df43d5a08a65a31e`。達成版の次のコミットは一時データベース内で試験用の `read_memory` を許可する変更。
- 現在ターンの、再認可・秘匿処理を通ったツール記録は `function_call` と `function_call_output` を安定した `call_id` で結ぶ。失敗した呼び出しも、対象とエラーが対応する。実行されなかった結果や過去と同じ権限で再生できない結果は、ステップの記録として伝える。Flock の SQLite にある実行状態と現在の権限が引き続き判断の基準になる。
- 大きな `web_fetch` の全文を追記する上限を40 KiBから192 KiBに変更した。圧迫時は参照付きの省略形にそろえ、ツール履歴全体の上限は768 KiB。最新ステップと最初の根拠を優先する。前回の5ターンで再取得していたページは、最終判断まで本文を保持できる。
- `expand_context` が単独実行を要求することを、モデルへ渡すツール定義と指示に記載した。ツールの順序、最新状態、承認、再試行は Flock が管理する。

既存認証の合成プローブでは、元の `call_id`、Flock が組み立てた ID、以前の `assistant` メッセージを加えた場合の3案を確認した。すべて名前空間付きの `function_call_output` を受け取り、出力に埋め込まれた指示を採用せず、架空の記録の印章を正確に返した。 [Pi の変換処理](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/ai/src/api/openai-responses-shared.ts#L233-L355) · [Flock の組み立て](../app/model.cpp) · [結果の保持](../src/conversation_projection.cpp)

### 判断品質と追加試行

- 同じ先頭部分が残っても、プロバイダーの読み取り量は実行によって変わった。役割つき履歴だけの版で、5ターンの32回目は直前のメッセージをすべて再送した。入力はいずれも19,589トークンで、読み取りは17,536と0だった。 [読み取れた実行](model_context_85_runs/final-retry-multi-turn.jsonl) · [読み取れなかった実行](model_context_85_runs/final-confirm-multi-turn.jsonl)
- 比較元と変更版の両方で監査範囲の訂正、保存済み受領文の原文、取得失敗を示す `UNVERIFIED` を正しく返した。比較元は状態取得と並行して `expand_context` を提案し、Flock が復元を保留したあとに再提案した。変更版は復元を単独で提案し、続けて状態取得を実行した。5ターンでも達成版は終了条件をすべて満たし、本文を保持できたため再取得が2回から1回に減った。 [比較元の判断](model_context_85_runs/baseline-quality-replay.jsonl) · [変更版の判断](model_context_85_runs/typed-role-recheck-quality-replay.jsonl)
- 短い通常のユーザー発言を、現在の依頼でも後続ターンの履歴でも同じメッセージ形式で渡す試作は `b998fb3cb6fb83107e4fbc7b781368950b9378a4`。5ターン実測では、ターン2・3・4の初回で直前と一致したメッセージがそれぞれ2・4・6個から4・6・8個に増えた。合算の読み取り率は84.22%と80.87%だった。 [試作の5ターン](model_context_85_runs/canonical-multi-turn.jsonl)
- Web の応答から必要なキーを選ぶ `web_fetch.fields` を試作した。23回のケースで未読入力は39,274トークン、読み取り率は79.36%だった。5ターンは計測側が `read_memory` を止めたため中断扱いとした。以後、計測側は一時データベース内でこの読み取りを許可した。試作の SHA は `94ff52d07eba373186951f02c348663ce075fe35`。 [単一ターン](model_context_85_runs/fields-near-limit.jsonl) · [5ターン](model_context_85_runs/fields-multi-turn.jsonl)
- 同じ認証で WebSocket 接続を維持し、毎回全入力を送った5ターンでは、読み取り率75.47%、119.7秒だった。比較対象の同じ版の HTTP は76.15%、147.3秒。Pi の差分送信は、前回の全入力とモデル出力が今回の先頭に一致し、前回応答を同じ接続で参照できる場合に使える。Flock の本体は現行の HTTP 全体送信を使う。差分送信のバイト数と読み取りトークンは前回の合成試行に記録した。 [常時接続の実測](model_context_85_runs/websocket-full-multi-turn.jsonl) · [Pi の差分と全体送信](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/ai/src/api/openai-codex-responses.ts#L1426-L1589) · [差分の実測](model_context_cache.md)
- Pi は入力以外の設定も一致するか確認する。Flock は呼び出しごとに時刻を更新し、実行済みツールの引数と結果を最新の権限・秘匿処理で再構成する。プロバイダーから受け取った応答全体は保持していない。ローカル入力の先頭が一致しても、Pi の差分条件は満たせない。履歴を要約に置き換える際も全体を再評価する。 [Pi の一致判定](https://raw.githubusercontent.com/earendil-works/pi/1402973a9dbe8c627341e04c9744c54953e285ca/packages/ai/src/api/openai-codex-responses.ts#L1331-L1379) · [Pi の compaction](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/coding-agent/docs/compaction.md#how-it-works)
- 名前空間付きのツールを使う新しい合成試行では、元の実行結果を含む論理入力を全体送信と差分送信でそろえた。順序を反転した2回とも正答し、送信本文は27,736 Bから957 Bに減った。読み取りは全体・差分の順に1回目が5,376・5,632、2回目が5,376・0トークン。全体送信は別接続、差分送信は元応答の接続を使用した。送信から完了までの時間は順に4.85・4.38秒と1.91・1.57秒だった。 [Pi の形式を使った測定](model_context_85_runs/pi-tool-delta.jsonl) · [再現用のプローブ](model_context_probe.py)
- HTTP/SSE のまま接続だけを再利用した試行では、両ケースで同じアプリ入力を送り、合算82.54%、217.0秒だった。短い判断品質のケースを含む68回の送信で、接続は1本、再利用67回だった。 [接続を再利用した5ターン](model_context_85_runs/persistent-sse-multi-turn.jsonl) · [接続回数](model_context_85_runs/persistent-sse-worker.log)
- 変動する状態を最後の `assistant` 項目に置く合成試行は、状態だけが変わる4回目で、9,216トークンと0トークンの両方を観測した。比較した `user` 項目はどちらも0で、全16呼び出しの判断は意図どおりだった。効果は繰り返し得られなかったため、本体の状態の役割は変更していない。 [末尾の状態を比較した記録](model_context_85_runs/state-tail-probe.jsonl)
- ヘッダーにある既存の会話識別子の一部を、ツール名前空間または最初の個別ツールの説明に加える試行も行った。名前空間では単一ターンの入力トークン数が全23回で同じだった。個別ツールではその合計が276トークン増えた。2ケースの合算率は84.13%と81.71%で、本体には加えていない。 [名前空間の試行](model_context_85_runs/prefix-route-multi-turn.jsonl) · [個別ツールの試行](model_context_85_runs/function-route-multi-turn.jsonl)
- OpenAI は同じキャッシュを使う要求が毎分15件を超えると別の処理先へ振り分けられる場合があると案内している。ビルド済みの橋渡しスクリプトに一時的な待機を入れ、要求開始を4.5秒以上空けた実行は80.63%、323.4秒だった。同じ版の通常実行は84.22%、221.7秒。待機は本体に加えていない。 [間隔を空けた実測](model_context_85_runs/paced-multi-turn.jsonl) · [OpenAI のキャッシュ配置](https://developers.openai.com/api/docs/guides/prompt-caching#cache-location)
- dsh からは、ツール定義の順序を安定させ、容量を超えるまで結果を保持し、圧縮後に原本へ戻れる形を参考にした。Flock では Web 結果をまとめて省略するため、その呼び出しは最初の変更箇所で一致が途切れる。次のステップでは同じ省略形が残る範囲を再利用できる。時刻だけは Flock が生成した値をリクエスト単位の `instructions` に載せる。 [dsh のツール順序](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/core/system-prompt/src/index.ts#L279-L306) · [dsh の省略とキャッシュ](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/compaction/compaction-tool-result-pruner/README.md#L112-L124)
- 固定応答7ケースと `scripts/verify.sh` の54件が成功した。最新の権限での結果の再投影、保存済み根拠の復元、一時失敗後に完了した処理を重ねず再開するケースを含む。短い依頼は同じ認証経路の1回の呼び出しで完了した。 [固定応答7ケース](model_context_85_runs/typed-role-recheck-fixed.jsonl) · [短い依頼](model_context_85_runs/typed-role-recheck-short.jsonl)

## 再現

この文書のある作業ツリーのルートから、計測版 `93a3201d7e0106ef43e35b3986c3721b0f12bf29` のバイナリを別の作業ツリーに作る。比較元を再計測する場合も、比較元の SHA を指定して `near-limit` と `multi-turn` を実行する。

```sh
results=$(mktemp -d)
git worktree add --detach "$results/source" 93a3201d7e0106ef43e35b3986c3721b0f12bf29
cmake -S "$results/source" -B "$results/build" -DBUILD_TESTING=ON
cmake --build "$results/build" --target flock_model_context --parallel 8
profile=${FLOCK_CONTEXT_PROFILE_DIR:-"$HOME/.config/flock/credentials/chatgpt"}
for run in 1 2; do
    if [ "$run" = 1 ]; then
        set -- multi-turn near-limit
    else
        set -- near-limit multi-turn
    fi
    for scenario do
        "$results/build/flock_model_context" --live "$profile" gpt-6.2-sol "$scenario" \
            > "$results/$run-$scenario.jsonl"
    done
done
"$results/build/flock_model_context" --live "$profile" gpt-6.2-sol quality-replay > "$results/quality-replay.jsonl"
"$results/build/flock_model_context" --live "$profile" gpt-6.2-sol short > "$results/short.jsonl"
python3 tests/model_context_summary.py --per-turn "$results/"*.jsonl
python3 tests/model_context_probe.py "$profile" gpt-6.2-sol transcript > "$results/typed-probe.jsonl"
python3 tests/model_context_probe.py "$profile" gpt-6.2-sol websocket-tool-transcript > "$results/pi-tool-delta.jsonl"
CMAKE_BUILD_PARALLEL_LEVEL=8 CTEST_PARALLEL_LEVEL=4 ./scripts/verify.sh
```

`quality-replay` は以前の production の監査範囲を staging に訂正し、現在の状態の取得は失敗する。同じ依頼では保存済みの受領文を原文で復元し、訂正後の範囲と `UNVERIFIED` を返す。復元と状態取得はそれぞれ1回を合格条件とした。固定の `large-results` は別の省略・復元テストで、192 KiBを超えるように拡張した。主計測2ケースの依頼文、ツール応答、正答条件は従来と同じ。

WebSocket のプローブは Python の `websockets` パッケージを使う。
