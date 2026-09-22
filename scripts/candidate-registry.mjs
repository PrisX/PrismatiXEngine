import {createHash} from "node:crypto";
import {createServer} from "node:http";
import {readFile} from "node:fs/promises";
import {pathToFileURL} from "node:url";

/** A loopback-only, read-only registry of explicitly supplied candidate tarballs. */
export async function startCandidateRegistry(candidates) {
  const packages = new Map();
  for (const {manifest, tarball} of candidates) {
    if (!manifest.name.startsWith("@prismatix/")) throw new Error("Only PrismatiX candidates may enter this registry");
    const bytes = await readFile(tarball);
    packages.set(manifest.name, {manifest, bytes,
      integrity: `sha512-${createHash("sha512").update(bytes).digest("base64")}`,
      shasum: createHash("sha1").update(bytes).digest("hex")});
  }
  let baseUrl;
  const requests = [];
  const server = createServer((request, response) => {
    const pathname = decodeURIComponent(new URL(request.url, "http://127.0.0.1").pathname).slice(1);
    requests.push({method: request.method, path: pathname});
    const tarballRequest = pathname.endsWith("/candidate.tgz");
    const name = tarballRequest ? pathname.slice(0, -"/candidate.tgz".length) : pathname;
    const item = packages.get(name);
    if (request.method !== "GET" || item === undefined) { response.writeHead(404); response.end(); return; }
    if (tarballRequest) {
      response.writeHead(200, {"content-type": "application/octet-stream", "content-length": item.bytes.length});
      response.end(item.bytes); return;
    }
    const {manifest, integrity, shasum} = item;
    const version = {...manifest, dist: {integrity, shasum, tarball: `${baseUrl}/${manifest.name}/candidate.tgz`}};
    response.writeHead(200, {"content-type": "application/json"});
    response.end(JSON.stringify({name, "dist-tags": {latest: manifest.version}, versions: {[manifest.version]: version}}));
  });
  await new Promise((accept, reject) => { server.once("error", reject); server.listen(0, "127.0.0.1", accept); });
  baseUrl = `http://127.0.0.1:${server.address().port}`;
  return {url: baseUrl, requests, close: () => new Promise((accept, reject) => server.close((error) => error ? reject(error) : accept()))};
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const config = process.argv[process.argv.indexOf("--config") + 1];
  if (!process.argv.includes("--config")) throw new Error("--config <JSON candidate array> is required");
  const registry = await startCandidateRegistry(JSON.parse(await readFile(config, "utf8")));
  process.stdout.write(`${JSON.stringify({url: registry.url})}\n`);
  for (const signal of ["SIGINT", "SIGTERM"]) process.once(signal, async () => { await registry.close(); });
}
