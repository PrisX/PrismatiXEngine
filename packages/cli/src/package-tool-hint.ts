/** A developer checkout can expose both Debug and optimized Packagers. Keep
 * explicit tool selection intact, but warn before an expensive large build. */
export function debugPackagerHint(path: string, inputBytes: bigint): string | undefined {
  if (inputBytes < 1024n ** 3n) return undefined;
  const directories = path.split(/[\\/]+/u).slice(0, -1).map(segment => segment.toLowerCase());
  if (!directories.some(segment => segment === "debug" || segment === "x64-debug"))
    return undefined;
  return "A Debug Packager was selected for a large artifact. Its unoptimized " +
    "validation and hashing may take much longer; use a Release or RelWithDebInfo " +
    "Packager for distribution builds. The Player may remain Debug for testing.";
}
