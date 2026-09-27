import {open} from "node:fs/promises";

/** Reads an authored text file with a hard byte cap, including when it grows
 * after stat. Invalid UTF-8 is rejected instead of silently replaced. */
export async function readBoundedUtf8File(path: string, maximumBytes: number): Promise<string> {
  if (!Number.isSafeInteger(maximumBytes) || maximumBytes < 0 ||
      maximumBytes >= Number.MAX_SAFE_INTEGER)
    throw new RangeError("maximumBytes must be non-negative and below Number.MAX_SAFE_INTEGER");
  const file = await open(path, "r");
  try {
    const info = await file.stat();
    if (!info.isFile() || info.size > maximumBytes)
      throw new RangeError(`File is not regular or exceeds ${maximumBytes} bytes: ${path}`);
    const limit = maximumBytes + 1;
    let bytes = Buffer.allocUnsafe(Math.min(limit, Math.max(1, info.size + 1)));
    let used = 0;
    while (true) {
      if (used === bytes.length) {
        const grown = Buffer.allocUnsafe(Math.min(limit, Math.max(bytes.length * 2, used + 1)));
        bytes.copy(grown, 0, 0, used);
        bytes = grown;
      }
      const {bytesRead} = await file.read(bytes, used, bytes.length - used, null);
      if (bytesRead === 0) break;
      used += bytesRead;
      if (used > maximumBytes)
        throw new RangeError(`File exceeds ${maximumBytes} bytes: ${path}`);
    }
    return new TextDecoder("utf-8", {fatal: true, ignoreBOM: true})
      .decode(bytes.subarray(0, used));
  } finally {
    await file.close();
  }
}
