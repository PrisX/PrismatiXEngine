// The extension registry client: search, look up and resolve extensions,
// download a .pxext with its SHA-256 and size checked, and publish through
// the presigned upload flow. It speaks the HTTP API v1 in
// docs/extensions-sharing.md and also works against a static index (plain
// files on GitHub Pages or any web host), where every route is a file:
// `<route>/index.json`.
//
// There is no default registry: the URL comes from --registry, the
// PRISMATIX_EXTENSION_REGISTRY environment variable or the project's
// `settings["extensions.registry"]`.

import {createHash} from "node:crypto";
import {existsSync} from "node:fs";
import {readFile} from "node:fs/promises";
import {join} from "node:path";

import {
  extensionPackageLimits,
  nativePackBytes,
  parseExtensionPackageId,
  resolveExtensionVersion,
  supportsPlatform,
  validateRegistryDocument,
  type ExtensionPackageValidation,
  type NativeExtensionPlatform,
  type RegistryDocumentKind,
  type RegistryExtension,
  type RegistryPublishResponse,
  type RegistrySearchResponse,
  type RegistrySearchResult,
  type RegistryVersion,
} from "@prismatix/authoring-sdk";

/** The project setting that names the registry. */
export const REGISTRY_SETTING = "extensions.registry";
export const REGISTRY_ENVIRONMENT = "PRISMATIX_EXTENSION_REGISTRY";
export const REGISTRY_TOKEN_ENVIRONMENT = "PRISMATIX_REGISTRY_TOKEN";

export interface RegistryLocation {
  readonly url: string;
  readonly source: "flag" | "environment" | "project";
}

const loopback = new Set(["localhost", "127.0.0.1", "[::1]", "::1"]);

/** A registry, download or upload URL: https, or http on this computer only. */
export function checkedUrl(value: string | URL, base?: string | URL): URL {
  let url: URL;
  try { url = new URL(value, base); } catch { throw new Error(`Not a URL: ${String(value)}`); }
  if (url.protocol === "https:") return url;
  if (url.protocol === "http:" && loopback.has(url.hostname)) return url;
  throw new Error(`Refusing ${url.href}: registries and downloads must use https (http only for localhost)`);
}

function normalizedBase(value: string): string {
  const url = checkedUrl(value);
  url.search = "";
  url.hash = "";
  return url.href.replace(/\/+$/u, "");
}

async function projectSetting(root: string): Promise<string | undefined> {
  for (const name of ["prismatix.json", "project.pxproject"]) {
    const path = join(root, name);
    if (!existsSync(path)) continue;
    try {
      const value = JSON.parse((await readFile(path, "utf8")).replace(/^﻿/u, "")) as {settings?: Record<string, unknown>};
      const setting = value.settings?.[REGISTRY_SETTING];
      return typeof setting === "string" && setting.trim().length > 0 ? setting.trim() : undefined;
    } catch { return undefined; }
  }
  return undefined;
}

/** The configured registry, or undefined when none is set. */
export async function locateRegistry(root: string | undefined, flag?: string,
  environment: NodeJS.ProcessEnv = process.env): Promise<RegistryLocation | undefined> {
  if (flag !== undefined) return {url: normalizedBase(flag), source: "flag"};
  const fromEnvironment = environment[REGISTRY_ENVIRONMENT]?.trim();
  if (fromEnvironment) return {url: normalizedBase(fromEnvironment), source: "environment"};
  const fromProject = root === undefined ? undefined : await projectSetting(root);
  return fromProject === undefined ? undefined : {url: normalizedBase(fromProject), source: "project"};
}

export const noRegistryMessage = "No extension registry is configured. Set one with --registry <url>, the " +
  `${REGISTRY_ENVIRONMENT} environment variable, or \`prismatix extension registry <url>\` (the project's ` +
  `settings["${REGISTRY_SETTING}"] in prismatix.json). Files (./x.pxext), https URLs and npm:<package> install without one.`;

export async function requireRegistry(root: string | undefined, flag?: string): Promise<RegistryLocation> {
  const location = await locateRegistry(root, flag);
  if (location === undefined) throw new Error(noRegistryMessage);
  return location;
}

export class RegistryRequestError extends Error {
  constructor(message: string, readonly status: number, readonly code?: string) {
    super(message);
    this.name = "RegistryRequestError";
  }
}

