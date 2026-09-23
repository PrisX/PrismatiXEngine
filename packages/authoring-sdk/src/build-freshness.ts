import {createHash} from "node:crypto";
import {existsSync, lstatSync, mkdirSync, readFileSync, readdirSync, writeFileSync} from "node:fs";
import {join, resolve} from "node:path";
import {fileURLToPath} from "node:url";

const stampName = ".prismatix-source-stamp.json";
const stampFormat = "PrismatiXSourceStamp/1";

function sourcePaths(root: string): string[] {
  const paths = ["package.json", "tsconfig.json"];
  const visit = (directory: string): void => {
    for (const entry of readdirSync(join(root, directory), {withFileTypes: true})) {
      const path = `${directory}/${entry.name}`;
      if (entry.isSymbolicLink()) throw new Error(`Build source contains a symbolic link: ${path}`);
      if (entry.isDirectory()) visit(path);
      else if (entry.isFile()) paths.push(path);
      else throw new Error(`Build source contains a special file: ${path}`);
    }
  };
  visit("src");
  return paths.sort((a, b) => a.localeCompare(b, "en"));
}

/** Deterministic local-source identity. Published npm packages omit src and
 * therefore never need a workspace freshness check at runtime. */
export function localSourceFingerprint(packageRoot: string): string {
  const root = resolve(packageRoot);
  const hash = createHash("sha256").update(`${stampFormat}\0`);
  for (const path of sourcePaths(root)) {
    hash.update(`${path}\0`);
    hash.update(readFileSync(join(root, ...path.split("/"))));
    hash.update("\0");
  }
  return hash.digest("hex");
}

export function writeLocalBuildStamp(packageRoot: string): void {
  const root = resolve(packageRoot);
  const path = join(root, "dist", "src", stampName);
  mkdirSync(join(root, "dist", "src"), {recursive: true});
  writeFileSync(path, `${JSON.stringify({format: stampFormat,
    fingerprint: localSourceFingerprint(root)})}\n`, "utf8");
}

export function assertLocalBuildFresh(packageRoot: string, packageName: string): void {
  const root = resolve(packageRoot);
  if (!existsSync(join(root, "src"))) return;
  const path = join(root, "dist", "src", stampName);
  let fingerprint: unknown;
  try {
    const info = lstatSync(path);
    if (!info.isFile() || info.isSymbolicLink() || info.size > 4096) throw new Error("invalid stamp");
    const stamp = JSON.parse(readFileSync(path, "utf8")) as {
      format?: unknown; fingerprint?: unknown;
    };
    if (stamp.format === stampFormat) fingerprint = stamp.fingerprint;
  } catch { /* An absent or damaged stamp is stale, not a reason to run old code. */ }
  if (fingerprint !== localSourceFingerprint(root))
    throw new Error(`PXSDKBUILD1001 ${packageName} local source differs from dist; run npm run build in ${root}`);
}

// The compiled helper is also the post-tsc stamp writer for both workspace
// packages, so the writer and runtime checker use exactly the same algorithm.
const invokedPath = process.argv[1] === undefined ? undefined : resolve(process.argv[1]);
if (invokedPath === fileURLToPath(import.meta.url)) {
  if (process.argv[2] !== "--stamp" || process.argv.length !== 4)
    throw new Error("Usage: build-freshness --stamp <package-root>");
  writeLocalBuildStamp(process.argv[3]!);
}
