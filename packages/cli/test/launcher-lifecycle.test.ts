import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {cp, mkdir, mkdtemp, readFile, rm, symlink, writeFile} from "node:fs/promises";
import {createRequire} from "node:module";
import {tmpdir} from "node:os";
import {dirname, join} from "node:path";
import {fileURLToPath} from "node:url";
import test from "node:test";

const entry = fileURLToPath(new URL("../src/cli.js", import.meta.url));
const require = createRequire(import.meta.url);
async function until(check: () => Promise<boolean>, message: () => string): Promise<void> {
  const deadline = Date.now() + 12_000;
  while (Date.now() < deadline) {
    if (await check()) return;
    await new Promise(resolve => setTimeout(resolve, 30));
  }
  assert.fail(message());
}
function alive(pid: number): boolean { try { process.kill(pid, 0); return true; } catch { return false; } }

for (const mode of ["ipc-stop", "launcher-crash"] as const) {
  test(`global to pinned local CLI to PreviewHost exits cleanly on ${mode}`, {timeout: 25_000}, async t => {
    const root = await mkdtemp(join(tmpdir(), "prismatix-launcher-lifecycle-"));
    let cleanupProcess: () => Promise<void> = async () => {};
    t.after(async () => { await cleanupProcess(); await rm(root, {recursive: true, force: true}); });
    const local = join(root, "node_modules", "@prismatix", "cli");
    await mkdir(join(root, "Story"), {recursive: true});
    await mkdir(join(root, ".prismatix"));
    await writeFile(join(root, "Story", "main.pxstory"), "Lifecycle fixture.\n[end]\n");
    await writeFile(join(root, "package.json"), JSON.stringify({devDependencies: {"@prismatix/cli": "0.2.0"}}));
    await cp(dirname(entry), join(local, "dist", "src"), {recursive: true});
    await writeFile(join(local, "package.json"), JSON.stringify({type: "module", version: "0.2.0"}));
    await symlink(dirname(dirname(dirname(fileURLToPath(import.meta.resolve("@prismatix/authoring-sdk"))))),
      join(root, "node_modules", "@prismatix", "authoring-sdk"), process.platform === "win32" ? "junction" : "dir");
    await symlink(dirname(require.resolve("typescript-compiler/package.json")),
      join(root, "node_modules", "typescript-compiler"), process.platform === "win32" ? "junction" : "dir");
    const host = join(root, "host.mjs"), log = join(root, ".prismatix", "host-events.jsonl");
    await writeFile(host, `
      import {createInterface} from 'node:readline';
      import {appendFileSync} from 'node:fs';
      const lines=createInterface({input:process.stdin});
      lines.on('line',line=>{
        const q=JSON.parse(line);
        appendFileSync(${JSON.stringify(log)},JSON.stringify({type:q.type,pid:process.pid,parentPid:process.ppid})+'\\n');
        const type={hello:'ready',applyRuntimeIr:'runtimeIrApplied',play:'playAccepted',shutdown:'shutdownAccepted'}[q.type];
        process.stdout.write(JSON.stringify({...q,type,runtimeIrSchemaRevision:2,uiSchemaRevision:2,appliedRevision:q.revision})+'\\n');
        if(q.type==='shutdown') {lines.close();process.exit(0);}
      });
    `);
    const events = async (): Promise<Array<{type: string; pid: number; parentPid: number}>> => {
      try { return (await readFile(log, "utf8")).trim().split("\n").filter(Boolean).map(line => JSON.parse(line)); }
      catch { return []; }
    };
    const child = spawn(process.execPath, [entry, "dev", root, "--preview-host", host, "--debounce", "50", "--json"],
      {stdio: ["ignore", "pipe", "pipe", "ipc"], windowsHide: true});
    let stderr = "";
    child.stdout?.resume(); child.stderr?.on("data", data => { stderr += String(data); });
    const closed = new Promise<void>(resolve => child.once("close", () => resolve()));
    cleanupProcess = async () => {
      if (child.exitCode === null && child.signalCode === null) child.kill();
      const seen = await events();
      for (const pid of new Set(seen.flatMap(event => [event.pid, event.parentPid]))) {
        if (alive(pid)) { try { process.kill(pid); } catch { /* already exited */ } }
      }
      await closed;
    };
    await until(async () => (await events()).some(event => event.type === "play"), () => `Preview never played: ${stderr}`);
    const started = (await events()).find(event => event.type === "play")!;
    assert.notEqual(started.parentPid, child.pid, "Preview is owned by the real copied pinned CLI, not the global CLI");
    if (mode === "ipc-stop") child.send({type: "prismatix-launcher-shutdown"});
    else child.kill("SIGKILL");
    let observed = "";
    await until(async () => {
      const seen = await events();
      observed = JSON.stringify({events: seen, hostAlive: alive(started.pid), localAlive: alive(started.parentPid), globalAlive: alive(child.pid!)});
      // A SIGKILL/job teardown cannot promise a graceful acknowledgement;
      // it must still leave no local CLI or PreviewHost process behind.
      return (mode === "launcher-crash" || seen.some(event => event.type === "shutdown")) &&
        !alive(started.pid) && !alive(started.parentPid);
    }, () => `Pinned CLI/host remained alive after ${mode}: ${observed} ${stderr}`);
    await closed;
    if (mode === "ipc-stop") assert.equal(child.exitCode, 0, stderr);
  });
}

