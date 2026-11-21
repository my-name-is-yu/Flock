# Flock v0 requirements and implementation order

Flock is a runtime for multiple independent Bots. Each Bot has its own role, memory, and recurring requests. It delegates the work for its tasks to T3 Sessions that it starts. The Bot takes requests and answers questions from active Sessions. When a Session finishes a Run, the Bot evaluates the result and decides whether to complete the task, request more work, or ask the user a question.

v0 consists of a single local C++20 runtime, SQLite, a CLI, a local Web UI, and T3 Code. Verify the full lifecycle of a request, from intake through a Session failure and a Flock restart to evaluation of the result and task closure. Use each Bot's conversation for day-to-day requests and answers. [Local Web UI specification](UI.md)

## Per-Bot configuration and state

| Item | Requirement |
| --- | --- |
| Bot configuration | Set the Bot's decision model and provider, the delegation target's T3 `environmentId` and `projectId`, the permission mode, the Run deadline, and cron in `bots/<botId>/config.json`. The Session model and provider are separate optional settings. When omitted, use the T3 project defaults. If those are also missing, show the configuration issue before delegation. Use the project's default workspace and specify `workspaceStrategy` only when needed. |
| `SOUL.md` | In the same directory, the user writes the Bot's role, priorities, and criteria for evaluating results. |
| `MEMORY.md` | The user records assumptions and previous decisions that should carry across tasks. On each inference turn, the Bot reads the current versions of its SOUL and MEMORY. |
| cron | Stores the recurring request ID, schedule, time zone, and request text. |
| Task history | Store requests, completion criteria, Sessions, work reports, and evidence used by the Bot in SQLite, keyed by Bot ID and task ID. Pass only the history of the relevant task to the model. |

Manual requests and cron use the same intake path. Cron uses five fields (minute, hour, day of month, month, and day of week) and an IANA time zone. While the runtime is running, register each combination of Bot ID, recurring request ID, and scheduled UTC timestamp once. Skip local times that do not exist at a daylight saving transition. When a local time occurs twice, treat each occurrence as a separate UTC schedule. Skip schedules that elapsed while the runtime was stopped and continue processing requests already registered after restarting.

Tasks that share a T3 environment and project run one at a time in the order received, across all Bots. Use the canonical `(environmentId, projectId)` as the serialization key. While the preceding task is waiting for user input or being cancelled, later manual and scheduled requests stay queued, and the CLI shows the preceding task. Tasks in different projects can proceed independently.

## Task intake and completion criteria

A **Task** is work owned by a Bot. At intake, the runtime saves the request, target project, and execution settings. The Bot reads the request and SOUL, then states the completion criteria in `doneWhen` on its first `delegate` action. If anything is unclear, it calls `ask_user` first. One example criterion is “Fix the bug and include a regression test in the same PR that fails before the fix.”

Save the first delegation and `doneWhen` in the same transaction. The current revision's saved `doneWhen` governs the completion decision. Use the latest SOUL and MEMORY to decide how to proceed under those criteria. Reject later delegations that supply a new `doneWhen`. If the user explicitly changes the completion criteria for an active task, save a new revision of the same task. Confirm that any superseded Run and its delegated work have ended before starting replacement work. Keep earlier Runs and reports in the history under their original revisions. Evaluate completion using evidence for the current revision.

## Bot-to-Bot requests and replies

The sender's `allowedBots` configuration controls which Bots it may contact; permission is directional. The recipient uses its own model, SOUL, MEMORY, and T3 settings. `send_bot_message` can request a consultation or work, and the sender waits for a question, answer, result, or failure from that exchange. `reply_to_bot` routes a response to its originating request. `read_bot_evidence` validates the returned Run against the exchange before saving excerpts. Persist the exchange and recipient task together so a restart does not duplicate delivery; cancelling the originating task also cancels work created for that exchange.

## Agent Loop

