#!/usr/bin/env node

import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {access, mkdtemp, readFile, rm} from "node:fs/promises";
import http from "node:http";
import {tmpdir} from "node:os";
import {basename, dirname, extname, join, resolve} from "node:path";

const modulePath = resolve(process.argv[2] ?? "");
assert.ok(process.argv[2],
  "Usage: node scripts/test-wasm-preview-browser.mjs <prismatix-preview.mjs> [browser]");
await access(modulePath);
const root = dirname(modulePath);
const moduleName = basename(modulePath);
const explicitBrowser = process.argv[3] ?? process.env.PRISMATIX_BROWSER;

async function exists(path) {
  try { await access(path); return true; } catch { return false; }
}

async function findBrowser() {
  if (explicitBrowser) return explicitBrowser;
  const candidates = process.platform === "win32" ? [
    join(process.env.PROGRAMFILES ?? "", "Google/Chrome/Application/chrome.exe"),
    join(process.env["PROGRAMFILES(X86)"] ?? "", "Microsoft/Edge/Application/msedge.exe"),
    join(process.env.LOCALAPPDATA ?? "", "Google/Chrome/Application/chrome.exe"),
  ] : process.platform === "darwin" ? [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
  ] : ["/usr/bin/google-chrome", "/usr/bin/google-chrome-stable",
       "/usr/bin/chromium", "/usr/bin/chromium-browser"];
  for (const candidate of candidates) if (await exists(candidate)) return candidate;
  throw new Error("Chrome/Chromium was not found; pass its path as the second argument");
}

const html = `<!doctype html><meta charset="utf-8">
<canvas id="canvas" width="640" height="360" tabindex="0"></canvas>
<script type="module">
const canvas = document.querySelector("#canvas");
globalThis.__surface = {pointer: 0, keyboard: 0};
canvas.addEventListener("pointerdown", () => globalThis.__surface.pointer++);
canvas.addEventListener("mousedown", () => globalThis.__surface.pointer++);
canvas.addEventListener("keydown", () => globalThis.__surface.keyboard++);
canvas.focus();
try {
  const createModule = (await import("./${moduleName}")).default;
  const module = await createModule({canvas, locateFile: file => new URL(file, location.href).href});
  const create = module.cwrap("px_preview_create", "number", ["string"]);
  const resize = module.cwrap("px_preview_resize", "number", ["number", "number", "number", "number"]);
  const tick = module.cwrap("px_preview_tick", "number", ["number", "number"]);
  const drain = module.cwrap("px_preview_drain_events", "string", ["number"]);
  const destroy = module.cwrap("px_preview_destroy", null, ["number"]);
  const handle = create(JSON.stringify({canvasSelector: "#canvas", width: 640, height: 360,
    logicalWidth: 640, logicalHeight: 360}));
  if (!handle) throw new Error("px_preview_create returned no handle");
  resize(handle, 640, 360, 1);
  tick(handle, 1 / 60);
  globalThis.__moduleReady = true;
  globalThis.__finishSmoke = async () => {
    tick(handle, 1 / 60);
    const events = JSON.parse(drain(handle));
    const Audio = globalThis.AudioContext ?? globalThis.webkitAudioContext;
    if (!Audio) throw new Error("Web Audio is unavailable");
    const audio = new Audio();
    const oscillator = audio.createOscillator();
    oscillator.start(); oscillator.stop();
    const result = {ok: true, width: canvas.width, height: canvas.height,
      pointer: globalThis.__surface.pointer, keyboard: globalThis.__surface.keyboard,
      audioState: audio.state, eventCount: events.length,
      crashed: events.some(event => event?.type === "crashed")};
    await audio.close();
    destroy(handle);
    return result;
  };
} catch (error) {
  globalThis.__smokeError = String(error?.stack ?? error);
}
</script>`;

const server = http.createServer(async (request, response) => {
  try {
    if (request.url === "/" || request.url === "/index.html") {
      response.writeHead(200, {"content-type": "text/html; charset=utf-8"});
      response.end(html); return;
    }
    const name = decodeURIComponent((request.url ?? "").slice(1));
    assert.ok(name === basename(name), "nested paths are not served");
    const path = join(root, name);
    const body = await readFile(path);
    const type = extname(path) === ".wasm" ? "application/wasm" : "text/javascript";
    response.writeHead(200, {"content-type": type, "cache-control": "no-store"});
    response.end(body);
  } catch (error) {
    response.writeHead(404); response.end(String(error));
  }
});
await new Promise((accept) => server.listen(0, "127.0.0.1", accept));
const address = server.address();
assert.ok(address && typeof address === "object");
const pageUrl = `http://127.0.0.1:${address.port}/`;

