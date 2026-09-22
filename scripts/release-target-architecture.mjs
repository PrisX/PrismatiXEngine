#!/usr/bin/env node
// Read the actual configured Ninja link graph; do not reconfigure or build it.
import assert from "node:assert/strict";
import {readFile, mkdir, writeFile} from "node:fs/promises";
import {basename, dirname, resolve} from "node:path";
const args = process.argv.slice(2);
const arg = (name, fallback) => args.includes(name) ? args[args.indexOf(name) + 1] : fallback;
const buildDirectory = resolve(arg("--build-dir", "out/build/release-candidate"));
const output = resolve(arg("--output", "out/release-evidence/target-architecture.json"));
const source = (await readFile(resolve(buildDirectory, "build.ninja"), "utf8")).replace(/\$\r?\n\s*/g, "");
const nodes = [];
for (const line of source.split(/\r?\n/)) {
  const match = /^build (\S+): CXX_(?:STATIC_LIBRARY|EXECUTABLE)_LINKER[^ ]*\s+(.*)$/.exec(line);
  if (!match) continue;
  const name = basename(match[1]).replace(/^lib/, "").replace(/\.(lib|a|exe)$/, "");
  if (name.startsWith("PrismatiX")) nodes.push({name, path: match[1], dependencyPaths: match[2].split(/\s+/)});
}
assert.ok(nodes.length > 0, "No configured PrismatiX native linker targets found (Ninja build required)");
const byPath = new Map(nodes.map(node => [node.path, node.name]));
const graph = Object.fromEntries(nodes.map(node => [node.name, [...new Set(node.dependencyPaths.map(path => byPath.get(path)).filter(Boolean))]]));
const violations = [];
function closure(name, visiting = new Set()) {
  if (visiting.has(name)) { violations.push(`Dependency cycle: ${[...visiting, name].join(" -> ")}`); return new Set(); }
  const next = new Set([...visiting, name]), result = new Set();
  for (const dependency of graph[name] ?? []) { result.add(dependency); for (const child of closure(dependency, next)) result.add(child); }
  return result;
}
const required = ["PrismatiXRuntimeContracts", "PrismatiXSDK", "PrismatiXRuntimeCore", "PrismatiXRuntimePlatformNative", "PrismatiXPlayer", "PrismatiXPreviewHost", "PrismatiXPackager"];
for (const name of required) if (!(name in graph)) violations.push(`Required target is missing: ${name}`);
for (const name of Object.keys(graph)) closure(name);
for (const name of ["PrismatiXRuntimeContracts", "PrismatiXSDK", "PrismatiXPackager"]) {
  for (const dependency of closure(name)) if (/RuntimeCore|RuntimePlatformNative|PreviewHost|Player|Editor|Studio/.test(dependency))
    violations.push(`${name} pulls runtime/application implementation into the public SDK/Packager boundary: ${dependency}`);
}
for (const name of ["PrismatiXPlayer", "PrismatiXPreviewHost"]) {
  const reachable = closure(name);
  for (const dependency of ["PrismatiXRuntimeCore", "PrismatiXRuntimePlatformNative", "PrismatiXRuntimeContracts"])
    if (!reachable.has(dependency)) violations.push(`${name} does not consume shared ${dependency}`);
}
const report = {kind: "configured-ninja-target-architecture", buildDirectory, checkedTargets: nodes.length,
  limitations: ["This checks configured link dependencies; it does not establish C++ ABI compatibility or runtime behavior.",
    "InstalledSDKConsumer compilation/link/run remains a separate release gate."], graph, violations: [...new Set(violations)], passed: violations.length === 0};
await mkdir(dirname(output), {recursive: true}); await writeFile(output, `${JSON.stringify(report, null, 2)}\n`);
process.stdout.write(`${report.passed ? "PASS" : "FAIL"}: ${nodes.length} configured targets; ${output}\n`);
if (!report.passed) { process.stderr.write(`${report.violations.join("\n")}\n`); process.exitCode = 1; }
