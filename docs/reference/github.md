# GitHub integration

For credential setup and review controls, see
[Connect GitHub and get pull request notifications](../guide/integrations.md#connect-github-and-get-pull-request-notifications).

## Repository access

Repository rules use lowercase `github.com/owner/repo` targets.

| Action | Scope |
| --- | --- |
| `repository.read` | Repository reads |
| `repository.pr` | Pull request changes |
| `repository.review` | Reviews |
| `repository.issues` | Issue changes |

Creating a PR from a different head repository also requires read access to that
repository. GitHub credentials and [installation policy](installation-policy.md) both
apply.

## Signed webhooks

Forward GitHub JSON deliveries to the Flock loopback listener at
`POST /api/git/webhooks/github.com` with an accepted loopback `Host` header. Preserve
the raw body and the `X-Hub-Signature-256`, `X-GitHub-Delivery`, and `X-GitHub-Event`
headers.

Subscribe to `pull_request` events to track changes to watched branches. For additional
notifications, subscribe as needed to `pull_request_review`,
`pull_request_review_comment`, `issue_comment` for PR comments, `check_run`,
`check_suite`, `workflow_job`, `workflow_run`, `status`, and `code_scanning_alert`.

Flock verifies the body's HMAC-SHA-256 signature and deduplicates by host and delivery
ID. For a valid delivery it returns HTTP 202 with `status` of `received`, `duplicate`,
or `ignored`. It handles check and workflow events when they complete.

Events with a matching watch go to the owning Bot, which needs `repository.read` on the
repository. A workflow job without a PR number is matched by commit SHA using the watch
and recorded PR synchronization events. An event matched only by SHA must identify a
single watch for delivery.

## Automatic follow-ups

When a Bot opens a PR at a person's request, it can ask the Session that prepared the PR
to fix a failing check or address actionable review feedback on the same branch. The Bot
cites comments and logs as evidence and reports whether it sent the request. The normal
Session-send rules apply, so Supervised mode can require approval in Flock.

The Bot makes up to two automatic fix requests per PR between messages from the original
requester. When it reaches that limit, it reports the problem and asks how to proceed.
Other watched PRs receive reports.

The Bot's `fixPullRequests` setting defaults to `true`. Setting it to `false` disables
automatic fix requests and keeps reports.

## PR creation recovery

If the host response to PR creation is lost, Flock keeps the operation. A retry with the
same exact repository and branch pair checks for the original PR across all states. If
the PR's identity remains unresolved, Flock adds a reconciliation item to the Decision
Inbox so the requester can link a verified candidate or reject the operation. A
definitive host rejection allows a fresh request.
