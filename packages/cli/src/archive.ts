// Archives without external tools: extracting a .tgz/.tar.gz (an npm package
// or a runtime bundle) safely, and writing the .zip and .tar.gz files a
// distribution ships in. Both writers record Unix permissions, so an
// executable stays executable when the archive is made on Windows.

import {createReadStream, createWriteStream} from "node:fs";
import {lstat, mkdir, open, readFile, readdir, readlink} from "node:fs/promises";
import {dirname, join, posix} from "node:path";
import {pipeline} from "node:stream/promises";
import {Readable} from "node:stream";
import {createGunzip, createGzip, crc32, deflateRawSync} from "node:zlib";

/** A file, directory or symbolic link read from or written to an archive; paths use "/". */
export interface ArchiveEntry {
  readonly path: string;
  /** A hard link (only read from tar archives) names another member as its target. */
  readonly type: "file" | "directory" | "symlink" | "hardlink";
  /** Permission bits (0o755, 0o644, ...). */
  readonly mode: number;
  readonly size: number;
  /** The link target of a symbolic link. */
  readonly target?: string;
}

/** A normalized relative archive path, or undefined when it is unsafe (absolute, "..", a drive, a backslash). */
export function safeArchivePath(name: string): string | undefined {
  let value = name.replace(/^(?:\.\/)+/u, "");
  if (value.endsWith("/")) value = value.slice(0, -1);
  if (value.length === 0 || value.length > 4096 || value.startsWith("/") || /[\\\x00]/u.test(value) ||
      /^[A-Za-z]:/u.test(value)) return undefined;
  const parts = value.split("/");
  if (parts.some(part => part === "" || part === "." || part === "..")) return undefined;
  return parts.join("/");
}

export interface ExtractLimits {
  readonly maxEntries?: number;
  readonly maxBytes?: number;
}

function octal(header: Buffer, start: number, length: number): number {
  // GNU base-256 for large values.
  if ((header[start]! & 0x80) !== 0) {
    let value = 0;
    for (let index = start + 1; index < start + length; index++) value = value * 256 + header[index]!;
    return value;
  }
  const text = header.toString("latin1", start, start + length).replace(/\0.*$/su, "").trim();
  return text.length === 0 ? 0 : Number.parseInt(text, 8);
}

function cString(header: Buffer, start: number, length: number): string {
  const end = header.indexOf(0, start);
  return header.toString("utf8", start, end < 0 || end > start + length ? start + length : end);
}

function paxRecords(data: Buffer): Map<string, string> {
  const records = new Map<string, string>();
  let at = 0;
  while (at < data.length) {
    const space = data.indexOf(0x20, at);
    if (space < 0) break;
    const length = Number.parseInt(data.toString("latin1", at, space), 10);
    if (!Number.isSafeInteger(length) || length <= 0 || at + length > data.length) break;
    const record = data.toString("utf8", space + 1, at + length - 1);
    const equals = record.indexOf("=");
    if (equals > 0) records.set(record.slice(0, equals), record.slice(equals + 1));
    at += length;
  }
  return records;
}

/**
 * Extracts a gzip-compressed tar archive into `destination`, which must be
 * a new or empty directory. Only regular files, directories and symbolic
 * links are accepted; a link is recorded (not created) so callers decide
 * whether to materialize it. Nothing in the archive is ever executed.
 */
