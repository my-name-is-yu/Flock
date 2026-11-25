# Installation policy

Flock starts in **Full**. Bots can use configured resources unless a **Never**
rule applies. **Supervised** asks for approval for actions with external
effects according to the defaults below. Bot ownership, workspace isolation,
and upstream credentials apply in both modes.

Targets match exactly or with a `*` glob. **Never** takes precedence in both
modes. Supervised then checks **Ask first**, **Allow**, and the category
defaults. Rules saved from Web settings, the CLI, or a card persist until an
installation administrator removes them. See
[permissions and approvals](../guide/flock-settings.md#permissions-and-approvals)
for the controls.

| Action | Supervised default |
| --- | --- |
| Repository writes, PR changes, reviews, and issue or PR comments | Ask |
| Channel posts outside the originating conversation, published artifacts, and email sends | Ask |
| T3 Session launch or a send outside the originating Thread | Ask |
| Bot creation or rename, and schedule, reminder, or trigger changes | Ask |
| Bot private memory edits (`memory.write`, targeting the Bot ID) | Allow |
| Distinct destructive operations or secret use at a new destination | Ask |
| MCP tool calls | Allow when discovery returns `annotations.readOnlyHint: true` for the configured server and exact tool within two minutes. Ask otherwise |
| Reads, workspace file access, and replies, reactions, edits, and attachments in the originating conversation | Allow |

Session read and T3 send rules use compact JSON targets such as
`["environment-id","thread-id"]`. Adapters retain secret values and exclude
them from card previews.

## Approval cards

An action that asks appears in the Decision Inbox and alongside the originating
conversation message, when available. The requester or an installation
administrator can choose **Allow once** or **Deny**. Denial blocks another
attempt until a new message from a person. **Allow for this Flock** resumes the
action and saves an exact Allow rule for the installation. This requires an
installation administrator.

Background cards go to the schedule's Bot owner or T3 connector person and
expire after 30 minutes. An installation administrator can choose **Allow next
time for this Flock** on an expired card to save a rule for future requests.
Work requested by a person stays open. Flock records and denies an action that
requires a card when no eligible requester exists. Changing modes closes
pending approvals for the previous mode.

Messages from linked Slack or iMessage senders can initiate work that needs
approval. Flock posts a waiting notice if channel replies are allowed. The
requester or installation root answers in Flock. Channel messages cannot answer
approvals or save rules.

## Saved task operations

Saved operations check the task's Bot, conversation scope, and current
generation. `change_task` also checks the current revision. A follow-up to a
saved destination validates its recorded environment, project, gateway, action,
and Run, then applies the normal T3 send policy and the saved gateway's T3
Operate permission. The authorized route is the saved gateway. A linked
target can use that route without its own Flock connection record.

## C++ adapter contract

See [`include/flock/policy.hpp`](../../include/flock/policy.hpp) for
resource operations. The transport must establish the calling Run's identity.
At the execution boundary, pass the authenticated Session environment and
Run reference to `Policy::executeSessionResource`, with the exact scope, the
upstream permission check, and the action. Flock resolves the owning task
and checks current policy before executing and journaling the action. The
callback receives the verified task ID to resolve further resource operations.
A peer task and its Runs resolve to the recipient Bot.

For secrets, call `Policy::useSecret` with the owning task, named `SecretRef`,
canonical destination, and stable operation key. An adapter can retry the
same key after a pending approval is granted. Flock passes the secret through
the callback only when the operation is admitted.
