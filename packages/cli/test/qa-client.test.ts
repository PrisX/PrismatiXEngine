import assert from "node:assert/strict";
import {mkdtemp, rm, writeFile} from "node:fs/promises";
import {createServer, type AddressInfo, type Socket} from "node:net";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";

import {PlayerTestClient} from "../src/qa-client.js";
import {bootFailure, loadJourney} from "../src/test-command.js";

// A stand-in Player: records requests and answers each with `answer`.
async function fakePlayer(answer: (request: Record<string, unknown>) => unknown) {
  const requests: Record<string, unknown>[] = [];
  const sockets: Socket[] = [];
  const server = createServer((socket) => {
    sockets.push(socket);
    let buffer = "";
    socket.setEncoding("utf8");
    socket.on("data", (chunk: string) => {
      buffer += chunk;
      for (let newline = buffer.indexOf("\n"); newline >= 0; newline = buffer.indexOf("\n")) {
        const request = JSON.parse(buffer.slice(0, newline)) as Record<string, unknown>;
        buffer = buffer.slice(newline + 1);
        requests.push(request);
        socket.write(`${JSON.stringify({id: request.id, ok: true, result: answer(request)})}\n`);
      }
    });
  });
  await new Promise<void>((accept) => server.listen(0, "127.0.0.1", accept));
  const port = (server.address() as AddressInfo).port;
  return {port, requests, close: () => {
    for (const socket of sockets) socket.destroy();
    server.close();
  }};
}

test("advanceUntil sends one request with the condition, timeout and line limit", async () => {
  const player = await fakePlayer((request) => request.op === "advanceUntil"
    ? {appState: "game", advancedLines: 7} : {});
  const client = await PlayerTestClient.connect(player.port, tmpdir());
  try {
    const reached = await client.advanceUntil({textContains: "雨"}, {maxLines: 50});
    assert.equal(reached.advancedLines, 7);
    await client.advanceUntil({choices: true});
    await client.save(2);
    const [first, second, save] = player.requests;
    assert.deepEqual({...first, id: undefined}, {id: undefined, op: "advanceUntil",
      condition: {textContains: "雨"}, timeoutMs: 120_000, maxLines: 50});
    assert.deepEqual(second?.condition, {choices: true});
    assert.equal(second?.maxLines, undefined);
    assert.deepEqual({...save, id: undefined}, {id: undefined, op: "save", slot: 2});
  } finally {
    client.close();
    player.close();
  }
});

test("tap sends a right click only when asked", async () => {
  const player = await fakePlayer(() => ({}));
  const client = await PlayerTestClient.connect(player.port, tmpdir());
  try {
    await client.tap(640, 360);
    await client.tap(10, 20, {button: "secondary"});
    const [left, right] = player.requests;
    assert.deepEqual({...left, id: undefined}, {id: undefined, op: "tap", x: 640, y: 360});
    assert.deepEqual({...right, id: undefined}, {id: undefined, op: "tap", x: 10, y: 20, button: "secondary"});
  } finally {
    client.close();
    player.close();
  }
});

test("a journey may set its own time limit", async () => {
  const scratch = await mkdtemp(join(tmpdir(), "prismatix-journey-"));
  try {
    await writeFile(join(scratch, "long.mjs"),
      "export const timeoutMs = 600000;\nexport default async function () {}\n");
    await writeFile(join(scratch, "plain.mjs"), "export default async function () {}\n");
    await writeFile(join(scratch, "bad.mjs"),
      "export const timeoutMs = '10 minutes';\nexport default async function () {}\n");
    assert.equal((await loadJourney(join(scratch, "long.mjs"), scratch)).timeoutMs, 600_000);
    assert.equal((await loadJourney(join(scratch, "plain.mjs"), scratch)).timeoutMs, undefined);
    await assert.rejects(loadJourney(join(scratch, "bad.mjs"), scratch), /timeoutMs must be/u);
  } finally { await rm(scratch, {recursive: true, force: true}); }
});

test("a Player that could not start is reported by its PXPLAYER5031 line and the stack under it", () => {
  const log = [
    "[2026-10-22 12:59:51.263] [info] [58452] Player boot: engine.ready",
    "[2026-10-22 12:59:51.264] [error] [58452] [diagnostic] PXJS7202 [Script.JavaScript] handler failed",
    "[2026-10-22 12:59:51.265] [critical] [58452] [diagnostic] PXPLAYER5031 [Player.Boot] The game could not start" +
      " — PXJS7202 [Script.JavaScript] JavaScript event handler failed (boom) — Error: boom",
    "    at <anonymous> (Content/Extensions/boom/main.js:360:13)",
    "",
    "[2026-10-22 12:59:51.315] [info] [58452] Runtime shut down.",
  ].join("\r\n");
  assert.equal(bootFailure(log), "PXPLAYER5031 [Player.Boot] The game could not start — PXJS7202 " +
    "[Script.JavaScript] JavaScript event handler failed (boom) — Error: boom\n" +
    "    at <anonymous> (Content/Extensions/boom/main.js:360:13)");
  assert.equal(bootFailure("[2026-10-22 12:59:51.315] [info] [1] Runtime shut down.\n"), undefined);
});