export async function extractTarGz(archive: string, destination: string,
  limits: ExtractLimits = {}): Promise<ArchiveEntry[]> {
  const maxEntries = limits.maxEntries ?? 50_000;
  const maxBytes = limits.maxBytes ?? 4 * 1024 ** 3;
  const entries: ArchiveEntry[] = [];
  const seen = new Set<string>();
  let total = 0;
  let pending: Buffer = Buffer.alloc(0);
  let ended = false;
  // Current file being written.
  let file: {handle: Awaited<ReturnType<typeof open>>; remaining: number; padding: number} | undefined;
  let skip = 0;
  let meta: {kind: "pax" | "longname" | "longlink"; remaining: number; padding: number; chunks: Buffer[]} | undefined;
  let pax = new Map<string, string>();
  let longName: string | undefined;
  let longLink: string | undefined;

  const record = (entry: ArchiveEntry) => {
    const key = process.platform === "win32" || process.platform === "darwin" ? entry.path.toLowerCase() : entry.path;
    if (seen.has(key)) throw new Error(`Archive lists ${entry.path} twice`);
    seen.add(key);
    entries.push(entry);
    if (entries.length > maxEntries) throw new Error(`Archive has more than ${maxEntries} entries`);
  };
  const header = async (block: Buffer) => {
    if (block.every(byte => byte === 0)) { ended = true; return; }
    let sum = 0;
    for (let index = 0; index < 512; index++) sum += index >= 148 && index < 156 ? 32 : block[index]!;
    if (sum !== octal(block, 148, 8)) throw new Error("Archive header checksum is invalid");
    const type = String.fromCharCode(block[156]!);
    const size = octal(block, 124, 12);
    if (!Number.isSafeInteger(size) || size < 0) throw new Error("Archive entry size is invalid");
    const padding = (512 - (size % 512)) % 512;
    if (type === "x" || type === "L" || type === "K") {
      if (size > 1024 * 1024) throw new Error("Archive metadata record is too large");
      meta = {kind: type === "x" ? "pax" : type === "L" ? "longname" : "longlink", remaining: size, padding, chunks: []};
      return;
    }
    if (type === "g") { skip = size + padding; return; }
    const prefix = block.subarray(257, 263).toString("latin1").startsWith("ustar") ? cString(block, 345, 155) : "";
    const rawName = pax.get("path") ?? longName ?? (prefix ? `${prefix}/${cString(block, 0, 100)}` : cString(block, 0, 100));
    const linkName = pax.get("linkpath") ?? longLink ?? cString(block, 157, 100);
    const entrySize = pax.has("size") ? Number(pax.get("size")) : size;
    if (!Number.isSafeInteger(entrySize) || entrySize < 0) throw new Error("Archive entry size is invalid");
    const entryPadding = (512 - (entrySize % 512)) % 512;
    pax = new Map(); longName = undefined; longLink = undefined;
    const mode = octal(block, 100, 8) & 0o7777;
    if (rawName.replace(/^(?:\.\/)+/u, "") === "" || rawName === "./") { skip = entrySize + entryPadding; return; }
    const path = safeArchivePath(rawName);
    if (path === undefined) throw new Error(`Archive entry has an unsafe path: ${rawName}`);
    if (type === "5") {
      record({path, type: "directory", mode, size: 0});
      await mkdir(join(destination, ...path.split("/")), {recursive: true});
      skip = entrySize + entryPadding;
      return;
    }
    if (type === "2" || type === "1") {
      // Hard links name another archive member; symbolic links a relative target.
      const target = type === "1" ? safeArchivePath(linkName) : linkName;
      if (target === undefined || target.length === 0) throw new Error(`Archive link ${path} has an unsafe target`);
      record({path, type: type === "1" ? "hardlink" : "symlink", mode, size: 0, target});
      skip = entrySize + entryPadding;
      return;
    }
    if (type !== "0" && type !== "\0" && type !== "7")
      throw new Error(`Archive entry ${path} has an unsupported type ${JSON.stringify(type)}`);
    total += entrySize;
    if (total > maxBytes) throw new Error(`Archive expands to more than ${maxBytes} bytes`);
    record({path, type: "file", mode, size: entrySize});
    const target = join(destination, ...path.split("/"));
    await mkdir(dirname(target), {recursive: true});
    const handle = await open(target, "wx");
    file = {handle, remaining: entrySize, padding: entryPadding};
    if (entrySize === 0) { await handle.close(); file = undefined; skip = entryPadding; }
  };

  const source = createReadStream(archive).pipe(createGunzip());
  try {
    for await (const chunk of source as AsyncIterable<Buffer>) {
      pending = pending.length === 0 ? chunk : Buffer.concat([pending, chunk]);
      for (;;) {
        if (ended) break;
        if (skip > 0) {
          const used = Math.min(skip, pending.length);
          skip -= used; pending = pending.subarray(used);
          if (skip > 0) break;
          continue;
        }
        if (file !== undefined) {
          const used = Math.min(file.remaining, pending.length);
          if (used > 0) await file.handle.write(pending.subarray(0, used));
          file.remaining -= used; pending = pending.subarray(used);
          if (file.remaining > 0) break;
          await file.handle.close();
          skip = file.padding; file = undefined;
          continue;
        }
        if (meta !== undefined) {
          const used = Math.min(meta.remaining, pending.length);
          meta.chunks.push(pending.subarray(0, used));
          meta.remaining -= used; pending = pending.subarray(used);
          if (meta.remaining > 0) break;
          const data = Buffer.concat(meta.chunks);
          if (meta.kind === "pax") pax = paxRecords(data);
          else if (meta.kind === "longname") longName = data.toString("utf8").replace(/\0.*$/su, "");
          else longLink = data.toString("utf8").replace(/\0.*$/su, "");
          skip = meta.padding; meta = undefined;
          continue;
        }
        if (pending.length < 512) break;
        const block = Buffer.from(pending.subarray(0, 512));
        pending = pending.subarray(512);
        await header(block);
      }
      if (ended) break;
    }
  } finally {
    if (file !== undefined) await file.handle.close().catch(() => undefined);
    source.destroy();
  }
  if (file !== undefined || meta !== undefined) throw new Error("Archive ends inside an entry");
  return entries;
}

