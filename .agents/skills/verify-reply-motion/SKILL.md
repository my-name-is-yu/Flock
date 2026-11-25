---
name: verify-reply-motion
description: Check queued live conversation refreshes, visibility changes, and catch-up delivery that can animate an older reply after reconnecting or returning to Flock.
---

# Verify reply motion

An eligible foreground live reply animates. A reply found during catch-up stays still even when a live notification is queued in the same batch. Otherwise reconnecting or returning to a tab makes an older reply look new.

Use Python 3.10 or later from the repository root.

```sh
python3 .agents/skills/verify-reply-motion/model.py
python3 .agents/skills/verify-reply-motion/model.py --historical
python3 .agents/skills/verify-reply-motion/model.py --ignore-catch-up
ctest --test-dir build --output-on-failure -R '^(flock_verify_reply_motion.*|flock_web_message_time_dom)$'
```

The model checks a selected conversation with existing history, one new reply, one in-flight read, three requests per queued batch, and one hide and return. It explores stream disconnects and readiness. It assumes the conversation is scrolled to the end, motion is enabled, and no dialog is open. It omits failed HTTP reads, polling, multi-message bursts, and browser timing. The invariant checks the original requests independently of the coalescer's accumulated generation. Chromium checks the animation on the actual reply bubble.

`createLiveUpdates` in `web/assets/live-updates.js` supplies the stream triggers. `coalescedRefresh` collects a pending batch until the busy read returns. `refreshView` and `renderTimeline` in `web/assets/app.js` decide whether to animate after a successful read. `exerciseReplyMotion` in `web/tests/message-time.mjs` supplies stream events and holds a view response at the application's fetch boundary to drive those paths.

Both historical model commands must exit 1 with `A catch-up refresh animated a reply`. `--historical` replays the old unconditional eligibility changed by `b4a4627`. In a scratch copy, set `liveAfterId` in `refreshView` to `context.newestEvent` for every refresh. The browser check must fail with `A reply animated across visibility generations`. `--ignore-catch-up` isolates the lost catch-up trigger. In a scratch copy replace the generation merge with `if (animationGeneration !== null && pending.animationGeneration !== animationGeneration) pending.animationGeneration = null;`. The browser check must fail with `A reply animated during a coalesced disconnect catch-up`.

The last source comparison was at `c08637cc92b132b05966464759d41f936496a7be`. Move that commit forward only after checking the model against the named functions and rerunning all model variants and the browser test. Use `/maintain-formal-verification-skill` when updating this check.
