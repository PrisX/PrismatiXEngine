/** Directories under the project root whose changes are not author edits. */
const ignoredRoots: ReadonlySet<string> = new Set([".prismatix", ".git", "node_modules", "dist"]);

/**
 * Whether a recursive `fs.watch` event on the project root is an author edit
 * that `prismatix dev` must rebuild for.
 *
 * Windows reports some events (the build touching the project root, for
 * example) with no file name. They are not edits; counting them restarted the
 * Preview right after launch, which skipped the title screen.
 */
export function isProjectEdit(filename: string | Buffer | null | undefined): boolean {
  if (filename === null || filename === undefined) return false;
  const path = String(filename);
  if (path.length === 0) return false;
  return !ignoredRoots.has(path.split(/[\\/]/u)[0] ?? "");
}