/** Lists a directory tree for an archive: files, directories and (on POSIX hosts) symbolic links, sorted. */
export async function archiveTree(root: string, exclude: (path: string) => boolean = () => false): Promise<ArchiveEntry[]> {
  const entries: ArchiveEntry[] = [];
  const walk = async (directory: string, prefix: string) => {
    const names = (await readdir(directory)).sort();
    for (const name of names) {
      const path = prefix ? `${prefix}/${name}` : name;
      if (exclude(path)) continue;
      const full = join(directory, name);
      const info = await lstat(full);
      if (info.isSymbolicLink()) {
        entries.push({path, type: "symlink", mode: 0o777, size: 0, target: await readlink(full)});
      } else if (info.isDirectory()) {
        entries.push({path, type: "directory", mode: 0o755, size: 0});
        await walk(full, path);
      } else if (info.isFile()) {
        entries.push({path, type: "file", mode: info.mode & 0o777, size: info.size});
      } else throw new Error(`Cannot archive ${full}: not a file, directory or link`);
    }
  };
  await walk(root, "");
  return entries;
}

/** Whether a file starts like a native executable or a script (ELF, Mach-O, "#!"). */
export async function looksExecutable(path: string): Promise<boolean> {
  const handle = await open(path, "r");
  try {
    const head = Buffer.alloc(4);
    const {bytesRead} = await handle.read(head, 0, 4, 0);
    if (bytesRead >= 2 && head[0] === 0x23 && head[1] === 0x21) return true;
    if (bytesRead < 4) return false;
    const big = head.readUInt32BE(0);
    const little = head.readUInt32LE(0);
    return big === 0x7f454c46 || big === 0xcafebabe || big === 0xcafebabf ||
      little === 0xfeedfacf || little === 0xfeedface;
  } finally { await handle.close(); }
}

const smallFile = 64 * 1024 * 1024;

async function fileCrc(path: string): Promise<number> {
  let crc = 0;
  for await (const chunk of createReadStream(path) as AsyncIterable<Buffer>) crc = crc32(chunk, crc);
  return crc >>> 0;
}

