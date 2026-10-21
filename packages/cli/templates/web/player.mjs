import createPlayer from "./prismatix-player.mjs";
import {WebFiles} from "./files.mjs";
import {WebVideo} from "./video.mjs";
import {WebEffects} from "./effects.mjs";
import {WebAccessibility} from "./accessibility.mjs";

const $ = id => document.getElementById(id);
const base = new URL("./", import.meta.url);
let module, manifest, running = false;
const error = value => { console.error(value); $("status").textContent = value.message ?? String(value); };
function abort(reason) {
  const problem = new Error(`遊戲執行中止：${reason}`);
  const service = module?.prismatix;
  service?.flushRequested?.reject(problem); service?.flushActive?.reject(problem);
  if (service) { service.flushRequested = null; service.flushActive = null; }
  running = false;
  error(problem);
}
function sync(populate) {
  return new Promise((resolve, reject) => module.FS.syncfs(populate,
    problem => problem ? reject(problem) : resolve()));
}
async function flushPersistence() {
  if (!running) return sync(false);
  const service=module.prismatix;
  const existing=service.flushRequested??service.flushActive;
  const pending=existing??{};
  if(!existing){
    pending.promise=new Promise((resolve,reject)=>{pending.resolve=resolve;pending.reject=reject;});
    service.flushRequested=pending;
  }
  if(!await pending.promise)throw new Error("無法儲存目前的設定與進度");
}
const encode = bytes => {
  let binary = "";
  for (let i = 0; i < bytes.length; i += 16384)
    binary += String.fromCharCode(...bytes.subarray(i, i + 16384));
  return btoa(binary);
};
function saveFiles(path = "/persist") {
  return module.FS.readdir(path).filter(name => name !== "." && name !== "..").flatMap(name => {
    const full = `${path}/${name}`, info = module.FS.stat(full);
    if (module.FS.isDir(info.mode)) return saveFiles(full);
    return /(?:\.pxsav|\/profile\.dat|\/config\.dat)$/u.test(full) ? [full] : [];
  });
}
async function boot() {
  const response = await fetch(new URL("web-export.json", base));
  if (!response.ok) throw new Error(`無法載入遊戲資訊 (${response.status})`);
  manifest = await response.json();
  if (manifest.format !== "PrismatiXWebExport" || manifest.schemaRevision !== 1)
    throw new Error("不相容的 Web 匯出格式");
  document.title = $("title").textContent = manifest.title;
  document.documentElement.lang = manifest.locale ?? "und";
  // A WebGPU export has no WebGL fallback inside it; its author exports a
  // WebGL2 build for browsers without WebGPU.
  if (manifest.renderer === "webgpu" && !(await navigator.gpu?.requestAdapter().catch(() => null)))
    throw new Error("此遊戲的網頁版使用 WebGPU，但這個瀏覽器無法使用 WebGPU。請改用支援 WebGPU 的瀏覽器。");
  const accessibility = new WebAccessibility($("semantics"), manifest.locale ?? "und");
  const files = new WebFiles(manifest, base);
  const video = new WebVideo(manifest, base, resume => {
    $("resume").hidden = false;
    $("resume").onclick = () => { $("resume").hidden = true; resume(); $("canvas").focus(); };
  }, error);
  // The media service worker serves byte ranges from immutable chunks. This
  // also supplies seekable video while offline without loading a whole film.
  if ("serviceWorker" in navigator) {
    const registration = await navigator.serviceWorker.register(new URL("sw.mjs", base), {type: "module", scope: base.href, updateViaCache: "none"});
    const offerUpdate = () => {
      if (!registration.waiting || !navigator.serviceWorker.controller) return;
      $("update").hidden = false;
      $("update").onclick = async () => {
        try {
          await flushPersistence();
          navigator.serviceWorker.addEventListener("controllerchange", () => location.reload(), {once: true});
          registration.waiting?.postMessage("activate-update");
        } catch(problem){error(problem);}
      };
    };
    offerUpdate();
    registration.addEventListener("updatefound", () => registration.installing?.addEventListener("statechange", offerUpdate));
    await navigator.serviceWorker.ready;
    if (!navigator.serviceWorker.controller)
      await new Promise(resolve => navigator.serviceWorker.addEventListener("controllerchange", resolve, {once: true}));
  } else if (Object.keys(manifest.videos).length) throw new Error("此瀏覽器需要 Service Worker 才能播放遊戲影片");
  module = await createPlayer({canvas: $("canvas"), noInitialRun: true,
    locateFile: path => new URL(path, base).href,
    print: text => console.log(text), printErr: text => console.error(text),
    onAbort: abort,
    prismatix: {files, video, effects: new WebEffects(manifest, files, base), accessibility, error,
      stopped: code => { running = false; $("welcome").hidden = false;
        const service = module.prismatix;
        const problem = new Error("遊戲已結束，請重新開始後再操作存檔");
        service.flushRequested?.reject(problem); service.flushActive?.reject(problem);
        service.flushRequested = null; service.flushActive = null;
        $("start").textContent = "重新開始"; $("start").onclick = () => location.reload();
        if (code) error(new Error(`遊戲結束，錯誤代碼 ${code}`)); }}});
  module.FS.mkdirTree("/game"); module.FS.mkdirTree("/persist");
  module.FS.mount(module.FS.filesystems.IDBFS, {}, "/persist");
  await sync(true);
  module.FS.chdir("/game");
  // SDL_GetPrefPath stays game-specific through the native persistence helper.
  module.ENV.PRISMATIX_PLAYER_DATA_ROOT = "/persist";
  // A live game queues imports for the next boot, before native settings,
  // profile and slot readers open their files.
  const pendingRequest = indexedDB.open(`prismatix-save-import-${manifest.gameId}`, 1);
  pendingRequest.onupgradeneeded = () => pendingRequest.result.createObjectStore("pending");
  const pendingDatabase = await new Promise((resolve, reject) => {
    pendingRequest.onsuccess = () => resolve(pendingRequest.result);
    pendingRequest.onerror = () => reject(pendingRequest.error);
  });
  const pending = await new Promise((resolve, reject) => {
    const request = pendingDatabase.transaction("pending").objectStore("pending").get("files");
    request.onsuccess = () => resolve(request.result); request.onerror = () => reject(request.error);
  });
  if (pending) {
    for (const entry of pending) {
      module.FS.mkdirTree(entry.path.slice(0, entry.path.lastIndexOf("/")));
      module.FS.writeFile(entry.path, entry.bytes);
    }
    await sync(false);
    await new Promise((resolve, reject) => {
      const transaction = pendingDatabase.transaction("pending", "readwrite");
      transaction.objectStore("pending").delete("files");
      transaction.oncomplete = resolve; transaction.onerror = () => reject(transaction.error);
    });
  }
  pendingDatabase.close();
  $("start").disabled = false;
  $("export-saves").disabled = $("import-saves").disabled = false;
  $("start").onclick = () => {
    if (running) return;
    video.unlock().catch(error);
    $("welcome").hidden = true; $("canvas").focus(); running = true;
    try { module.callMain([]); }
    catch (problem) { if (problem !== "unwind") error(problem); }
  };
  $("download").disabled = !globalThis.caches;
  $("download").onclick = async () => {
    $("download").disabled = true; $("download-progress").hidden = false;
    try {
      await navigator.storage?.persist?.();
      await files.downloadAll((done, total) => {
        $("download-progress").max = total; $("download-progress").value = done;
        $("download-status").textContent = `下載 ${Math.round(done / total * 100)}%`;
      });
      $("download-status").textContent = "完整遊戲已儲存，可離線遊玩。";
    } catch (problem) { error(problem); $("download-status").textContent = "離線下載尚未完成。"; }
    finally { $("download").disabled = false; }
  };
  $("export-saves").onclick = async () => {
    try {
      await flushPersistence();
      const data = {format: "PrismatiXWebSaves", schemaRevision: 1, gameId: manifest.gameId,
        files: saveFiles().map(path => ({path: path.slice(9), bytes: encode(module.FS.readFile(path))}))};
      const url = URL.createObjectURL(new Blob([JSON.stringify(data)], {type: "application/json"}));
      const anchor = document.createElement("a");
      anchor.href = url; anchor.download = `${manifest.gameId}.pxwebsave`; anchor.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (problem) { error(problem); }
  };
  $("import-saves").onchange = async event => {
    try {
      const file = event.target.files[0];
      if (!file || file.size > 128 * 1024 * 1024) throw new Error("存檔檔案超過 128 MiB");
      const data = JSON.parse(await file.text());
      if (data.format !== "PrismatiXWebSaves" || data.schemaRevision !== 1 || data.gameId !== manifest.gameId ||
          !Array.isArray(data.files) || data.files.length > 10000) throw new Error("存檔屬於不同遊戲或格式不相容");
      const prepared = data.files.map(entry => {
        if (typeof entry.path !== "string" || !/^Game [a-f0-9]{64}\/Save\/(?:save_\d+\.pxsav|autosave\.pxsav|profile\.dat|config\.dat)$/u.test(entry.path))
          throw new Error("存檔路徑不合法");
        return {path: `/persist/${entry.path}`, bytes: Uint8Array.from(atob(entry.bytes), c => c.charCodeAt(0))};
      });
      const gameHash = [...new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(manifest.gameId)))]
        .map(value => value.toString(16).padStart(2, "0")).join("");
      if (prepared.some(entry => !entry.path.startsWith(`/persist/Game ${gameHash}/Save/`)))
        throw new Error("存檔資料識別與此遊戲不符");
      // Stop the page's game before replacing its saved state, so it cannot
      // overwrite imported slots on a subsequent frame or shutdown.
      if (running) {
        await flushPersistence();
        const request = indexedDB.open(`prismatix-save-import-${manifest.gameId}`, 1);
        request.onupgradeneeded = () => request.result.createObjectStore("pending");
        const database = await new Promise((resolve, reject) => {
          request.onsuccess = () => resolve(request.result); request.onerror = () => reject(request.error);
        });
        await new Promise((resolve, reject) => {
          const transaction = database.transaction("pending", "readwrite");
          transaction.objectStore("pending").put(prepared, "files");
          transaction.oncomplete = resolve; transaction.onerror = () => reject(transaction.error);
        });
        database.close(); location.reload(); return;
      }
      for (const entry of prepared) {
        module.FS.mkdirTree(entry.path.slice(0, entry.path.lastIndexOf("/")));
        module.FS.writeFile(entry.path, entry.bytes);
      }
      await sync(false); location.reload();
    } catch (problem) { error(problem); }
  };
}
$("fullscreen").onclick = async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await document.querySelector("main").requestFullscreen();
  } catch (problem) { error(problem); }
};
$("canvas").addEventListener("contextmenu", event => event.preventDefault());
boot().catch(error);