const profile = await mkdtemp(join(tmpdir(), "prismatix-browser-smoke-"));
const browser = spawn(await findBrowser(), [
  "--headless=new", "--no-sandbox", "--disable-dev-shm-usage",
  "--enable-webgl", "--use-gl=angle", "--use-angle=swiftshader",
  "--autoplay-policy=no-user-gesture-required", "--remote-debugging-port=0",
  `--user-data-dir=${profile}`, "about:blank",
], {stdio: ["ignore", "pipe", "pipe"], windowsHide: true});
let browserLog = "";
browser.stderr.setEncoding("utf8");
browser.stderr.on("data", value => { browserLog += value; });

async function waitFor(condition, label, timeoutMs = 30_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const value = await condition();
    if (value) return value;
    await new Promise(accept => setTimeout(accept, 50));
  }
  throw new Error(`Timed out waiting for ${label}\n${browserLog}`);
}

let socket;
try {
  const endpoint = await waitFor(() => {
    const match = /DevTools listening on (ws:\/\/[^\s]+)/u.exec(browserLog);
    return match?.[1];
  }, "browser DevTools endpoint");
  const endpointUrl = new URL(endpoint);
  const created = await fetch(
    `http://${endpointUrl.host}/json/new?${encodeURIComponent(pageUrl)}`,
    {method: "PUT"});
  if (!created.ok) throw new Error(await created.text());
  const target = await created.json();
  socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((accept, reject) => {
    socket.addEventListener("open", accept, {once: true});
    socket.addEventListener("error", reject, {once: true});
  });
  let nextId = 1;
  const pending = new Map();
  socket.addEventListener("message", event => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      const {accept, reject} = pending.get(message.id); pending.delete(message.id);
      if (message.error) reject(new Error(JSON.stringify(message.error)));
      else accept(message.result);
    }
  });
  const command = (method, params = {}) => new Promise((accept, reject) => {
    const id = nextId++; pending.set(id, {accept, reject});
    socket.send(JSON.stringify({id, method, params}));
  });
  const evaluate = async expression => {
    const result = await command("Runtime.evaluate", {expression, awaitPromise: true,
      returnByValue: true});
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  await command("Runtime.enable");
  await command("Page.enable");
  await waitFor(async () => {
    const error = await evaluate("globalThis.__smokeError");
    if (error) throw new Error(error);
    return await evaluate("globalThis.__moduleReady === true");
  }, "WASM Preview initialization");
  await command("Input.dispatchMouseEvent", {type: "mousePressed", x: 40, y: 40,
    button: "left", clickCount: 1});
  await command("Input.dispatchMouseEvent", {type: "mouseReleased", x: 40, y: 40,
    button: "left", clickCount: 1});
  await command("Input.dispatchKeyEvent", {type: "rawKeyDown", key: "Enter",
    code: "Enter", windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13});
  await command("Input.dispatchKeyEvent", {type: "keyUp", key: "Enter",
    code: "Enter", windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13});
  const result = await evaluate("globalThis.__finishSmoke()");
  assert.equal(result.ok, true);
  assert.equal(result.width, 640);
  assert.equal(result.height, 360);
  assert.ok(result.pointer > 0, "canvas did not receive browser pointer input");
  assert.ok(result.keyboard > 0, "canvas did not receive browser keyboard input");
  assert.equal(result.crashed, false, "WASM Preview emitted a crash event");
  assert.ok(["running", "suspended", "closed"].includes(result.audioState));
  console.log("PrismatiX WASM Preview browser canvas/audio/input smoke passed");
} finally {
  socket?.close();
  const exited = browser.exitCode === null
    ? new Promise(accept => browser.once("exit", accept)) : Promise.resolve();
  browser.kill();
  await Promise.race([exited, new Promise(accept => setTimeout(accept, 5_000))]);
  await new Promise(accept => server.close(accept));
  await rm(profile, {recursive: true, force: true, maxRetries: 10, retryDelay: 100});
}