function dosTime(date: Date): {time: number; date: number} {
  const year = Math.min(Math.max(date.getFullYear(), 1980), 2107);
  return {
    time: (date.getHours() << 11) | (date.getMinutes() << 5) | Math.floor(date.getSeconds() / 2),
    date: ((year - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate(),
  };
}

export interface ArchiveSource {
  /** The archive's entries in order; paths are what the archive stores. */
  readonly entries: readonly ArchiveEntry[];
  /** The file on disk holding an entry's bytes. */
  readonly fileOf: (entry: ArchiveEntry) => string;
  /** Modification time recorded for every entry. */
  readonly modified?: Date;
}

/**
 * Writes a .zip with Unix permissions ("version made by" Unix), UTF-8
 * names and ZIP64 records when sizes or offsets need them. Small files are
 * deflated when that saves space; large ones (game archives are already
 * compressed) are stored.
 */
export async function writeZip(output: string, source: ArchiveSource): Promise<void> {
  const stream = createWriteStream(output, {flags: "wx"});
  let offset = 0;
  const write = async (data: Buffer) => {
    if (!stream.write(data)) await new Promise<void>((accept) => stream.once("drain", accept));
    offset += data.length;
  };
  const central: Buffer[] = [];
  const stamp = dosTime(source.modified ?? new Date());
  try {
    for (const entry of source.entries) {
      const directory = entry.type === "directory";
      const name = Buffer.from(directory ? `${entry.path}/` : entry.path, "utf8");
      let data: Buffer | undefined;
      let crc = 0;
      let size = 0;
      let compressed = 0;
      let method = 0;
      const unixType = entry.type === "symlink" ? 0o120000 : directory ? 0o040000 : 0o100000;
      if (entry.type === "symlink") {
        data = Buffer.from(entry.target ?? "", "utf8");
        size = compressed = data.length; crc = crc32(data) >>> 0;
      } else if (!directory) {
        const path = source.fileOf(entry);
        size = (await lstat(path)).size;
        if (size <= smallFile) {
          const raw = await readFile(path);
          crc = crc32(raw) >>> 0;
          const deflated = deflateRawSync(raw, {level: 6});
          if (deflated.length < raw.length) { data = deflated; method = 8; } else data = raw;
          compressed = data.length;
        } else {
          crc = await fileCrc(path);
          compressed = size;
        }
      }
      const zip64 = size >= 0xffffffff || compressed >= 0xffffffff || offset >= 0xffffffff;
      const localExtra = zip64 ? Buffer.alloc(20) : Buffer.alloc(0);
      if (zip64) {
        localExtra.writeUInt16LE(0x0001, 0); localExtra.writeUInt16LE(16, 2);
        localExtra.writeBigUInt64LE(BigInt(size), 4); localExtra.writeBigUInt64LE(BigInt(compressed), 12);
      }
      const local = Buffer.alloc(30);
      local.writeUInt32LE(0x04034b50, 0);
      local.writeUInt16LE(zip64 ? 45 : 20, 4);
      local.writeUInt16LE(0x0800, 6);
      local.writeUInt16LE(method, 8);
      local.writeUInt16LE(stamp.time, 10); local.writeUInt16LE(stamp.date, 12);
      local.writeUInt32LE(crc, 14);
      local.writeUInt32LE(zip64 ? 0xffffffff : compressed, 18);
      local.writeUInt32LE(zip64 ? 0xffffffff : size, 22);
      local.writeUInt16LE(name.length, 26); local.writeUInt16LE(localExtra.length, 28);
      const localOffset = offset;
      await write(local); await write(name); await write(localExtra);
      if (data !== undefined) await write(data);
      else if (!directory) {
        for await (const chunk of createReadStream(source.fileOf(entry)) as AsyncIterable<Buffer>) await write(chunk);
      }
      const centralZip64 = zip64 || localOffset >= 0xffffffff;
      const extra = centralZip64 ? Buffer.alloc(28) : Buffer.alloc(0);
      if (centralZip64) {
        extra.writeUInt16LE(0x0001, 0); extra.writeUInt16LE(24, 2);
        extra.writeBigUInt64LE(BigInt(size), 4); extra.writeBigUInt64LE(BigInt(compressed), 12);
        extra.writeBigUInt64LE(BigInt(localOffset), 20);
      }
      const record = Buffer.alloc(46);
      record.writeUInt32LE(0x02014b50, 0);
      record.writeUInt16LE((3 << 8) | (centralZip64 ? 45 : 20), 4);
      record.writeUInt16LE(centralZip64 ? 45 : 20, 6);
      record.writeUInt16LE(0x0800, 8);
      record.writeUInt16LE(method, 10);
      record.writeUInt16LE(stamp.time, 12); record.writeUInt16LE(stamp.date, 14);
      record.writeUInt32LE(crc, 16);
      record.writeUInt32LE(centralZip64 ? 0xffffffff : compressed, 20);
      record.writeUInt32LE(centralZip64 ? 0xffffffff : size, 24);
      record.writeUInt16LE(name.length, 28); record.writeUInt16LE(extra.length, 30);
      // External attributes: Unix type and permissions high, MS-DOS directory bit low.
      record.writeUInt32LE((((unixType | (entry.mode & 0o7777)) << 16) | (directory ? 0x10 : 0)) >>> 0, 38);
      record.writeUInt32LE(centralZip64 ? 0xffffffff : localOffset, 42);
      central.push(record, name, extra);
    }
    const centralOffset = offset;
    for (const part of central) await write(part);
    const centralSize = offset - centralOffset;
    const count = source.entries.length;
    if (count >= 0xffff || centralOffset >= 0xffffffff || centralSize >= 0xffffffff) {
      const zip64End = Buffer.alloc(56);
      zip64End.writeUInt32LE(0x06064b50, 0);
      zip64End.writeBigUInt64LE(44n, 4);
      zip64End.writeUInt16LE((3 << 8) | 45, 12); zip64End.writeUInt16LE(45, 14);
      zip64End.writeBigUInt64LE(BigInt(count), 24); zip64End.writeBigUInt64LE(BigInt(count), 32);
      zip64End.writeBigUInt64LE(BigInt(centralSize), 40); zip64End.writeBigUInt64LE(BigInt(centralOffset), 48);
      const locator = Buffer.alloc(20);
      locator.writeUInt32LE(0x07064b50, 0);
      locator.writeBigUInt64LE(BigInt(offset), 8);
      locator.writeUInt32LE(1, 16);
      await write(zip64End); await write(locator);
    }
    const end = Buffer.alloc(22);
    end.writeUInt32LE(0x06054b50, 0);
    end.writeUInt16LE(Math.min(count, 0xffff), 8); end.writeUInt16LE(Math.min(count, 0xffff), 10);
    end.writeUInt32LE(Math.min(centralSize, 0xffffffff), 12);
    end.writeUInt32LE(Math.min(centralOffset, 0xffffffff), 16);
    await write(end);
  } finally {
    await new Promise<void>((accept, reject) => stream.end((error?: Error | null) => error ? reject(error) : accept()));
  }
}

function tarHeader(name: string, entry: ArchiveEntry, size: number, mtime: number, typeflag: string,
  linkName = ""): Buffer {
  const block = Buffer.alloc(512);
  block.write(name, 0, 100, "utf8");
  const field = (value: number, start: number, length: number) =>
    block.write(`${value.toString(8).padStart(length - 1, "0")}\0`, start, length, "latin1");
  field(entry.mode & 0o7777, 100, 8);
  field(0, 108, 8); field(0, 116, 8);
  field(size, 124, 12);
  field(Math.floor(mtime), 136, 12);
  block.fill(0x20, 148, 156);
  block.write(typeflag, 156, 1, "latin1");
  block.write(linkName, 157, 100, "utf8");
  block.write("ustar\0", 257, 6, "latin1");
  block.write("00", 263, 2, "latin1");
  let sum = 0;
  for (const byte of block) sum += byte;
  block.write(`${sum.toString(8).padStart(6, "0")}\0 `, 148, 8, "latin1");
  return block;
}

function paxHeader(records: Readonly<Record<string, string>>, mtime: number): Buffer[] {
  let body = "";
  for (const [key, value] of Object.entries(records)) {
    const line = ` ${key}=${value}\n`;
    let length = Buffer.byteLength(line) + 1;
    while (String(length).length + Buffer.byteLength(line) !== length) length = String(length).length + Buffer.byteLength(line);
    body += `${length}${line}`;
  }
  const data = Buffer.from(body, "utf8");
  const header = tarHeader("././@PaxHeader", {path: "", type: "file", mode: 0o644, size: 0}, data.length, mtime, "x");
  return [header, data, Buffer.alloc((512 - (data.length % 512)) % 512)];
}

/** Writes a POSIX (pax) tar compressed with gzip; long names and large files use pax records. */
export async function writeTarGz(output: string, source: ArchiveSource): Promise<void> {
  const mtime = (source.modified ?? new Date()).getTime() / 1000;
  async function* blocks(): AsyncGenerator<Buffer> {
    for (const entry of source.entries) {
      const name = entry.type === "directory" ? `${entry.path}/` : entry.path;
      const size = entry.type === "file" ? (await lstat(source.fileOf(entry))).size : 0;
      const records: Record<string, string> = {};
      if (Buffer.byteLength(name) > 100 || !/^[\x20-\x7e]*$/u.test(name)) records.path = name;
      if (entry.target !== undefined && (Buffer.byteLength(entry.target) > 100 || !/^[\x20-\x7e]*$/u.test(entry.target)))
        records.linkpath = entry.target;
      if (size > 0o77777777777) records.size = String(size);
      if (Object.keys(records).length > 0) yield* paxHeader(records, mtime);
      const shortName = records.path === undefined ? name : posix.basename(name).slice(0, 99);
      yield tarHeader(shortName, entry, size > 0o77777777777 ? 0 : size, mtime,
        entry.type === "directory" ? "5" : entry.type === "symlink" ? "2" : "0",
        records.linkpath === undefined ? entry.target ?? "" : "");
      if (entry.type === "file") {
        for await (const chunk of createReadStream(source.fileOf(entry)) as AsyncIterable<Buffer>) yield chunk;
        if (size % 512 !== 0) yield Buffer.alloc(512 - (size % 512));
      }
    }
    yield Buffer.alloc(1024);
  }
  await pipeline(Readable.from(blocks()), createGzip({level: 6}), createWriteStream(output, {flags: "wx"}));
}
