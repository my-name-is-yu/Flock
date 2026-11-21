# Flock's local Web UI

The first GUI is a local Web UI that works alongside standard T3 Code. Open it in a standalone browser or in a T3 browser tab. The running C++ runtime continues accepting scheduled requests and monitoring Sessions after you close the UI. If users frequently need to see when a Bot is waiting for input while they work in a different T3 Thread, consider integrating the UI into T3 as a dedicated tab.

## Home and Bot switching

Home shows the Bot list alongside the conversation with the selected Bot. The day after a task finishes, select the same Bot to read the result and request the next task. Grok Bot's list and the way Devin Bots connect conversations, items waiting for input, and work Sessions inform the design. [Grok Bot UI design](https://x.ai/news/designing-grok-bot) · [Devin Bots usage guide](https://docs.devin.ai/work-with-devin/bots)

| Area | Display and controls |
| --- | --- |
| Bot list on the left | Each row shows an avatar, a name, and the latest question or report on one line. Use the row menu to pin frequently used Bots as small avatars above the list. Unpinning a Bot returns it to a regular row. |
| “Waiting for input” below the list | Shows the items that need user action for the selected Bot and the total count. Select an item to see the related request, why it is waiting, and the available actions. |
| “Sessions” below “Waiting for input” | Shows the T3 Sessions the selected Bot started for its tasks, along with each request and status. “Open in T3” opens the work history for the corresponding Thread. |
| Conversation in the center | Shows manual and scheduled requests, Bot questions, user answers, completion reports, and decision rationales in chronological order. Each entry identifies the relevant task. Select the avatar at the top to open the Bot settings and see its role and scheduled requests. |

Switching Bots updates “Waiting for input,” “Sessions,” and the conversation together. In a narrow T3 panel, the Bot list collapses and can be opened from the conversation. The UI shows the history of multiple tasks in sequence, while the Bot receives only the history of the task it is currently evaluating.

## New requests and replies to questions

In the usual case, the input at the bottom of the conversation sends a **new request** to the selected Bot. If another task is already running in the same project, the UI shows which task must finish first.

Answer a Bot's question by selecting an option or choosing “Write a reply.” The composer shows the question while you enter your reply. Sending or canceling the reply returns the composer to new requests. New request drafts and replies are kept separately so switching Bots preserves the intended recipient.

For example, if Release asks “Should the draft include only user-facing changes?”, “User-facing changes only” answers that question. Sending “Also check the changes in the next version” in the standard composer creates another task.

Each answer is tied to the same wait ID used by the CLI and is accepted only once. If the CLI receives an answer first or T3 resolves the wait, the UI updates the original question and closes the reply composer. Approving extra work and extending a deadline each have a dedicated control. If T3 requires permission approval, “Approve in T3” opens the relevant Thread. [Task states and where work resumes](SPEC.md#task-states-and-where-work-resumes)

## Avatars

Give each Bot a distinct appearance and color with Open Bot Avatar. Use the same avatar in the list, pinned shortcuts, conversation, and settings. The list uses static avatars. The conversation header avatar can animate to reflect states such as the Bot making a decision or its Session working. Show each state in text as well and respect the device's reduced-motion setting. [Open Bot Avatar demo](https://dominikscholz.github.io/open-bot-avatar/) · [Rendering and state specification](https://github.com/DominikScholz/open-bot-avatar#mount-an-avatar)

## Acceptance criteria

- Set up two Bots with separate requests and Sessions. Switch between them from both the list rows and the pinned avatars. Verify that the conversation, waiting items, Sessions, and settings all follow the selection. Also test unpinning and switching Bots in a narrow viewport.
- Send a new request while the same Bot has an unanswered question. Verify that one task is created and the original question remains pending. Reply to that question and verify that only the original task resumes. If the CLI answers first, submitting the reply that was left open in the Web UI must leave the answer processed exactly once.
- Close the UI while the runtime receives a Session result. Reopen the UI and confirm that the result appears in the conversation. Select an item that requires action in T3 and verify that the relevant Thread opens in the correct environment.
