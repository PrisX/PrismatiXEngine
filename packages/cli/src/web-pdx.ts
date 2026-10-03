import {createHash, createDecipheriv} from "node:crypto";
import {open} from "node:fs/promises";
import {zstdDecompressSync, crc32} from "node:zlib";

// Export reads only small shader resources. PDX archives are never loaded whole.
export async function readWebShaderArtifact(archive: string, logicalPath: string, passphrase = ""): Promise<Buffer | undefined> {
  const handle = await open(archive, "r");
  try {
    const fileSize = (await handle.stat()).size;
    const read = async (offset: number, size: number) => {
      if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(size) || offset < 0 || size < 0 || offset + size > fileSize)
        throw new Error("Invalid PDX range");
      const bytes = Buffer.alloc(size); let done = 0;
      while (done < size) { const result = await handle.read(bytes, done, size - done, offset + done); if (!result.bytesRead) throw new Error("Truncated PDX resource"); done += result.bytesRead; }
      return bytes;
    };
    const header = await read(0, 28), version = header.readUInt32LE(4);
    if (header.toString("ascii", 0, 4) !== `PDX${version}` || (version !== 4 && version !== 5)) throw new Error("Unsupported PDX archive");
    const encrypted = !!(header.readUInt32LE(8) & 1);
    const key = createHash("sha256").update(passphrase).digest();
    const decrypt = (bytes: Buffer, context = "") => {
      if (!encrypted || bytes.length < 28) throw new Error("Invalid encrypted shader artifact");
      const decoder = createDecipheriv("aes-256-gcm", key, bytes.subarray(0, 12));
      decoder.setAuthTag(bytes.subarray(-16)); decoder.setAAD(Buffer.from(context));
      return Buffer.concat([decoder.update(bytes.subarray(12, -16)), decoder.final()]);
    };
    const indexOffset = Number(header.readBigUInt64LE(12)), indexSize = Number(header.readBigUInt64LE(20));
    if (indexOffset < 28 || indexSize > 256 * 1024 * 1024) throw new Error("Invalid PDX index bounds");
    let index = await read(indexOffset, indexSize); if (encrypted) index = decrypt(index);
    let cursor = 4; const count = index.readUInt32LE(0);
    if (count > 1000000) throw new Error("Invalid PDX index count");
    for (let i = 0; i < count; i++) {
      const length = index.readUInt16LE(cursor + 8); cursor += 10;
      if (!length || length > 4096) throw new Error("Invalid PDX resource name");
      const name = index.toString("utf8", cursor, cursor + length); cursor += length;
      const offset = Number(index.readBigUInt64LE(cursor)), rawSize = Number(index.readBigUInt64LE(cursor + 8)), storedSize = Number(index.readBigUInt64LE(cursor + 16));
      const crc = index.readUInt32LE(cursor + 24), flags = index[cursor + 28]!; cursor += 29;
      let chunkSize = 0, chunks = 0;
      if (flags & 4) { chunkSize = index.readUInt32LE(cursor); chunks = index.readUInt32LE(cursor + 4); cursor += 8; }
      if (name !== logicalPath) continue;
      if (rawSize <= 0 || rawSize > 4 * 1024 * 1024 || storedSize > 8 * 1024 * 1024 || offset + storedSize > indexOffset)
        throw new Error("Web shader exceeds bounded resource limits");
      let bytes = await read(offset, storedSize);
      if (flags & 4) {
        if (!(flags & 1) || (flags & 2) || chunkSize !== 256 * 1024 || chunks !== Math.ceil(rawSize / chunkSize)) throw new Error("Invalid PDX encryption chunks");
        const plain: Buffer[] = []; let position = 0;
        for (let chunk = 0; chunk < chunks; chunk++) {
          const size = Math.min(chunkSize, rawSize - chunk * chunkSize);
          plain.push(decrypt(bytes.subarray(position, position + size + 28), `${name}#pdx5:${chunk}:${size}`)); position += size + 28;
        }
        bytes = Buffer.concat(plain);
      } else if (flags & 1) bytes = decrypt(bytes);
      if (flags & 2) bytes = zstdDecompressSync(bytes, {maxOutputLength: rawSize});
      if (bytes.length !== rawSize || crc32(bytes) !== crc) throw new Error("PDX shader integrity check failed");
      return bytes;
    }
    return undefined;
  } finally { await handle.close(); }
}
