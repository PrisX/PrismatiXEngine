import {WebFiles} from "./files.mjs";
const base = new URL("./", import.meta.url);
const exportId = "__PRISMATIX_EXPORT_ID__";
const shellName = `prismatix-web-shell-${base.pathname}-${exportId}`;
let manifestPromise;
async function manifest() {
  return manifestPromise ??= (async () => {
    const cache = await caches.open(shellName);
    const key = new URL("web-export.json", base);
    const response = await cache.match(key) ?? await fetch(key, {cache: "no-store"});
    if (!response.ok) throw new Error("Web export manifest could not be downloaded");
    const data = await response.json();
    if (data.exportId !== exportId) throw new Error("Web export changed while downloading; reload to update");
    return data;
  })().catch(error => { manifestPromise=undefined;throw error; });
}
self.addEventListener("install", event => event.waitUntil((async () => {
  const data = await manifest();
  const cache = await caches.open(shellName);
  await cache.addAll(data.shell.map(path => new URL(path, base).href));
  const cached=await(await cache.match(new URL("web-export.json",base))).json();
  if(cached.exportId!==exportId){await caches.delete(shellName);throw new Error("Export changed during shell installation");}
})()));
self.addEventListener("activate", event => event.waitUntil(self.clients.claim()));
// A running game retains its shell/version until the player elects to reload.
self.addEventListener("message", event => {
  if (event.data === "activate-update") event.waitUntil(self.skipWaiting());
});
async function currentManifest() {
  try { return await manifest(); }
  catch {
    const cached = await (await caches.open(shellName)).match(new URL("web-export.json", base));
    if (!cached) throw new Error("Game has not been cached for offline use");
    return cached.json();
  }
}
async function respond(request) {
  const url = new URL(request.url);
  if (url.origin !== base.origin || !url.pathname.startsWith(base.pathname)) return fetch(request);
  const path = decodeURIComponent(url.pathname.slice(base.pathname.length));
  if (path.startsWith("_data/")) return fetch(request);
  const data = await currentManifest();
  const shell = await (await caches.open(shellName)).match(new URL(path || "./", base));
  if (shell) return shell;
  const file = data.files.find(file => file.path === path);
  if (file) {
    const range = request.headers.get("range");
    let first = 0, last = file.size - 1;
    if (range) {
      const match = /^bytes=(\d*)-(\d*)$/u.exec(range);
      if (!match || (!match[1] && !match[2])) return new Response(null, {status: 416, headers: {"Content-Range": `bytes */${file.size}`}});
      if (!match[1]) first = Math.max(0, file.size - Number(match[2]));
      else { first = Number(match[1]); if (match[2]) last = Math.min(last, Number(match[2])); }
      if (!Number.isSafeInteger(first) || !Number.isSafeInteger(last) || first > last || first >= file.size)
        return new Response(null, {status: 416, headers: {"Content-Range": `bytes */${file.size}`}});
    }
    const files = new WebFiles(data, base);
    let cursor = first;
    const stream = new ReadableStream({async pull(controller) {
      try {
        if (cursor > last) { controller.close(); return; }
        const count = Math.min(1024 * 1024, last - cursor + 1);
        controller.enqueue(await files.read(path, cursor, count)); cursor += count;
      } catch (error) { controller.error(error); }
    }});
    const headers = {"Accept-Ranges": "bytes", "Content-Length": String(last - first + 1),
      "Content-Type": path.endsWith(".mp4") ? "video/mp4" : "application/octet-stream"};
    if (range) headers["Content-Range"] = `bytes ${first}-${last}/${file.size}`;
    return new Response(stream, {status: range ? 206 : 200, headers});
  }
  try { return await fetch(request); }
  catch {
    const cached = await (await caches.open(shellName)).match(request);
    if (cached) return cached;
    throw new Error(`Offline resource is missing: ${path}`);
  }
}
self.addEventListener("fetch", event => {
  if (event.request.method === "GET") event.respondWith(respond(event.request));
});
