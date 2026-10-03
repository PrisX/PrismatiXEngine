import {createServer} from "node:http";
import {createReadStream} from "node:fs";
import {stat} from "node:fs/promises";
import {resolve, join, relative, extname, isAbsolute, sep} from "node:path";
import {pathToFileURL} from "node:url";

export function webExportServer(directory, overrides = new Map()) {
  const root = resolve(directory);
  const types = {".html": "text/html; charset=utf-8", ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8", ".wasm": "application/wasm", ".json": "application/json"};
  return createServer(async (request, response) => {
    try {
      const pathname = decodeURIComponent(new URL(request.url, "http://localhost").pathname);
      const path = join(root, pathname === "/" ? "index.html" : pathname);
      const within = relative(root, path);
      if (within === ".." || within.startsWith(`..${sep}`) || isAbsolute(within)) throw new Error("Invalid path");
      const override = overrides.get(pathname);
      const file = override ? {size: override.length} : await stat(path);
      if (!override && !file.isFile()) throw new Error("Not a file");
      response.writeHead(200, {"Content-Type": types[extname(path)] ?? "application/octet-stream",
        "Content-Length": file.size, "Cache-Control": pathname.startsWith("/_data/") ? "public, max-age=31536000, immutable" : "no-cache",
        "Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp"});
      if (request.method === "HEAD") response.end();
      else if (override) response.end(override);
      else createReadStream(path).pipe(response);
    } catch { response.writeHead(404); response.end("Not found"); }
  });
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  if (!process.argv[2]) throw new Error("Usage: node scripts/serve-web-export.mjs <export-directory> [port]");
  const server = webExportServer(process.argv[2]);
  server.listen(Number(process.argv[3] ?? 8080), "127.0.0.1", () =>
    console.log(`Web game: http://127.0.0.1:${server.address().port}/`));
}
