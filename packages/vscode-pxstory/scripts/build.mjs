// Bundles the extension client (CommonJS, as VS Code loads it) and the
// language server with its project-validation worker (ESM, sharing chunks
// with the Authoring SDK) into dist/. Build the SDK first.
import {mkdir, readdir, readFile, rm, writeFile} from "node:fs/promises";
import {dirname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";

import {build} from "esbuild";

// Importing the workspace SDK runs its local build-freshness check, so a
// stale SDK dist fails here instead of being frozen into the bundle.
await import("@prismatix/authoring-sdk");

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const dist = join(root, "dist");
const minify = !process.argv.includes("--no-minify");

/** The SDK entry re-checks its source tree at import time. Inside a bundle
 * that check would look for sources next to the extension; it already ran
 * above, so the bundled copy omits it. */
const bundledSdkEntry = {
  name: "bundled-sdk-entry",
  setup(builder) {
    // esbuild filters are Go regular expressions: no JavaScript flags.
    builder.onLoad({filter: /authoring-sdk[\\/]dist[\\/]src[\\/]index\.js$/}, async (args) => {
      const text = await readFile(args.path, "utf8");
      const replaced = text.replace(/^assertLocalBuildFresh\(.*\);$/mu,
        "/* local build freshness was checked when the extension was built */");
      if (replaced === text) throw new Error("SDK build-freshness call not found; update scripts/build.mjs");
      return {contents: replaced, loader: "js"};
    });
  },
};

await rm(dist, {recursive: true, force: true});
await mkdir(dist, {recursive: true});

const common = {bundle: true, platform: "node", target: "node20", sourcemap: true, minify,
  logLevel: "warning", absWorkingDir: root, legalComments: "none", metafile: true,
  plugins: [bundledSdkEntry]};

const client = await build({...common, entryPoints: ["src/extension.ts"], format: "cjs",
  outfile: join(dist, "extension.cjs"), external: ["vscode"]});

const server = await build({...common,
  entryPoints: {"server-main": "src/server/main.ts", "project-worker": "src/server/project-worker.ts"},
  format: "esm", splitting: true, outdir: dist, chunkNames: "chunks/[name]-[hash]",
  // CommonJS dependencies (vscode-languageserver) call require() for Node
  // built-ins, which an ESM bundle only provides through createRequire.
  banner: {js: "import {createRequire as __pxCreateRequire} from \"node:module\";" +
    "const require = __pxCreateRequire(import.meta.url);"}});

// A separate entry module: the SDK's build-freshness helper treats the module
// named by process.argv[1] as its own command line, so the forked server file
// must not be a bundle that contains it.
await writeFile(join(dist, "server.js"),
  "// Forked by the VS Code language client; see scripts/build.mjs.\nawait import(\"./server-main.js\");\n");

// The .vsix redistributes every bundled package; ship their license texts.
const packages = new Map();
for (const input of [...Object.keys(client.metafile.inputs), ...Object.keys(server.metafile.inputs)]) {
  const path = resolve(root, input);
  let directory = dirname(path);
  while (directory !== dirname(directory)) {
    const manifest = await readFile(join(directory, "package.json"), "utf8").then(JSON.parse, () => undefined);
    if (manifest?.name !== undefined && manifest.version !== undefined) {
      // First-party @prismatix packages are covered by this extension's LICENSE.
      if (directory !== root && !manifest.name.startsWith("@prismatix/")) packages.set(`${manifest.name}@${manifest.version}`, {directory, manifest});
      break;
    }
    directory = dirname(directory);
  }
}
const notices = ["Third-party software bundled in the PrismatiX Story extension", ""];
for (const [id, {directory, manifest}] of [...packages].sort(([left], [right]) => left.localeCompare(right))) {
  const licenseFile = (await readdir(directory)).find((name) => /^(?:license|licence|copying)(?:\.|$)/iu.test(name));
  const text = licenseFile === undefined ? `License: ${manifest.license ?? "see package"}`
    : (await readFile(join(directory, licenseFile), "utf8")).trim();
  notices.push("=".repeat(76), `${id} (${manifest.license ?? "unknown license"})`, "", text, "");
}
await writeFile(join(dist, "THIRD_PARTY_NOTICES.txt"), `${notices.join("\n")}\n`);
