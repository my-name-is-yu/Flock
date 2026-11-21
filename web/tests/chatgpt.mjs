import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { appendFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { delimiter, join } from "node:path";
import { fileURLToPath } from "node:url";

const script = fileURLToPath(import.meta.url);
const blockers = "Summarize the release blockers.";
const report = "The signing certificate expired on 18 November.";
const consultation = "Ask Verifier to confirm the release criteria.";
const peerReply = "Publish only after the certificate is renewed.";
const why = "Why is the release blocked?";
const renewal = "When will the signing certificate be renewed?";
const notes = "Prepare release notes.";
const checksums = "Check artifact checksums in the backup environment.";

function call(name, fields, reason) {
  return { type: "function_call", namespace: "flock", status: "completed", name,
    arguments: JSON.stringify({ ...fields, reason }) };
}

function route(event) {
  const decide = fields => call("route_conversation", { routes: [fields] }, "Route the user's request.");
  switch (event.text) {
    case blockers:
    case consultation:
    case notes:
      return decide({ kind: "create_task" });
    case checksums:
      assert.ok(event.environments.includes("backup"));
      return decide({ kind: "create_task", environment: "backup" });
    case why:
    case renewal: {
      const source = event.results.find(result => result.request === blockers);
      assert.equal(source?.text, report);
      return decide({ kind: "answer_result", resultMessageId: source.messageId });
    }
    case "それ止めて": {
      const active = event.tasks.filter(task => task.state === "running");
      assert.deepEqual(active.map(task => task.request).sort(), [checksums, notes].sort());
      return decide({ kind: "clarify", operation: "cancel_task", text: "どの作業を停止しますか？",
        candidates: active.map(task => task.taskId) });
    }
    default:
      assert.fail(`Unexpected composer message ${event.text}`);
  }
}

function output({ task, event }) {
  switch (event.kind) {
    case "route_conversation":
      return route(event);
    case "answer_conversation_result":
      assert.equal(event.source.text, report);
      if (event.text === why) {
        return call("answer_conversation_result", { text: "The release is blocked by the expired signing certificate." },
          "The report identifies the expired certificate.");
      }
      assert.equal(event.text, renewal);
      return call("clarify_conversation_result", { text: "What is the planned certificate renewal date?" },
        "The report only gives the expiration date.");
    case "intake":
      if (task.request === consultation) {
        assert.deepEqual(task.peers, [{ botId: "verifier", name: "Verifier" }]);
        return call("send_bot_message", { botId: "verifier", kind: "consult", text: "What are the release criteria?" },
          "Verifier owns the release criteria.");
      }
      assert.ok([blockers, notes, checksums].includes(task.request));
      return call("delegate", { instruction: task.request, doneWhen: "Return the findings and their evidence." },
        "A Session must check the release artifacts.");
    case "result":
      assert.equal(event.report, report);
      return call("complete", { report }, "The Session identified the blocker and the expiration date.");
    case "bot_request":
      assert.equal(task.botId, "verifier");
      assert.equal(event.text, "What are the release criteria?");
      return call("reply_to_bot", { kind: "result", text: peerReply }, "The release criterion is available.");
    case "bot_result":
      assert.equal(event.fromBotId, "verifier");
      assert.equal(event.text, peerReply);
      return call("complete", { report: peerReply }, "Verifier confirmed the release criterion.");
    default:
      assert.fail(`Unexpected model event ${event.kind}`);
  }
}

function mockResponses() {
  globalThis.fetch = async (url, options) => {
    try {
      assert.equal(String(url), "https://api.openai.com/v1/responses");
      assert.equal(options.headers.authorization, "Bearer fixture-access");
      const request = JSON.parse(options.body);
      assert.equal(request.model, "fixture-model");
      assert.equal(request.stream, true);
      const namespace = request.tools[0];
      assert.equal(namespace.name, "flock");
      const result = output(JSON.parse(request.input[0].content));
      const tool = namespace.tools.find(tool => tool.name === result.name);
      assert.ok(tool, `Response tool ${result.name} was missing from the production request`);
      const args = JSON.parse(result.arguments);
      assert.ok(tool.parameters.required.every(key => key in args));
      assert.ok(Object.keys(args).every(key => key in tool.parameters.properties));
      if (result.name === "route_conversation") {
        const segment = tool.parameters.properties.routes.items.properties;
        assert.ok(segment.kind.enum.includes(args.routes[0].kind));
        assert.ok(Object.keys(args.routes[0]).every(key => key in segment));
        assert.match(request.instructions, /omit environment to use the Bot's configured default/);
      }
      const response = { type: "response.completed", response: { output: [result] } };
      return new Response(`data: ${JSON.stringify(response)}\n\n`, { headers: { "content-type": "text/event-stream" } });
    } catch (error) {
      appendFileSync(process.env.FLOCK_WEB_TEST_ERRORS, `${error.stack}\n`);
      throw error;
    }
  };
}

function run(executable) {
  const home = mkdtempSync(join(tmpdir(), "flock-web-chatgpt-"));
  try {
    const credentials = join(home, "credentials", "chatgpt");
    mkdirSync(credentials, { recursive: true, mode: 0o700 });
    writeFileSync(join(credentials, "profile.json"), JSON.stringify({
      subject: "fixture", accessToken: "fixture-access", refreshToken: "fixture-refresh",
      granted: ["chatgpt.tokens.use.direct"], expiresAt: Date.now() + 3600_000,
    }), { mode: 0o600 });
    const writeBot = (id, config) => {
      const dir = join(home, "bots", id);
      mkdirSync(dir, { recursive: true });
      writeFileSync(join(dir, "config.json"), JSON.stringify({
        modelProvider: "chatgpt", model: "fixture-model", environmentId: "local-env", projectId: id, ...config,
      }));
    };
    writeBot("release", { name: "Release", allowedBots: ["verifier"], conversationEnvironments: { backup: "backup-env" } });
    writeBot("verifier", { name: "Verifier" });

    // The production launcher invokes node through PATH and strips NODE_OPTIONS.
    const bin = join(home, "bin");
    mkdirSync(bin);
    writeFileSync(join(bin, "node"), '#!/bin/sh\nexec "$FLOCK_WEB_TEST_NODE" --import "$FLOCK_WEB_TEST_PRELOAD" "$@"\n',
      { mode: 0o700 });
    const errors = join(home, "response-errors.log");
    const result = spawnSync(executable, [home], {
      stdio: "inherit",
      timeout: 25_000,
      env: { ...process.env, PATH: [bin, process.env.PATH].filter(Boolean).join(delimiter),
        FLOCK_WEB_TEST_NODE: process.execPath, FLOCK_WEB_TEST_PRELOAD: script, FLOCK_WEB_TEST_ERRORS: errors },
    });
    if (existsSync(errors)) assert.fail(readFileSync(errors, "utf8"));
    if (result.error) throw result.error;
    assert.equal(result.status, 0, "Web ChatGPT regression failed");
  } finally {
    rmSync(home, { recursive: true, force: true });
  }
}

if (realpathSync(process.argv[1]) === script) {
  if (Number(process.versions.node.split(".")[0]) < 22) process.exit(77);
  run(process.argv[2]);
} else {
  mockResponses();
}
