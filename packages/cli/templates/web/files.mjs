// PDX streams keep independent cursors in C++. Only the requested immutable
// chunks enter memory, so archives and whole games may exceed WASM's address space.
export const dataCacheName = "prismatix-web-data-v1";

export class WebFiles {
  constructor(manifest, base, {fetcher = globalThis.fetch, caches = globalThis.caches,
      crypto = globalThis.crypto, onProgress = () => {}} = {}) {
    this.manifest = manifest;
    this.base = new URL(base);
    this.fetcher = (...args) => fetcher(...args);
    this.caches = caches;
    this.crypto = crypto;
    this.onProgress = onProgress;
    this.files = new Map(manifest.files.map(file => [file.path, file]));
    this.pending = new Map();
    this.memory = new Map();
    this.memoryBytes = 0;
    this.memoryLimit = 16 * 1024 * 1024;
  }
  normalize(path) {
    if (path.startsWith("/game/")) path = path.slice(6);
    else if (path === "/game" || path === "/game/" || path === ".") path = "";
    else if (path.startsWith("./")) path = path.slice(2);
    if (path.startsWith("/") || path.includes("\\") || path.includes("\0") ||
        path.split("/").some(part => part === "." || part === "..")) return null;
    return path;
  }
  size(path) { return this.files.get(this.normalize(path))?.size ?? -1; }
  list(directory) {
    const normalized = this.normalize(directory);
    if (normalized === null) return null;
    const prefix = normalized ? normalized.replace(/\/$/u, "") + "/" : "";
    const names = [...this.files.keys()].filter(path => path.startsWith(prefix))
      .map(path => path.slice(prefix.length)).filter(path => !path.includes("/"));
    return names.length || [...this.files.keys()].some(path => path.startsWith(prefix))
      ? names.sort() : null;
  }
  async verified(response, chunk) {
    if (!response.ok) throw new Error(`Resource download failed (${response.status})`);
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (bytes.length !== chunk.size) throw new Error("Resource chunk has the wrong size");
    const hash = [...new Uint8Array(await this.crypto.subtle.digest("SHA-256", bytes))]
      .map(value => value.toString(16).padStart(2, "0")).join("");
    if (hash !== chunk.sha256) throw new Error("Resource chunk checksum mismatch");
    return bytes;
  }
  async chunk(chunk) {
    const url = new URL(chunk.url, this.base).href;
    const hit = this.memory.get(url);
    if (hit) { this.memory.delete(url); this.memory.set(url, hit); return hit; }
    if (this.pending.has(url)) return this.pending.get(url);
    const pending = (async () => {
      let cache = null;
      if (this.caches) {
        try { cache = await this.caches.open(dataCacheName); }
        catch (error) { this.cacheError = error; }
      }
      let bytes;
      const cached = await cache?.match(url);
      if (cached) {
        try { bytes = await this.verified(cached, chunk); }
        catch { await cache.delete(url); }
      }
      if (!bytes) {
        bytes = await this.verified(await this.fetcher(url), chunk);
        // Offline support must report storage failures instead of claiming a
        // complete download. Normal online play may continue without a cache.
        if (cache) {
          try { await cache.put(url, new Response(bytes)); }
          catch (error) { this.cacheError = error; }
        }
        this.onProgress(chunk.size);
      }
      this.memory.set(url, bytes);
      this.memoryBytes += bytes.length;
      while (this.memoryBytes > this.memoryLimit && this.memory.size > 1) {
        const oldest = this.memory.keys().next().value;
        this.memoryBytes -= this.memory.get(oldest).length;
        this.memory.delete(oldest);
      }
      return bytes;
    })();
    this.pending.set(url, pending);
    try { return await pending; }
    finally { this.pending.delete(url); }
  }
  async read(path, offset, count) {
    const file = this.files.get(this.normalize(path));
    if (!file) throw new Error(`Missing exported resource: ${path}`);
    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(count) || offset < 0 || count < 0)
      throw new Error("Invalid resource range");
    if (offset >= file.size || count === 0) return new Uint8Array();
    const output = new Uint8Array(Math.min(count, file.size - offset));
    let start = 0, written = 0;
    for (const chunk of file.chunks) {
      const end = start + chunk.size;
      if (end > offset && start < offset + output.length) {
        const bytes = await this.chunk(chunk);
        const first = Math.max(0, offset - start);
        const last = Math.min(bytes.length, offset + output.length - start);
        output.set(bytes.subarray(first, last), written);
        written += last - first;
      }
      start = end;
      if (written === output.length) break;
    }
    if (written !== output.length) throw new Error("Exported resource has an incomplete chunk index");
    return output;
  }
  async downloadAll(onProgress = () => {}, signal) {
    if (!this.caches) throw new Error("This browser cannot store an offline copy");
    this.cacheError = null;
    const chunks = new Map(this.manifest.files.flatMap(file =>
      file.chunks.map(chunk => [chunk.url, chunk])));
    const total = [...chunks.values()].reduce((sum, chunk) => sum + chunk.size, 0);
    let done = 0;
    for (const chunk of chunks.values()) {
      signal?.throwIfAborted();
      await this.chunk(chunk);
      if (this.cacheError) throw this.cacheError;
      // Verify that memory hits have also reached persistent storage.
      const cache = await this.caches.open(dataCacheName);
      const key = new URL(chunk.url, this.base).href;
      if (!await cache.match(key)) await cache.put(key, new Response(await this.chunk(chunk)));
      done += chunk.size;
      onProgress(done, total);
    }
  }
}
