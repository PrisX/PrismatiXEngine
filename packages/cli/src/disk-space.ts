import {statfs} from "node:fs/promises";
import {dirname, resolve} from "node:path";

const mib = 1024n * 1024n;
const gib = 1024n * mib;

export type PackageDiskSpaceAssessment = "sufficient" | "warning" | "insufficient";

/** The publication staging directory is a sibling of output, not inside it. */
export async function availableBytesForOutput(output: string): Promise<bigint | undefined> {
  let parent = dirname(resolve(output));
  for (;;) {
    try {
      const volume = await statfs(parent, {bigint: true});
      return volume.bavail * volume.bsize;
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") return undefined;
      const ancestor = dirname(parent);
      if (ancestor === parent) return undefined;
      parent = ancestor;
    }
  }
}

export function assessPackageDiskSpace(
  inputBytes: bigint, availableBytes: bigint, compression: string,
): PackageDiskSpaceAssessment {
  if (compression === "none" && availableBytes < inputBytes) return "insufficient";
  // The archive needs an index, Player and publication metadata too. For
  // compressed packages the final size is unknown, so this is advisory only.
  const reserve = inputBytes / 100n > 16n * mib ? inputBytes / 100n : 16n * mib;
  return availableBytes < inputBytes + reserve ? "warning" : "sufficient";
}

export function formatDiskBytes(bytes: bigint): string {
  const unit = bytes >= gib ? gib : mib;
  const scaled = (bytes * 100n + unit / 2n) / unit;
  return `${scaled / 100n}.${String(scaled % 100n).padStart(2, "0")} ${unit === gib ? "GiB" : "MiB"}`;
}
