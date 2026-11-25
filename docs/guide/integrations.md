# Connect GitHub, messaging, and local computers

## Connect GitHub and get pull request notifications

1. As an installation administrator, open **Flock settings → Connections → GitHub**.
2. Enter an **Access token** with access to the repositories and operations you want the
   Bot to use. Save it, then choose **Check connection** to check the saved token.
3. For PR notifications, also save a **Webhook secret**. Set the same secret on a
   GitHub JSON webhook and forward its deliveries to Flock using the
   [signed webhook contract](../reference/github.md#signed-webhooks).
4. Restart Flock to apply saved credential changes. To restrict what a Bot can do over
   the connection, add repository rules in
   [Flock settings → Permissions](flock-settings.md#permissions-and-approvals).

In the Bot's **General** settings, turn off **Fix failing checks and review feedback**
to keep PR reports without automatic fix requests. Review any approval for a fix request
in **Needs you**. See
[how PR follow-ups work](../reference/github.md#automatic-follow-ups).

If a PR creation attempt loses its response, ask the Bot to retry the same request. If
an **Unconfirmed pull request** item appears in the Decision Inbox, link a verified PR
or reject the operation. Ask explicitly for a replacement after a confirmed PR closes.
See [PR creation recovery](../reference/github.md#pr-creation-recovery).

For host-managed deployment, the daemon can also take the access token and webhook
secret from `FLOCK_GITHUB_TOKEN` and `FLOCK_GITHUB_WEBHOOK_SECRET`. Saved Web values
override the corresponding environment values.

## Connect Slack or iMessage

Start a provider bridge on the Flock host before registering it. The bridge connects to
the messaging service and translates messages and replies using the
[channel bridge protocol](../reference/channel-bridge.md).

1. As an installation administrator, open **Flock settings → Connections → Slack** or
   **iMessage**. Enter the Slack **Workspace ID** or iMessage **Connection name**, the
   **Local bridge port**, and the **Bridge secret** shared with the provider bridge.
   Choose **Add bridge** and use **Check** to see whether the port responds.
2. Set up the sender's Flock account and share the Bot with that person from its
   **Access** page. See [Flock settings](flock-settings.md).
3. In **Flock settings → Bots**, choose **Access** for the Bot. Under
   **Channel senders**, choose the channel, enter the provider **Sender ID** and Flock
   **Person ID**, then choose **Link sender**.
4. Restart Flock to apply the bridge and sender settings. To control channel use,
   attachment reads, and posting, set rules in
   [Flock settings → Permissions](flock-settings.md#permissions-and-approvals). Use the
   [channel policy targets](../reference/channel-bridge.md#channel-access).

To publish HTML previews in a channel, open
**Flock settings → Connections → Bridge options** and save an HTTPS
**Shared preview URL**. Route only the
[artifact preview path](../reference/channel-bridge.md#html-previews) from that HTTPS
origin to Flock. Restart Flock to apply the change.

## Approve access to a local computer

The installation owner can register the Flock host in
**Flock settings → Connections → Local machines**. Enter a **Machine name** and the
Flock person ID of its owner, then choose **Add machine**. If a trusted Session bridge
will make requests, save its **Session bridge secret** under **Bridge options**.
Restart Flock to apply the settings.

When a Bot requests local file access for a Session, the registered owner reviews the
requested paths in **Flock settings → Bots → Access → Connected computers**. Open
approval links in a browser on the Flock host to reach its loopback listener. Choose
**Approve** or **Decline** on the request. Use **Disconnect** to withdraw an approved
connection. See
[local machine operations](../reference/channel-bridge.md#local-machine-operations) for
the Session bridge and C++ adapter contracts.

Set up reusable T3 execution environments in the [Bot's settings](bots.md).

## Load channel and local machine settings from the host

For a host-managed installation, set `FLOCK_CHANNEL_BRIDGES_FILE` to a host-controlled
JSON file before starting `flock run`. The Web UI shows these settings and sender links
as host-managed. Each `credentialEnv` and `sessionBridgeCredentialEnv` names an
environment variable holding a secret. See
[credential requirements](../reference/channel-bridge.md#channel-access).

```json
{
  "slack": {
    "T123": {
      "bridgePort": 3410,
      "credentialEnv": "SLACK_BRIDGE_KEY",
      "senders": {"U123": {"personId": "alice", "botId": "release"}}
    }
  },
  "imessage": {
    "messages-on-mac": {
      "bridgePort": 3411,
      "credentialEnv": "IMESSAGE_BRIDGE_KEY",
      "senders": {"linked-sender": {"personId": "alice", "botId": "release"}}
    }
  },
  "artifactBaseUrl": "https://previews.example.com",
  "localMachines": {"laptop": "alice"},
  "sessionBridgeCredentialEnv": "FLOCK_SESSION_BRIDGE_KEY"
}
```