Implement the agent loop in Flock's C++20 runtime. For each event, the Bot picks one valid action from the five in the table below, and the runtime validates and commits it. When history reading is enabled, the Bot can read records for the same event before committing. To connect a live model, Flock uses the official Sign in with ChatGPT flow to obtain browser approval, then calls the Responses API directly using an eligible ChatGPT plan. Select the model in the Bot configuration from those available to the connected account. The runtime maps Responses API tool calls to the same action contract used by fixed-response models. Keep Flock's credentials and execution path independent of the Codex CLI. [Official Sign in with ChatGPT implementation guide](https://developers.openai.com/cookbook/articles/sign-in-with-chatgpt)

Store Flock's model credentials separately from T3 credentials. Confirm that the user authorized Flock to use their plan. Store the credentials in the OS credential store or in a file readable only by its owner. Keep credentials out of Bot configuration, task history, logs, and model input.

The public Devin Bots specification also informs the division of responsibilities between Bots and Sessions. In Flock, the Bot handles the task and the assigned Session implements, builds, and verifies the work. [Devin Bots configuration and child Sessions](https://docs.devin.ai/work-with-devin/bots)

When T3 returns a Run ID for a `delegate` call, the runtime saves the ID and starts monitoring the Run. Monitoring an active Run requires zero model calls. When the Session asks a question, pass it to the owning Bot. When the Run ends, pass its work report or failure reason to the Bot. If delegated work is still active, wait for it to settle. Monitoring can resume after a Flock restart.

The implementation draws on how Pi extracts tool calls from a model response and feeds their results back into the next inference, and how Codex routes each tool call to its handler. Flock persists the action and waits for an event before the next inference. [Pi's `runLoop`](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/agent/src/agent-loop.ts#L241-L277) · [Codex's `ToolRouter`](https://github.com/openai/codex/blob/7edf91b13c54f20aab22fece0abfa4a5c28379b3/codex-rs/core/src/tools/router.rs#L284-L424)

Pi exposes separate tools for reading, editing, and writing files. In Codex's multi-agent implementation, `send_message` and `followup_task` share internal delivery code. The first delivers a message, and the second can start a new turn for the recipient. Flock likewise uses separate tools to answer an existing question and to request additional work that needs a new Run. [Pi tool definitions](https://github.com/earendil-works/pi/blob/1402973a9dbe8c627341e04c9744c54953e285ca/packages/coding-agent/src/core/tools/index.ts#L194-L202) · [Codex message delivery](https://github.com/openai/codex/blob/7edf91b13c54f20aab22fece0abfa4a5c28379b3/codex-rs/core/src/tools/handlers/multi_agents_v2/send_message.rs#L40-L47) · [Follow-up delivery](https://github.com/openai/codex/blob/7edf91b13c54f20aab22fece0abfa4a5c28379b3/codex-rs/core/src/tools/handlers/multi_agents_v2/followup_task.rs#L40-L47)

A **Session** (child Session) is a T3 Thread started by the owning Bot for a task. A **Run** is a single execution of an instruction in that Thread. The Session handles research, file operations, and tests. A task can have multiple Sessions in its history, with at most one active Run at a time.

1. **Receive an event.** The CLI records requests, answers, and cancellations in SQLite. The runtime monitors T3 and also records unanswered questions and Run endings as events. Monitoring alone makes zero model calls.
2. **The Bot decides.** The model receives the owning Bot's SOUL and MEMORY, the request, the saved completion criteria, the remaining work allowance, the current event, and the task's recent instructions and reports. Use the relevant Run's final answer as its report. If it ended abnormally, include the reason and any final answer available. Mark long records as truncated and include a reference with the environment, Thread, Run, and item IDs. Set size limits on history reads and model input. Retrieve older details in pages.
3. **The runtime commits the decision.** The model returns the tool name, arguments, and rationale. The runtime checks the task, Session, event generation, question validity, and remaining allowance. Perform external calls outside SQLite transactions. On commit, store the processed event, action, allowance, and state transition together. If the Run ends or a cancellation arrives during inference, discard the stale decision and process the latest state.
4. **The runtime executes the action.** Send the committed instruction to T3 or display the question or completion in the CLI. The runtime manages waits and deadlines.

| Action | What the Bot decides | What the runtime validates and executes |
| --- | --- | --- |
| `delegate` | What a new Session should do and what result to expect. Include `doneWhen` on the first delegation. | Start a Thread in the target project and associate the task, Thread, and Run. |
| `answer_session_question` | How to answer the Session's question. Supply the question ID and an answer for each prompt. | Reply to the pending `user_input` for the task's Run and resume monitoring that Run. This consumes zero additional runs. |
| `follow_up` | What additional work the existing Session should do and why. | Send the instruction to the Thread for the task's finished Run and record a new Run. |
| `complete` | Which result satisfies `doneWhen` and the evidence that supports completion. | Save the Bot's rationale in the task history and record completion. |
| `ask_user` | What the user needs to answer before work can proceed. | Save the question and rationale and display the question in the CLI. Associate the answer with that question and process it once in the same task. |

If a Session asks which branch to use and the request names the branch, the Bot returns it via `answer_session_question`. If the user needs to decide, link `ask_user` to the original Session question and answer that same question after the user responds. A request to test another branch requires additional work, so use `follow_up` once the current Run ends.

**Evaluate `session_read` as an option.** Start with the five actions and stored reports. For the comparison only, allow the Bot to use `t3_thread_read` to read the original record for a Session and Run belonging to the task, with an item or range specified. Return the source, excerpt, and whether more is available to the same event. Reading alone starts zero Runs, consumes zero additional runs, and leaves the event pending. Start the comparison with a limit of three reads per event and 8 KiB of UTF-8 per excerpt. After the limit is reached, present only the five core actions. Save each excerpt, its source reference, and the number of reads used against the event. After a restart before the decision commits, resume with the same excerpts and remaining read budget. Use the phase 4 comparison to decide whether to adopt history reading by default. [Research comparing how much history to provide](https://arxiv.org/abs/2508.21433)

**Allow up to three additional runs after the initial delegation.** Later `delegate` actions and `follow_up` consume one run per action ID at commit. Delivery retries, answers to Sessions, questions for the user, and completion decisions consume zero. If the Bot asks for more work after it has used its allowance, hold the instruction, show the reason, and wait for user input.

Set a deadline for each model call. Limit timeouts, transient errors, invalid formats, and disallowed actions to three failures per event in total. Increase the delay after transient errors and return the reason for format and state violations. When more work requires explicit approval, apply the work allowance procedure first. Keep the failure count across restarts and wait for user input after three failures. Record an explicit retry from the CLI as a new attempt allowance in the history.

## Task states and where work resumes

| What triggers a decision | Actions the Bot can commit |
| --- | --- |
| After intake and before the first delegation | `delegate` with a required `doneWhen`, or `ask_user`. |
| The task's Run has an unanswered question | `answer_session_question` for that question, or `ask_user` linked to the original question. |
| The task's Run result is available and delegated work has settled | `delegate`, `follow_up` in the finished Session, `ask_user`, or `complete` if the saved criteria are met. |
| Retry, user answer, or approval of more work | Return to the original decision point and apply the same rules to the current state. |

| State | Transition |
| --- | --- |
| Queued → Awaiting decision | Start the task when it reaches the front of the project queue. |
| Awaiting decision | A committed delegation, follow-up, or Session answer moves the task to Awaiting delivery. A question for the user moves it to Waiting for input. A committed completion moves it to Complete. |
| Awaiting delivery | For launches and follow-ups, identify the corresponding Run and move to Session running. After answering a question, resume monitoring the same Run. If delivery is uncertain, follow the recovery procedure. |
| Session running | Move to Awaiting decision after a verified question for this Run arrives or the work ends. Continue monitoring ordinary progress. |
| Waiting for input | Save the reason for the wait, the original event, and the relevant action, Run, and question as applicable. Resume as described below. |
| Cancelling → Cancelled | Stop new inference, work requests, and answers to questions. Reconcile in-flight delivery and active work. Start the next task in the project after the relevant work ends. |
| Complete or Cancelled | These states are terminal. Ignore late events that could restart work. |

**Resolving a wait for user input.** The CLI shows the reason for the wait and the available actions. Accept a user's action only once for the relevant wait ID. The user can cancel the task for any wait reason. Continue monitoring trackable Runs during a wait and prioritize cancellation once requested.

| Reason for waiting | User action and resumption |
| --- | --- |
| Bot question | Save the answer and return to the original decision. If the original Session question is resolved first, clear the wait and resume monitoring. If the Run has ended, process its result. Discard late answers. |
| Additional work limit | “Approve one more run” increases the allowance by one and triggers a new decision using current information. Keep the rejected instruction unsent. An answer to a regular question leaves the allowance as it is. |
| Model failure limit | “Retry decision” resumes from the same unprocessed event. |
| T3 approval pending or a question that cannot be answered automatically | Show the Thread link so the user can respond in T3. Resume monitoring the same Run after resolution. |
| Run deadline reached | Continue monitoring. “Extend deadline” saves a new absolute deadline. If the work ends while waiting, evaluate its result immediately. |
| Uncertain delivery of an instruction or answer | “Reconcile” or a user-specified Run or question starts the recovery procedure. Resend a delegation or follow-up only after confirming that T3 did not accept it. |
| Expired T3 credentials or missing execution settings | Pause the affected connection. Reconcile after reauthentication or a configuration fix. If delivery had yet to start, return to the pre-delivery step. If delivery was uncertain, use the recovery procedure. |

**Cancellation and deadlines.** Save the Run's absolute deadline before the first send and preserve it across restarts. When the deadline is reached, move to Waiting for input as described above. For an explicit cancellation, first persist the cancellation request, then send `t3_thread_interrupt(threadId, runId)` for the identified active Run. If canceled during delivery, reconcile the delivery outcome and apply the same procedure to any relevant Run identified later. Confirm that the targeted work has ended after interrupting it. If delegated work is still active or its location is unknown, show the reason and links to known Threads in the CLI and keep the task in Cancelling. The user can stop the remaining work in T3. The runtime records cancellation once it verifies that the relevant work has ended. Limit automatic interruption to the targeted Run so other work the user started in the same Thread can continue. [T3 interruption specification](https://github.com/pingdotgg/t3code/blob/v0.1.11/docs/orchestration-v2/orchestrator-mcp-server.md#t3_thread_interrupt)

## Using external verification tools

Set up verification tools in separate projects as needed. Document their use in the request or in the Bot's SOUL. In v0, the delegated Session uses the tools. The Bot can include verification in the initial instructions or review the report and follow up with just the checks it needs. The Session includes tool results or links to them in its work report.

If the completion criteria require a test to fail before a fix and pass afterward, the Bot asks for both results. If it receives only the post-fix result and finds no pre-fix result in the history, it can request that evidence with `follow_up`. If the Session could not use a tool, the Bot uses the reported circumstances to decide what to do next. Adding verification tools uses the same actions exposed to the owning Bot.

When the completion criteria specify a commit or configuration, verify that the result came from that commit and configuration. If the request is to measure memory use in a build with updates enabled, measurements from a build with updates disabled leave the criterion open. Use the matching measurement record if it exists, or ask the Session to measure under the requested conditions.

## Connecting to T3 v0.1.11

Flock connects to T3's `/mcp` endpoint as an external MCP client. Manage the connection URL and OAuth credentials per environment. On first connection, the CLI dynamically registers an OAuth client and the user approves it in the browser through the authorization code flow with PKCE (S256). Store tokens in the OS credential store or in a file readable only by its owner so the runtime can reuse the credentials after a restart. Keep tokens out of Bot configuration, task history, logs, and model input. Launching, sending, and interrupting require an approval level that permits writes. Each Bot's permission mode must stay within that limit. External sign-in lasts 30 days in this version. This version issues no refresh tokens. When credentials expire, reauthenticate the same environment in the browser and reconcile the paused operations. [T3 external client specification](https://github.com/pingdotgg/t3code/blob/v0.1.11/docs/user/outside-agents.md)

| Flock operation | T3 behavior |
| --- | --- |
| `delegate` | Call `t3_thread_launch` in the target project. Flock associates the ordinary Thread with its own task. If the returned Run ID is still pending, match the sent action ID against the Thread history. |
| `follow_up` | Request a new Run with `t3_thread_send(mode="queue")` in the corresponding Thread after the previous work has ended. If a separate Run was started manually, hold the send and reconcile the target again. |
| Monitoring and history reads | Use the same target Run ID for short `t3_thread_wait` calls. Check for questions, deadlines, and cancellation between calls. A wait timeout means work is still ongoing. Follow explicit cursors from `t3_thread_read` to page through history. Use `messages` for reports and `activity` for questions and work events. If a response is truncated, retrieve the rest using the item ID and offset. |
| `answer_session_question` | Identify the question via `list` and `read` on `t3_pending_request`. Check that it is still unanswered immediately before calling `respond`. Use a separate delivery path for starting Runs. |
| `ask_user` and `complete` | Handle them in Flock's SQLite database and CLI. |

**Associating questions with Runs.** T3's `list` returns IDs of pending `user_input` requests, and `read` returns a question ID and its prompts. Neither returns a Run ID. Flock deduplicates by `(environmentId, threadId, requestId)`. Match unanswered prompts to the question events in the assigned Thread's `activity`, which identify the originating Thread, Run, and prompts. Answer automatically only when the prompts match exactly one question in the monitored Run. A count alone is insufficient evidence of Run ownership. Direct the user to T3 if multiple Runs contain identical questions, the record is incomplete, or the question format is unsupported. Validate required prompts, available options, multiple selections, and free-text input against T3's definitions. Use the Thread-wide count of unresolved items, including approvals, as a monitoring signal. The user handles approvals in T3. If the user answers first, treat the question as resolved and resume monitoring. [Listing and answering questions](https://github.com/pingdotgg/t3code/blob/v0.1.11/apps/server/src/mcp/toolkits/thread/handlers.ts#L248-L285) · [Question event fields](https://github.com/pingdotgg/t3code/blob/v0.1.11/apps/server/src/mcp/OrchestratorMcpService.ts#L1206-L1228)

**When work ends.** `completed` means the Run ended successfully. If the Session is still waiting on delegated work, use `untilDelegatedWorkSettled` to wait for it. When T3 returns a `resultRun`, retrieve the final report from that Run. Keep references to both the original Run and the Run that produced the report. Evaluate the result once per delegation. For failures, interruptions, and similar outcomes, pass the reason and any partial report to the Bot. The Bot decides whether the task is complete based on its saved criteria. [Wait result implementation](https://github.com/pingdotgg/t3code/blob/v0.1.11/apps/server/src/mcp/OrchestratorMcpService.ts#L3706-L3719)

## Restarts and duplicate prevention

Before delivery, the runtime saves the action ID, destination, payload, and credential generation as a non-secret identifier. It records the start of delivery just before the external call. For launches and follow-ups, include the full action ID in the instruction and pass the same value as the MCP `clientRequestId`. Associate each answer with the original question ID. Ingest completion events once per environment, Thread, and Run. Ingest questions once per environment, Thread, and question ID.

| State at restart | How to resume |
| --- | --- |
| Event saved, decision pending | Re-evaluate the unprocessed event. |
| Action committed, delivery pending | Verify that the task has not been canceled, the question has not been resolved, and the action is still applicable. Then send the saved instruction. |
| Delegation or follow-up delivery started, acceptance result pending | For a new delegation, inspect the target project's Threads. For a follow-up, inspect the known Thread's messages. Reconnect if exactly one Run matches the full action ID, destination, and payload. If there are zero or multiple matches or reconciliation is inconclusive, disable automatic resending and wait for user input. Use titles only to narrow the search. |
| Question answer delivery started, acceptance result pending | Reconcile using the saved Thread and question ID. If resolved, resume monitoring the relevant Run. If the question disappeared or expired, ingest the latest Run state. If acceptance is still uncertain, disable automatic resending of the answer and wait for user input. |
| T3 ID or result saved | Resume monitoring the same Run or evaluating the saved, unprocessed event. |

**Preventing duplicate sends on reconnection.** For external launches and follow-ups, T3 v0.1.11 uses a namespace derived from the OAuth client session ID together with `clientRequestId`. A Flock restart or a new HTTP connection can reuse the same credentials. Signing in again can change the credential session. A changed session can turn an instruction with the same key into new work, so reconcile against history first. Follow the same procedure when credential continuity is uncertain. [External client namespace](https://github.com/pingdotgg/t3code/blob/v0.1.11/apps/server/src/auth/McpOAuth.ts#L620-L634) · [Launch ID generation](https://github.com/pingdotgg/t3code/blob/v0.1.11/apps/server/src/mcp/toolkits/project/handlers.ts#L210-L219) · [Follow-up ID generation](https://github.com/pingdotgg/t3code/blob/v0.1.11/apps/server/src/mcp/OrchestratorMcpService.ts#L3622-L3639)

For transient communication failures, retry on the existing connection only for paths verified against live T3 to avoid duplicates with the same credential session, payload, and key. Wait between attempts and allow up to two retries per action after delivery starts. After restarting or reauthenticating, reconcile as described above first. Show the instruction and action ID in the CLI. Reconcile again if the user specifies an existing Run. After reconciliation, allow manual resending only if T3 confirms the instruction was never accepted. Zero history matches alone are insufficient to establish that.

## Implementation order and acceptance criteria

### 1. SQLite and Agent Loop

Start with C++20, SQLite, and fixed responses for the model and T3. Take one request through delegation, an answer to a Session question, a Flock restart, receipt of the same Run's report, and completion. Share SQLite between the CLI and runtime, enable WAL, and set a finite lock wait. Allow one runtime process per database. Prevent duplicates with unique constraints and transactions for events, actions, and answers.

- Execute zero unknown, multiple, or out-of-state actions. Include attempts to complete before initial delegation, delegate additional work while stopped at an unanswered question, and overwrite committed `doneWhen`. Make zero model calls during ordinary Run monitoring.
- Reproduce an ambiguous request, a user answer, initial delegation, Run failure, a follow-up, and completion. Verify that the first delegation waits for the answer and that the Bot continues making decisions on the same task after the failure.
- After three consecutive invalid model responses, wait for user input. Deadline expiration and pending approval each result in zero new Runs and zero automatic interruptions. For explicit cancellation, hold the project's next task until the interruption of the targeted Run is confirmed. After restarting during cancellation, continue tracking the same target.
- Request three additional Runs across existing and new Sessions, then ask the user before requesting a fourth. An ordinary answer leaves the allowance as it is. Delivering the same approval twice adds one run. Redelivering events after cancellation produces zero new sends.
- Compare a Session asking for the target branch when the request already specifies it and when only the user can decide. The first case produces zero questions for the user and the second produces one. Both cases send one answer, start zero new Runs, and use zero additional runs from the allowance. Requesting a different test after the Run ends starts one new Run and consumes one run from the allowance.
- Reproduce a Run ending while waiting for a deadline extension, a question being resolved in T3 while the Bot is waiting for the user to answer it, and approval for more work followed by cancellation. Verify that stale CLI input for a wait that has cleared causes zero sends, zero allowance increases, and zero task resumptions.
- Change an active task's completion criteria and restart while the superseded Run is stopping. Verify that the task keeps its ID, that replacement work waits for the old Run and its delegated work to end, and that completion uses the replacement Run's evidence under the new revision.

### 2. Bot configuration, cron, CLI, and a live model

- Deliver the same cron occurrence twice and verify that it registers once. A time skipped at the start of daylight saving registers zero occurrences. A time repeated at the end registers twice with distinct UTC timestamps. Accept another request for the same project while the preceding task is waiting for user input. After a restart, verify that the answer reaches the original question and the queued request starts when the preceding task closes.
- Edit SOUL after the first delegation and verify that the active task is still evaluated against its saved criteria. Give the Bots different MEMORY content to verify that model input includes only the owning Bot's MEMORY after a restart.
- Approve Sign in with ChatGPT for Flock in a browser and connect directly to the Responses API with an eligible plan. Verify model selection and tool calling on the live connection. Give both Bots the same report, “All existing tests passed. No regression test was added.” The Bot that requires the regression test chooses a follow-up. The Bot that requires the existing tests to pass chooses completion. Hold the model and each Bot's input fixed and evaluate each Bot three times. Record every output and rationale. Each trial must choose the expected action.

### 3. Live T3 connection and recovery

Verify this phase by connecting an independent external MCP client to T3 v0.1.11. Run the following checks against the live environment.

- Use external OAuth to launch work, retrieve history, send follow-ups with `queue`, and interrupt work. Check project defaults for model and workspace, explicit settings, insufficient privileges, and expired credentials. Verify that an action ID can be used to track a Run even when its ID becomes available later.
- For both initial delegation and a follow-up, restart before delivery starts, immediately after T3 accepts the instruction, and after the result is saved. Redeliver the completion notification as well. Try recovery within the same OAuth session and after signing in again. Verify that each instruction produces one Run and one result ingestion. Zero matches, multiple matches, and inconclusive reconciliation must each result in zero automatic resends.
- Identify a pending question and answer it within the same Run. Also test restarting immediately after acceptance, the user answering first, an identical question in a different Run, and permission approval. Verify zero automatic answers when ownership is ambiguous, zero duplicate answers to resolved questions, and zero additional Runs.
- Reproduce a separate Run started manually in the same Thread and a Session that delegates internally and puts the final report in a later Run. Verify that follow-ups and interruptions target the right work, the final report is associated correctly, and the task counts the result once.

### 4. Completion decisions with live Sessions

- Request the memory usage of a build at a specified commit with updates enabled. Keep the Session's final report identical (“Measurement complete”) and vary only the commit and configuration in the T3 work records. When the Bot can read matching evidence, it should complete the task with zero additional runs. When only an older commit or different configuration is recorded, it should request the missing measurement. Hold the model, completion criteria, work allowance, and records fixed. Compare three conditions using only the final report, retrieving excerpts via `session_read` when needed, and including the same work records in the initial input. Record incorrect completions, unnecessary repeat measurements, input size, and inference count separately. Use the results to choose the history-reading approach for routine use.
- Restart the runtime during a history read and verify that retrieved excerpts and the remaining read allowance carry over. Confirm that reads alone start zero Runs, consume zero additional runs, and complete zero events.
- Replay short and long histories in the same target environment and record the runtime's peak RSS while waiting and while reading. Verify that the runtime does not load the entire history, independently of model input token counts and execution cost.
- In an environment without external verification tools, submit a request such as extracting open items from meeting notes. The Bot should choose completion based on the returned list, and the CLI should show the list and the rationale.
- For a task that needs verification, have cron request a fix for a known bug and require a test that reproduces it first. Run the reproduction test in the delegated Session, save the failure report, and restart Flock. Verify that the Bot requests the fix and a test rerun in the same Thread and can decide completion from the pre-fix and post-fix results. Use the test tools already available in the delegated Session.

v0 supports one local user. The CLI and Web UI operate on the same tasks, questions, and decision history. Verify Web UI interactions using the [UI acceptance criteria](UI.md#acceptance-criteria). Automatic MEMORY updates by Bots and external notifications are future work.
