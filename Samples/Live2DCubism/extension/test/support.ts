// A stand-in for the Player's raw drawing host that enforces the rules this
// sample has to live with (docs/native-extensions.md and ScriptRawGraphics.cpp):
// vertices before indices, indices inside the vertices, 64 uploads a frame.

import {closeSync, openSync, readSync, writeSync} from "node:fs";
import {execFileSync, spawn, type ChildProcess} from "node:child_process";
import {mkdtempSync, rmSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";

import type {MeshLike, MeshOptionsLike, RawLike, TextureSourceLike} from "../src/renderer.ts";

export const MAX_UPLOADS_PER_FRAME = 64;

export class FakeMesh implements MeshLike {
  readonly id: string;
  options: MeshOptionsLike = {};
  vertexCount = 0;
  vertexData = new Float32Array(0);
  indexData = new Uint16Array(0);
  destroyed = false;
  private readonly raw: FakeRaw;

  constructor(raw: FakeRaw, id: string, options: MeshOptionsLike) {
    this.raw = raw;
    this.id = id;
    this.options = {...options};
  }

  update(options: MeshOptionsLike): boolean {
    this.options = {...this.options, ...options};
    return true;
  }

  vertices(data: Float32Array): void {
    if (data.length % 8 !== 0) throw new TypeError("vertices hold 8 floats each");
    this.raw.upload();
    this.vertexCount = data.length / 8;
    this.vertexData = data.slice();
    // Like the Player: indices left over from more vertices are dropped.
    if (this.indexData.some((index) => index >= this.vertexCount)) this.indexData = new Uint16Array(0);
  }

  indices(data: Uint16Array | Uint32Array): void {
    if (data.length % 3 !== 0) throw new TypeError("indices describe triangles");
    for (const index of data) if (index >= this.vertexCount) throw new RangeError(`PXJS7802: index ${index} is past the mesh's ${this.vertexCount} vertices`);
    this.raw.upload();
    this.indexData = Uint16Array.from(data);
  }

  destroy(): boolean {
    this.destroyed = true;
    return this.raw.meshes.delete(this.id);
  }
}

export class FakeRaw implements RawLike {
  readonly meshes = new Map<string, FakeMesh>();
  uploadsThisFrame = 0;
  maxUploadsInAFrame = 0;

  beginFrame(): void {
    this.uploadsThisFrame = 0;
  }

  upload(): void {
    this.uploadsThisFrame += 1;
    this.maxUploadsInAFrame = Math.max(this.maxUploadsInAFrame, this.uploadsThisFrame);
    if (this.uploadsThisFrame > MAX_UPLOADS_PER_FRAME) throw new RangeError("PXJS7801: more than 64 uploads in a frame");
  }

  createMesh(id: string, options: MeshOptionsLike = {}): MeshLike {
    const mesh = new FakeMesh(this, id, options);
    this.meshes.set(id, mesh);
    return mesh;
  }

  asset(path: string): TextureSourceLike {
    return {kind: "asset", path};
  }

  /** Meshes that draw, in draw order. */
  drawn(): FakeMesh[] {
    return [...this.meshes.values()]
      .filter((mesh) => mesh.options.visible === true && mesh.vertexCount > 0)
      .sort((a, b) => (a.options.order ?? 0) - (b.options.order ?? 0));
  }
}

const here = process.env.LIVE2D_NATIVE_BUILD ?? "";

/** The library and tools `tools/test.sh` builds; tests that need them skip when it did not run. */
export const nativeBuild = here === "" ? null : {
  directory: here,
  plugin: process.env.LIVE2D_PLUGIN ?? "",
  core: join(here, process.platform === "darwin" ? "libmock_cubism_core.dylib" : "libmock_cubism_core.so"),
  harness: join(here, "plugin_harness"),
};

const SERVICES = "live2d.core.load,live2d.model.create,live2d.model.update,live2d.model.release";

/**
 * The real plugin library behind a blocking call, like `native.request` in the
 * Player: a child process (native/test/plugin_harness.c) loads it and two FIFOs
 * carry the frames.
 */
export class HarnessNative {
  private readonly directory = mkdtempSync(join(tmpdir(), "px-live2d-"));
  private readonly child: ChildProcess;
  private readonly requestFd: number;
  private readonly responseFd: number;

  constructor(pluginPath: string, harnessPath: string) {
    const requests = join(this.directory, "requests");
    const responses = join(this.directory, "responses");
    execFileSync("mkfifo", [requests, responses]);
    this.child = spawn(harnessPath, [pluginPath, SERVICES, requests, responses], {stdio: ["ignore", "ignore", "inherit"]});
    // The harness opens the request FIFO first, then the response FIFO.
    this.requestFd = openSync(requests, "w");
    this.responseFd = openSync(responses, "r");
  }

  private readExactly(size: number): Uint8Array {
    const bytes = Buffer.alloc(size);
    for (let done = 0; done < size;) {
      const count = readSync(this.responseFd, bytes, done, size - done, null);
      if (count === 0) throw new Error("the plugin harness closed its end");
      done += count;
    }
    return new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  request(service: string, payload: Uint8Array): Uint8Array {
    const name = Buffer.from(service, "utf8");
    const frame = Buffer.alloc(4 + name.length + 4 + payload.length);
    frame.writeUInt32LE(name.length, 0);
    name.copy(frame, 4);
    frame.writeUInt32LE(payload.length, 4 + name.length);
    Buffer.from(payload.buffer, payload.byteOffset, payload.byteLength).copy(frame, 8 + name.length);
    for (let done = 0; done < frame.length;) done += writeSync(this.requestFd, frame, done);
    const header = this.readExactly(8);
    const status = new DataView(header.buffer, header.byteOffset, 8).getUint32(0, true);
    const length = new DataView(header.buffer, header.byteOffset, 8).getUint32(4, true);
    const body = this.readExactly(length);
    if (status !== 0) throw new Error(`PXJS7902: the service '${service}' failed (status ${status})`);
    return body;
  }

  close(): void {
    closeSync(this.requestFd);
    closeSync(this.responseFd);
    this.child.kill();
    rmSync(this.directory, {recursive: true, force: true});
  }
}