export interface RegistryClientOptions {
  readonly fetch?: typeof fetch;
  readonly token?: string;
}

/** A document from the registry and the URL it came from (relative URLs in it resolve against that). */
export interface Located<T> {
  readonly value: T;
  readonly url: string;
  /** True when it came from a static index file. */
  readonly static: boolean;
}

const maxDocumentBytes = 8 * 1024 * 1024;

async function readLimited(response: Response, limit: number, what: string): Promise<Uint8Array> {
  const declared = Number(response.headers.get("content-length") ?? "NaN");
  if (Number.isFinite(declared) && declared > limit) throw new Error(`${what} is larger than ${limit} bytes`);
  if (response.body === null) return new Uint8Array();
  const chunks: Uint8Array[] = [];
  let total = 0;
  const reader = response.body.getReader();
  for (;;) {
    const {done, value} = await reader.read();
    if (done) break;
    total += value.length;
    if (total > limit) { await reader.cancel(); throw new Error(`${what} is larger than ${limit} bytes`); }
    chunks.push(value);
  }
  const bytes = new Uint8Array(total);
  let at = 0;
  for (const chunk of chunks) { bytes.set(chunk, at); at += chunk.length; }
  return bytes;
}

function errorBody(text: string): {code: string; message: string} | undefined {
  try {
    const value = JSON.parse(text) as unknown;
    const result = validateRegistryDocument<{error: {code: string; message: string}}>("error", value);
    return result.valid ? result.value!.error : undefined;
  } catch { return undefined; }
}

function sha256(bytes: Uint8Array): string {
  return createHash("sha256").update(bytes).digest("hex");
}

function idPath(id: string): string {
  const parsed = parseExtensionPackageId(id);
  if (parsed === undefined) throw new Error(`Not a registry extension id (owner/name, lowercase): ${id}`);
  return `${encodeURIComponent(parsed.owner)}/${encodeURIComponent(parsed.name)}`;
}

export class RegistryClient {
  readonly base: string;
  readonly #fetch: typeof fetch;
  readonly #token: string | undefined;

  constructor(base: string, options: RegistryClientOptions = {}) {
    this.base = normalizedBase(base);
    this.#fetch = options.fetch ?? fetch;
    this.#token = options.token;
  }

  async #request(url: string, init: RequestInit = {}): Promise<Response> {
    try { return await this.#fetch(url, {redirect: "follow", ...init}); }
    catch (cause) { throw new Error(`Could not reach ${url}: ${cause instanceof Error ? cause.message : String(cause)}`); }
  }

