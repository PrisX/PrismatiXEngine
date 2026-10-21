import {spawn} from "node:child_process";
import {stat} from "node:fs/promises";
import {extname, join, resolve} from "node:path";
import {discoverRuntimeTool, type RuntimeDiscoveryOptions} from "./runtime-discovery.js";
import {distributionDataRoot} from "./distribution-layout.js";

export interface SaveInspectionOptions {
  readonly path: string;
  readonly packagePath?: string;
  readonly secret?: string;
  readonly json?: boolean;
  readonly runtime?: RuntimeDiscoveryOptions;
  readonly timeoutMs?: number;
}
export interface SaveInspectionResult {
  /** Preserve native numeric spelling/uint64 precision. Do not parse and reserialize. */
  readonly output: string;
  readonly source: string;
}
const maximumSaveBytes = 64 * 1024 * 1024;
const maximumReplyBytes = 64 * 1024 * 1024;
const maximumDiagnosticBytes = 64 * 1024;

/**
 * The native Player reads a package manifest file. Accept what an author has
 * at hand: a distribution folder (<dir>/Package/manifest.json), its Package
 * folder (<dir>/manifest.json), or the manifest path itself.
 */
export async function resolvePackageManifest(path: string): Promise<string> {
  const target = resolve(path);
  const info = await stat(target).catch(() => undefined);
  if (info === undefined) throw new Error(`--package ${path} does not exist`);
  if (info.isFile()) return target;
  const data = await distributionDataRoot(target);
  for (const candidate of [join(data, "Package", "manifest.json"), join(target, "manifest.json")]) {
    if ((await stat(candidate).catch(() => undefined))?.isFile()) return candidate;
  }
  throw new Error(`--package ${path} has no Package/manifest.json or manifest.json; ` +
    "pass a distribution folder, its Package folder, or the manifest file");
}

/** Native Player owns save authentication, schema and runtime-state validation. */
export async function inspectSaveWithRuntime(options: SaveInspectionOptions): Promise<SaveInspectionResult> {
  const path = resolve(options.path);
  const info = await stat(path);
  if (!info.isFile() || info.size === 0 || info.size > maximumSaveBytes)
    throw new Error("Save inspection requires a nonempty file of at most 64 MiB");
  const player = await discoverRuntimeTool("player", options.runtime ?? {});
  const args = ["--inspect-save", path];
  if (options.packagePath !== undefined) args.push("--package", await resolvePackageManifest(options.packagePath));
  if (options.secret !== undefined) args.push("--secret", options.secret);
  if (options.json) args.push("--json");
  const output = await new Promise<string>((accept, reject) => {
    const script = [".js", ".mjs", ".cjs"].includes(extname(player.path).toLowerCase());
    const child = spawn(script ? process.execPath : player.path,
      script ? [player.path, ...args] : args,
      {stdio: ["ignore", "pipe", "pipe"], windowsHide: true});
    const chunks: Buffer[] = [];
    let length = 0;
    let diagnostics = Buffer.alloc(0);
    let failure: Error | undefined;
    let escalation: NodeJS.Timeout | undefined;
    const stop = (error: Error): void => {
      failure ??= error;
      child.kill();
      escalation ??= setTimeout(() => child.kill("SIGKILL"), 1000);
    };
    const interrupt = (): void => stop(new Error("Save inspection was interrupted"));
    process.once("SIGINT", interrupt); process.once("SIGTERM", interrupt);
    const timer = setTimeout(() => stop(new Error("Native save inspection timed out")), options.timeoutMs ?? 30_000);
    const cleanup = (): void => {
      clearTimeout(timer); if (escalation) clearTimeout(escalation);
      process.removeListener("SIGINT", interrupt); process.removeListener("SIGTERM", interrupt);
    };
    child.stdout.on("data", (chunk: Buffer) => {
      if (failure) return;
      length += chunk.length;
      if (length > maximumReplyBytes) { chunks.length = 0; stop(new Error("Native save inspection output exceeds 64 MiB")); return; }
      chunks.push(chunk);
    });
    child.stderr.on("data", (chunk: Buffer) => {
      diagnostics = Buffer.concat([diagnostics, chunk]).subarray(-maximumDiagnosticBytes);
    });
    child.once("error", error => { cleanup(); reject(error); });
    child.once("close", (code, signal) => {
      cleanup();
      if (failure) { reject(failure); return; }
      if (code !== 0) {
        reject(new Error(`Native save inspection failed (${code ?? signal ?? "unknown"}): ${diagnostics.toString("utf8").trim()}`));
        return;
      }
      try {
        const text = new TextDecoder("utf-8", {fatal: true}).decode(Buffer.concat(chunks, length)).trim();
        // Check the transport, without changing native numeric lexemes. This is
        // intentionally not a second, divergent implementation of SaveSystem.
        const response: unknown = JSON.parse(text);
        if (response === null || typeof response !== "object" || Array.isArray(response))
          throw new Error("Native save inspection did not return a JSON object");
        accept(`${text}\n`);
      } catch (error) { reject(new Error("Native save inspection returned invalid JSON", {cause: error})); }
    });
  });
  return {output, source: player.source};
}
