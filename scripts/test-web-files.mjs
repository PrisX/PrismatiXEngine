import assert from "node:assert/strict";
import {webcrypto, createHash} from "node:crypto";
import test from "node:test";
import {WebFiles, dataCacheName} from "../packages/cli/templates/web/files.mjs";

const base = "https://example.test/game/";
function chunk(text, url) {
  const data = new TextEncoder().encode(text);
  return {data, url, size: data.length, sha256: createHash("sha256").update(data).digest("hex")};
}
function fixture(chunks, cacheAvailable = true) {
  const stored = new Map(), requests = [];
  const cache = {
    async match(url) { return stored.get(url)?.clone(); },
    async put(url, response) { stored.set(url, response.clone()); },
    async delete(url) { return stored.delete(url); },
  };
  const caches = cacheAvailable ? {async open(name) { assert.equal(name, dataCacheName); return cache; }} : undefined;
  const file = {path: "Content.pdx", size: chunks.reduce((sum, value) => sum + value.size, 0), chunks};
  const files = new WebFiles({files: [file]}, base, {
    crypto: webcrypto, caches,
    async fetcher(url) {
      requests.push(url);
      const item = chunks.find(value => new URL(value.url, base).href === url);
      return new Response(item.data);
    },
  });
  return {files, file, stored, requests};
}
test("reads across chunks with independent ranges and bounded downloads", async () => {
  const {files, requests} = fixture([chunk("abcde", "a"), chunk("fghij", "b"), chunk("klmno", "c")]);
  assert.equal(new TextDecoder().decode(await files.read("/game/Content.pdx", 3, 6)), "defghi");
  assert.equal(requests.length, 2);
  assert.equal(new TextDecoder().decode(await files.read("Content.pdx", 0, 2)), "ab");
  assert.equal(requests.length, 2);
  assert.equal((await files.read("Content.pdx", 100, 1)).length, 0);
  assert.equal(files.size("/game/Content.pdx"), 15);
  assert.deepEqual(files.list("/game"), ["Content.pdx"]);
});
test("rejects traversal, missing files and invalid ranges", async () => {
  const {files} = fixture([chunk("abc", "a")]);
  for (const path of ["/game/../Content.pdx", "../Content.pdx", "Content\\pdx", "/persist/save.pxsav"])
    assert.equal(files.size(path), -1);
  await assert.rejects(files.read("missing", 0, 1), /Missing exported resource/);
  await assert.rejects(files.read("Content.pdx", -1, 1), /Invalid resource range/);
  await assert.rejects(files.read("Content.pdx", 0, 0.5), /Invalid resource range/);
});
test("repairs a corrupt persistent chunk, and rejects corrupt network content", async () => {
  const {files, stored, requests} = fixture([chunk("abc", "a")]);
  stored.set(new URL("a", base).href, new Response("xyz"));
  assert.equal(new TextDecoder().decode(await files.read("Content.pdx", 0, 3)), "abc");
  assert.equal(requests.length, 1);
  files.memory.clear(); files.memoryBytes = 0;
  stored.clear(); files.fetcher = async () => new Response("xyz");
  await assert.rejects(files.read("Content.pdx", 0, 3), /checksum mismatch/);
});
test("deduplicates concurrent downloads and persists a complete offline copy", async () => {
  const {files, requests, stored} = fixture([chunk("abc", "a"), chunk("def", "b")]);
  await Promise.all([files.read("Content.pdx", 0, 3), files.read("Content.pdx", 0, 2)]);
  assert.equal(requests.length, 1);
  const progress = [];
  await files.downloadAll((done, total) => progress.push([done, total]));
  assert.deepEqual(progress.at(-1), [6, 6]);
  assert.equal(stored.size, 2);
  files.memory.clear(); files.memoryBytes = 0;
  files.fetcher = async () => { throw new Error("offline"); };
  assert.equal(new TextDecoder().decode(await files.read("Content.pdx", 1, 4)), "bcde");
});
test("keeps online playback available without storage and reports offline storage failure", async () => {
  const {files} = fixture([chunk("abc", "a")], false);
  assert.equal(new TextDecoder().decode(await files.read("Content.pdx", 0, 3)), "abc");
  await assert.rejects(files.downloadAll(), /cannot store an offline copy/);
});
test("maintains exact cursors beyond four GiB without allocating an archive", async () => {
  const {files, file} = fixture([chunk("end", "a")]);
  file.chunks.unshift({size: 2 ** 32, url: "unread", sha256: "unread"});
  file.size += 2 ** 32;
  assert.equal(new TextDecoder().decode(await files.read("Content.pdx", 2 ** 32 + 1, 2)), "nd");
});
test("evicts only memory, preserving immutable offline chunks", async () => {
  const {files, stored} = fixture([chunk("abc", "a"), chunk("def", "b")]);
  files.memoryLimit = 3;
  await files.read("Content.pdx", 0, 6);
  assert.equal(files.memoryBytes, 3);
  assert.equal(stored.size, 2);
});