  async #json<T>(response: Response, url: string, kind: RegistryDocumentKind): Promise<T> {
    const text = new TextDecoder().decode(await readLimited(response, maxDocumentBytes, url));
    let value: unknown;
    try { value = JSON.parse(text.replace(/^﻿/u, "")); }
    catch { throw new Error(`${url} did not answer with JSON`); }
    const result = validateRegistryDocument<T>(kind, value, url);
    if (!result.valid)
      throw new Error(`${url} is not a registry ${kind} document: ${result.diagnostics.map((item) => item.details ?? item.message).join("; ")}`);
    return result.value!;
  }

  async #failure(response: Response, url: string): Promise<never> {
    const text = new TextDecoder().decode(await readLimited(response, 64 * 1024, url).catch(() => new Uint8Array()));
    const body = errorBody(text);
    throw new RegistryRequestError(body === undefined ? `${url} answered ${response.status} ${response.statusText}`.trim()
      : `${body.message} (${body.code}, ${response.status})`, response.status, body?.code);
  }

  /**
   * GETs a /v1 route. A 404 without a JSON error body, or an HTML page (a
   * static host listing the route's folder), means a static index, so the
   * route's `index.json` file is tried next.
   */
  async get<T>(route: string, kind: RegistryDocumentKind, query?: URLSearchParams): Promise<Located<T>> {
    const url = `${this.base}${route}${query === undefined || [...query].length === 0 ? "" : `?${query}`}`;
    const response = await this.#request(url, {headers: {accept: "application/json"}});
    const html = /^text\/html\b/iu.test(response.headers.get("content-type") ?? "");
    if (response.ok && !html) return {value: await this.#json<T>(response, url, kind), url: response.url || url, static: false};
    if (!response.ok && response.status !== 404) return this.#failure(response, url);
    const text = new TextDecoder().decode(await readLimited(response, 64 * 1024, url).catch(() => new Uint8Array()));
    const body = errorBody(text);
    if (body !== undefined) throw new RegistryRequestError(`${body.message} (${body.code}, 404)`, 404, body.code);
    const fallback = `${this.base}${route}/index.json`;
    const file = await this.#request(fallback, {headers: {accept: "application/json"}});
    if (file.ok) return {value: await this.#json<T>(file, fallback, kind), url: file.url || fallback, static: true};
    if (file.status === 404) throw new RegistryRequestError(`Not found on ${this.base}: ${route}`, 404, "not_found");
    return this.#failure(file, fallback);
  }

  /** Search; a static index is filtered here the way the search route filters. */
  async search(options: {readonly query?: string; readonly engine?: string; readonly capability?: string;
    readonly platform?: string; readonly page?: number}): Promise<RegistrySearchResponse> {
    const query = new URLSearchParams();
    if (options.query) query.set("q", options.query);
    if (options.engine) query.set("engine", options.engine);
    if (options.capability) query.set("capability", options.capability);
    if (options.platform) query.set("platform", options.platform);
    if (options.page !== undefined && options.page > 1) query.set("page", String(options.page));
    const found = await this.get<RegistrySearchResponse>("/v1/search", "searchResponse", query);
    const absolute = (result: RegistrySearchResult): RegistrySearchResult =>
      result.icon === undefined ? result : {...result, icon: new URL(result.icon, found.url).href};
    if (!found.static) return {...found.value, results: found.value.results.map(absolute)};
    const words = (options.query ?? "").toLowerCase().split(/\s+/u).filter(Boolean);
    const matches = found.value.results.filter((result) => {
      const text = [result.id, result.displayName, result.description, ...result.keywords ?? []].join(" ").toLowerCase();
      // A static index lists each extension's latest version only, so it
      // is not filtered by engine here: installing resolves a compatible one.
      return words.every((word) => text.includes(word)) &&
        (options.capability === undefined || result.capabilities.includes(options.capability)) &&
        (options.platform === undefined || supportsPlatform(result.platforms, options.platform));
    });
    const pageSize = 20;
    const page = Math.max(1, options.page ?? 1);
    return {...(options.query ? {query: options.query} : {}), total: matches.length, page, pageSize,
      results: matches.slice((page - 1) * pageSize, page * pageSize).map(absolute)};
  }

  /** An extension's details; icon and screenshot URLs come back absolute. */
  async extension(id: string): Promise<Located<RegistryExtension>> {
    const found = await this.get<RegistryExtension>(`/v1/extensions/${idPath(id)}`, "extension");
    if (found.value.id !== id) throw new Error(`${found.url} describes ${found.value.id}, not ${id}`);
    const value = {...found.value,
      ...(found.value.icon === undefined ? {} : {icon: new URL(found.value.icon, found.url).href}),
      ...(found.value.screenshots === undefined ? {} : {screenshots: found.value.screenshots.map((shot) => new URL(shot, found.url).href)})};
    return {...found, value};
  }

  async version(id: string, version: string): Promise<Located<RegistryVersion>> {
    const found = await this.get<RegistryVersion>(`/v1/extensions/${idPath(id)}/versions/${encodeURIComponent(version)}`, "version");
    if (found.value.version !== version) throw new Error(`${found.url} describes version ${found.value.version}, not ${version}`);
    return found;
  }

  /**
   * The newest version of `id` that satisfies `range` and runs on `engine`
   * (the resolve route; against a static index, resolved here from the
   * extension's version list by the same rule).
   */
  async resolve(id: string, range: string, engine: string, platform?: string): Promise<Located<RegistryVersion>> {
    const route = `/v1/extensions/${idPath(id)}/resolve`;
    const url = `${this.base}${route}?${new URLSearchParams({range, engine, ...(platform === undefined ? {} : {platform})})}`;
    const response = await this.#request(url, {headers: {accept: "application/json"}});
    if (response.ok) {
      const value = await this.#json<RegistryVersion>(response, url, "version");
      // A registry that ignores `platform` would answer with any version.
      if (platform !== undefined && !supportsPlatform(value.platforms ?? value.manifest.platforms, platform))
        throw new RegistryRequestError(`${id}@${value.version} does not run on ${platform}`, 404, "platform_unsupported");
      return {value, url: response.url || url, static: false};
    }
    if (![404, 405, 501].includes(response.status)) return this.#failure(response, url);
    const text = new TextDecoder().decode(await readLimited(response, 64 * 1024, url).catch(() => new Uint8Array()));
    const body = errorBody(text);
    if (body !== undefined) throw new RegistryRequestError(`${id}@${range}: ${body.message} (${body.code})`, response.status, body.code);
    const details = await this.extension(id);
    const resolved = resolveExtensionVersion(details.value.versions, range, engine, platform);
    if (resolved.version === undefined) throw new RegistryRequestError(resolutionMessage(id, range, engine, resolved, platform),
      404, resolved.problem === "engine" ? "engine_incompatible" : resolved.problem === "platform" ? "platform_unsupported"
        : "no_matching_version");
    return this.version(id, resolved.version.version);
  }

  async #downloadChecked(reference: string, base: string, size: number, expected: string, limit: number): Promise<Uint8Array> {
    const url = checkedUrl(reference, base);
    if (size > limit) throw new Error(`${url.href} is ${size} bytes; at most ${limit} are allowed`);
    const response = await this.#request(url.href);
    checkedUrl(response.url || url.href);
    if (!response.ok) return this.#failure(response, url.href);
    const bytes = await readLimited(response, size, url.href).catch((cause: unknown) => {
      throw new Error(`${url.href} is larger than the ${size} bytes the registry lists; nothing was installed (${String(cause)})`);
    });
    if (bytes.length !== size)
      throw new Error(`${url.href} is ${bytes.length} bytes, not the ${size} the registry lists; nothing was installed`);
    const digest = sha256(bytes);
    if (digest !== expected)
      throw new Error(`${url.href} does not match the SHA-256 the registry lists (got ${digest}, expected ${expected}); nothing was installed`);
    return bytes;
  }

  /** Downloads a version's .pxext and checks its size and SHA-256 against the version document. */
  async download(version: Located<RegistryVersion>): Promise<Uint8Array> {
    return this.#downloadChecked(version.value.download, version.url, version.value.size, version.value.sha256,
      extensionPackageLimits.packageBytes);
  }

  /** Downloads a version's native pack for one platform, checked against the version document. */
  async downloadNative(version: Located<RegistryVersion>, platform: NativeExtensionPlatform): Promise<Uint8Array> {
    const entry = version.value.platformDownloads?.[platform];
    if (entry === undefined)
      throw new Error(`${version.value.id ?? "The extension"}@${version.value.version} has no ${platform} native pack on the registry`);
    return this.#downloadChecked(entry.download, version.url, entry.size, entry.sha256, nativePackBytes);
  }

  #authorization(): Record<string, string> {
    if (this.#token === undefined || this.#token.length === 0)
      throw new Error(`Set ${REGISTRY_TOKEN_ENVIRONMENT} to your registry token to publish; it is never stored in the project`);
    return {authorization: `Bearer ${this.#token}`};
  }

  /**
   * Publishes a validated package: POST /v1/extensions announces it and gets
   * a presigned upload URL, the bytes go there with PUT (without the token),
   * and POST /v1/uploads/{uploadId}/confirm makes the version public.
   */
  async publish(bytes: Uint8Array, validation: ExtensionPackageValidation,
    onProgress?: (message: string) => void,
    nativePacks: ReadonlyMap<NativeExtensionPlatform, Uint8Array> = new Map()): Promise<Located<RegistryVersion>> {
    const authorization = this.#authorization();
    if (!validation.valid || validation.metadata === undefined || validation.manifest === undefined)
      throw new Error("Only a valid package can be published");
    const platformPackages = Object.fromEntries([...nativePacks].map(([platform, data]) =>
      [platform, {sha256: sha256(data), size: data.length}]));
    const request = {id: validation.metadata.id, version: validation.metadata.version!, sha256: validation.sha256,
      size: validation.size, manifest: validation.manifest, metadata: validation.metadata,
      ...(nativePacks.size === 0 ? {} : {platformPackages})};
    const announce = `${this.base}/v1/extensions`;
    onProgress?.(`Announcing ${request.id}@${request.version} to ${this.base}`);
    const started = await this.#request(announce, {method: "POST", body: JSON.stringify(request),
      headers: {...authorization, "content-type": "application/json", accept: "application/json"}});
    if (!started.ok) return this.#failure(started, announce);
    const upload = await this.#json<RegistryPublishResponse>(started, announce, "publishResponse");
    const target = checkedUrl(upload.uploadUrl, started.url || announce);
    onProgress?.(`Uploading ${bytes.length} bytes`);
    const headers: Record<string, string> = {"content-type": "application/octet-stream", ...upload.uploadHeaders ?? {}};
    for (const name of Object.keys(headers)) if (name.toLowerCase() === "authorization") delete headers[name];
    const put = await this.#request(target.href, {method: "PUT", body: bytes as Uint8Array<ArrayBuffer>, headers});
    if (!put.ok) return this.#failure(put, target.href);
    for (const [platform, data] of nativePacks) {
      const slot = upload.platformUploads?.[platform];
      if (slot === undefined) throw new Error(`${announce} gave no upload for the ${platform} native pack; this registry may not support native extensions`);
      const destination = checkedUrl(slot.uploadUrl, started.url || announce);
      const packHeaders: Record<string, string> = {"content-type": "application/octet-stream", ...slot.uploadHeaders ?? {}};
      for (const name of Object.keys(packHeaders)) if (name.toLowerCase() === "authorization") delete packHeaders[name];
      onProgress?.(`Uploading the ${platform} native pack (${data.length} bytes)`);
      const sent = await this.#request(destination.href, {method: "PUT", body: data as Uint8Array<ArrayBuffer>, headers: packHeaders});
      if (!sent.ok) return this.#failure(sent, destination.href);
    }
    const confirm = `${this.base}/v1/uploads/${encodeURIComponent(upload.uploadId)}/confirm`;
    onProgress?.("Confirming the upload");
    const confirmed = await this.#request(confirm, {method: "POST", headers: {...authorization, accept: "application/json"}});
    if (!confirmed.ok) return this.#failure(confirmed, confirm);
    const value = await this.#json<RegistryVersion>(confirmed, confirm, "version");
    if (value.sha256 !== validation.sha256 || value.version !== request.version)
      throw new Error(`${confirm} confirmed ${value.version} (${value.sha256}), not the uploaded ${request.version} (${validation.sha256})`);
    for (const [platform, announced] of Object.entries(platformPackages))
      if (value.platformDownloads?.[platform as NativeExtensionPlatform]?.sha256 !== announced.sha256)
        throw new Error(`${confirm} did not confirm the uploaded ${platform} native pack (${announced.sha256})`);
    return {value, url: confirmed.url || confirm, static: false};
  }

  /** Marks a version yanked (or not): installs skip it unless they pin it exactly. */
  async yank(id: string, version: string, yanked: boolean, reason?: string): Promise<RegistryVersion> {
    const authorization = this.#authorization();
    const url = `${this.base}/v1/extensions/${idPath(id)}/versions/${encodeURIComponent(version)}/yank`;
    const response = await this.#request(url, {method: "POST",
      body: JSON.stringify({yanked, ...(reason === undefined ? {} : {reason})}),
      headers: {...authorization, "content-type": "application/json", accept: "application/json"}});
    if (!response.ok) return this.#failure(response, url);
    return this.#json<RegistryVersion>(response, url, "version");
  }
}

export function resolutionMessage(id: string, range: string, engine: string,
  resolved: {readonly problem?: string; readonly incompatible?: readonly {version: string; requiredEngineVersion: string}[]},
  platform?: string): string {
  if (resolved.problem === "invalid-range") return `${range} is not a version range`;
  if (resolved.problem === "platform")
    return `No version of ${id} matching ${range} for engine ${engine} runs on ${platform}`;
  if (resolved.problem === "engine")
    return `No version of ${id} matching ${range} runs on engine ${engine}: ${(resolved.incompatible ?? [])
      .map((item) => `${item.version} needs ${item.requiredEngineVersion}`).join(", ")}`;
  return `No published version of ${id} matches ${range} (yanked versions install only when pinned exactly)`;
}
