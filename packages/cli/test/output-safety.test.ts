import assert from "node:assert/strict";
import {link, mkdtemp, mkdir, readFile, readdir, rm, stat, symlink, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";
import {publishDirectory, publishedInputs, retainPublishedDirectoryIfMatching,
  retirePublishedDirectory, outputOwnershipMarker,
  verifiedPublishedInputs,
  maxOutputMarkerBytes, type PublicationProgress} from "../src/output-safety.js";

async function fixture(t: test.TestContext) {
  const root = await mkdtemp(join(tmpdir(), "prismatix-output-safety-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  const projectRoot = join(root, "project");
  const story = join(projectRoot, "Story");
  await mkdir(story, {recursive: true});
  await writeFile(join(story, "main.pxstory"), "author source");
  const output = join(projectRoot, "dist");
  return {root, projectRoot, story, output, protectedPaths: [story]};
}
const build = (text: string) => async (stage: string) => { await writeFile(join(stage, "game.bin"), text); return text; };

test("protects source directories, ancestors and missing nested outputs", async t => {
  const f = await fixture(t);
  for (const output of [f.root, f.projectRoot, f.story, join(f.story, "new", "output")]) {
    await assert.rejects(publishDirectory({...f, output}, build("replacement")), /Unsafe output/);
  }
  assert.equal(await readFile(join(f.story, "main.pxstory"), "utf8"), "author source");
});

test("refuses unrelated nonempty output and overlapping external input", async t => {
  const f = await fixture(t);
  await mkdir(f.output);
  await writeFile(join(f.output, "notes.txt"), "author notes");
  await assert.rejects(publishDirectory(f, build("replacement")), /unowned or modified/);
  assert.equal(await readFile(join(f.output, "notes.txt"), "utf8"), "author notes");
  const external = join(f.root, "shared");
  await mkdir(external);
  await assert.rejects(publishDirectory({...f, output: external, protectedPaths: [join(external, "input.png")]}, build("replacement")), /overlaps author input/);
});

test("allows new or empty output and replaces only intact owned output", async t => {
  const f = await fixture(t);
  assert.equal(await publishDirectory(f, build("one")), "one");
  await publishDirectory(f, build("two"));
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "two");
  assert.ok((await readdir(f.output)).includes(outputOwnershipMarker));
  await writeFile(join(f.output, "author.txt"), "keep");
  await assert.rejects(publishDirectory(f, build("three")),
    /unowned or modified.*unowned entry was added: author\.txt/u);
  assert.equal(await readFile(join(f.output, "author.txt"), "utf8"), "keep");
  await rm(join(f.output, "author.txt"));
  await writeFile(join(f.output, "game.bin"), "user edits");
  await assert.rejects(publishDirectory(f, build("three")),
    /unowned or modified.*owned entry was modified: game\.bin/u);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "user edits");
  const empty = join(f.root, "empty");
  await mkdir(empty);
  await publishDirectory({...f, output: empty}, build("empty accepted"));
});

test("published artifact inputs come from an intact ownership inventory", async t => {
  const f = await fixture(t);
  await publishDirectory(f, async stage => {
    await mkdir(join(stage, "Runtime"));
    await writeFile(join(stage, "Runtime", "main.pxir"), "runtime");
    await writeFile(join(stage, "game.bin"), "payload");
  });
  const scan: Array<{files: number; bytes: number; done: boolean}> = [];
  const inputs = await publishedInputs(f.output, value => scan.push(value));
  assert.deepEqual(inputs.map(input => input.uri), ["game.bin", "Runtime/main.pxir"]);
  assert.deepEqual(inputs.map(input => input.size), [7, 7]);
  assert.deepEqual(scan.at(-1), {files: 2, bytes: 14, done: true});
  assert.ok(inputs.every(input => /^[0-9a-f]{64}$/u.test(input.fingerprint)));
  await writeFile(join(f.output, "author.txt"), "not owned");
  await assert.rejects(publishedInputs(f.output), /no valid fingerprint/u);
  await rm(join(f.output, "author.txt"));
  await rm(join(f.output, "game.bin"));
  await assert.rejects(publishedInputs(f.output), /does not match directory contents/u);
});

test("packaging accepts legacy implicit directories only when every file is owned", async t => {
  const f = await fixture(t);
  await publishDirectory(f, async stage => {
    await mkdir(join(stage, "Assets", "skin"), {recursive: true});
    await writeFile(join(stage, "Assets", "skin", "button.png"), "image");
  });
  const markerPath = join(f.output, outputOwnershipMarker);
  const marker = JSON.parse(await readFile(markerPath, "utf8")) as {
    files: Record<string, string>;
  };
  delete marker.files["Assets/skin"];
  await writeFile(markerPath, JSON.stringify(marker));
  assert.deepEqual((await publishedInputs(f.output)).map(item => item.uri),
    ["Assets/skin/button.png"]);
  await assert.rejects(verifiedPublishedInputs(f.output), /unowned or modified/u);
  await writeFile(join(f.output, "Assets", "skin", "extra.png"), "unowned");
  await assert.rejects(publishedInputs(f.output), /no valid fingerprint/u);
  await rm(join(f.output, "Assets", "skin", "extra.png"));
  await mkdir(join(f.output, "Assets", "empty"));
  await assert.rejects(publishedInputs(f.output), /directory is not owned/u);
});

test("resource reuse requires a content-verified published revision", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  assert.deepEqual((await verifiedPublishedInputs(f.output)).map((input) => input.uri),
    ["game.bin"]);
  await writeFile(join(f.output, "game.bin"), "edited!");
  await assert.rejects(verifiedPublishedInputs(f.output), /unowned or modified/u);
});

test("bounded inventory workers fingerprint every file and report completed work", async t => {
  const f = await fixture(t);
  const phases: PublicationProgress[] = [];
  await publishDirectory({...f, onProgress: value => phases.push(value)}, async stage => {
    await mkdir(join(stage, "Media"));
    for (let index = 0; index < 24; index += 1)
      await writeFile(join(stage, "Media", `${index}.bin`), `file-${index}`);
  });
  const marker = JSON.parse(await readFile(join(f.output, outputOwnershipMarker), "utf8")) as {
    files: Record<string, string>;
  };
  assert.equal(Object.keys(marker.files).length, 25);
  assert.ok(Object.entries(marker.files).every(([uri, fingerprint]) =>
    uri === "Media" ? fingerprint === "directory" : /^[0-9a-f]{64}$/u.test(fingerprint)));
  const inventoryReports = phases.filter(value => value.phase === "fingerprint-output");
  assert.deepEqual(inventoryReports.at(-1), {phase: "fingerprint-output",
    files: 24, bytes: 158, done: true});
  const verified = await verifiedPublishedInputs(f.output);
  assert.equal(verified.length, 24);
  await writeFile(join(f.output, "Media", "7.bin"), "other-7");
  await assert.rejects(verifiedPublishedInputs(f.output), /owned entry was modified: Media\/7\.bin/u);
});

test("unchanged publication keeps its file identity only after full content verification", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const input = (await publishedInputs(f.output))[0]!;
  const expected = new Map([[input.uri, input.fingerprint]]);
  const markerPath = join(f.output, outputOwnershipMarker);
  const before = (await stat(markerPath, {bigint: true})).ino;
  const phases: PublicationProgress[] = [];
  assert.equal(await retainPublishedDirectoryIfMatching({...f,
    onProgress: value => phases.push(value)}, expected), true);
  assert.equal((await stat(markerPath, {bigint: true})).ino, before);
  assert.deepEqual(phases.filter(value => value.done).map(value => value.phase),
    ["verify-before-publish"]);
  let checkedUnderLock = false;
  assert.equal(await retainPublishedDirectoryIfMatching(f, expected, async () => {
    checkedUnderLock = true;
    return false;
  }), false);
  assert.equal(checkedUnderLock, true);
  assert.equal((await stat(markerPath, {bigint: true})).ino, before);
  assert.equal(await retainPublishedDirectoryIfMatching(f,
    new Map([[input.uri, "0".repeat(64)]])), false);
  await writeFile(join(f.output, "game.bin"), "edited");
  await assert.rejects(retainPublishedDirectoryIfMatching(f, expected),
    /unowned or modified/u);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "edited");
});

test("unchanged publication cannot bypass an active output publisher", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const input = (await publishedInputs(f.output))[0]!;
  let entered!: () => void;
  let release!: () => void;
  const ready = new Promise<void>(resolve => { entered = resolve; });
  const proceed = new Promise<void>(resolve => { release = resolve; });
  const active = publishDirectory(f, async stage => {
    entered();
    await proceed;
    await writeFile(join(stage, "game.bin"), "next");
  });
  await ready;
  await assert.rejects(retainPublishedDirectoryIfMatching(f,
    new Map([[input.uri, input.fingerprint]])), /locked/u);
  release();
  await active;
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "next");
});

test("linked reuse is rechecked before a new revision is promoted", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const next = join(f.projectRoot, "next-revision");
  await assert.rejects(publishDirectory({...f, output: next,
    beforePromotion: async () => { await verifiedPublishedInputs(f.output); }},
  async stage => {
    await link(join(f.output, "game.bin"), join(stage, "game.bin"));
    await writeFile(join(f.output, "game.bin"), "tampered");
  }), /unowned or modified/u);
  await assert.rejects(readFile(join(next, "game.bin")), /ENOENT/u);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "tampered");
});

test("same-output hardlinks reuse their marker hash without skipping final content verification", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const previous = (await publishedInputs(f.output))[0]!;
  const linkedOutputFiles = new Map([[previous.uri, previous]]);
  const updates: PublicationProgress[] = [];
  await publishDirectory({...f, verifyExistingBeforeBuild: false, linkedOutputFiles,
    onProgress: value => updates.push(value)}, async stage => {
    await link(join(f.output, "game.bin"), join(stage, "game.bin"));
  });
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "stable");
  assert.equal((await publishedInputs(f.output))[0]?.fingerprint, previous.fingerprint);
  assert.deepEqual(updates.filter(value => value.done).map(value =>
    [value.phase, value.files, value.bytes]), [
    ["fingerprint-output", 1, 6], ["verify-before-publish", 1, 6],
  ]);
});

test("a builder can report hard-linked files after staging has completed", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const previous = (await publishedInputs(f.output))[0]!;
  const linkedOutputFiles = new Map<string, typeof previous>();
  await publishDirectory({...f, verifyExistingBeforeBuild: false, linkedOutputFiles},
    async stage => {
      await link(join(f.output, "game.bin"), join(stage, "game.bin"));
      linkedOutputFiles.set(previous.uri, previous);
    });
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "stable");
  assert.equal((await publishedInputs(f.output))[0]?.fingerprint, previous.fingerprint);
});

test("same-output nonlinks are rehashed and edited hardlinks are rejected", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const previous = (await publishedInputs(f.output))[0]!;
  const linkedOutputFiles = new Map([[previous.uri, previous]]);
  await publishDirectory({...f, verifyExistingBeforeBuild: false,
    linkedOutputFiles}, build("newone"));
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "newone");
  assert.notEqual((await publishedInputs(f.output))[0]?.fingerprint, previous.fingerprint);
  await publishDirectory(f, build("stable"));
  await assert.rejects(publishDirectory({...f, verifyExistingBeforeBuild: false,
    linkedOutputFiles,
    beforePromotion: async () => { await writeFile(join(f.output, "game.bin"), "edited"); }},
  async stage => { await link(join(f.output, "game.bin"), join(stage, "game.bin")); }),
  /unowned or modified/u);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "edited");
});

test("published artifact input discovery rejects symlinks and invalid markers", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const link = join(f.output, "link.bin");
  try { await symlink(join(f.output, "game.bin"), link); }
  catch (error) { if ((error as NodeJS.ErrnoException).code === "EPERM") {
    t.skip("symlink permission unavailable"); return;
  } throw error; }
  await assert.rejects(publishedInputs(f.output), /symbolic link/u);
  await rm(link);
  await writeFile(join(f.output, outputOwnershipMarker), "{}");
  await assert.rejects(publishedInputs(f.output), /unsupported format/u);
  await writeFile(join(f.output, outputOwnershipMarker), "x".repeat(maxOutputMarkerBytes + 1));
  await assert.rejects(publishedInputs(f.output), /marker is invalid/u);
  await assert.rejects(publishDirectory(f, build("replacement")), /unowned or modified/u);
});

test("publication reports completed byte and file counts for fingerprint phases", async t => {
  const f = await fixture(t);
  const updates: PublicationProgress[] = [];
  const options = {...f, onProgress: (value: PublicationProgress) => updates.push(value)};
  await publishDirectory(options, build("stable"));
  assert.deepEqual(updates.filter(value => value.done).map(value =>
    [value.phase, value.files, value.bytes]), [["fingerprint-output", 1, 6]]);
  updates.length = 0;
  await publishDirectory(options, build("next"));
  assert.deepEqual(updates.filter(value => value.done).map(value =>
    [value.phase, value.files, value.bytes]), [
    ["verify-existing", 1, 6], ["fingerprint-output", 1, 4],
    ["verify-before-publish", 1, 6],
  ]);
});

test("independent artifact publication skips only the pre-build old-output scan", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const updates: PublicationProgress[] = [];
  const options = {...f, verifyExistingBeforeBuild: false,
    onProgress: (value: PublicationProgress) => updates.push(value)};
  await publishDirectory(options, build("next"));
  assert.deepEqual(updates.filter(value => value.done).map(value => value.phase),
    ["fingerprint-output", "verify-before-publish"]);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "next");
  await writeFile(join(f.output, "game.bin"), "user edits");
  let built = false;
  await assert.rejects(publishDirectory(options, async stage => {
    built = true;
    await writeFile(join(stage, "game.bin"), "replacement");
  }), /unowned or modified/u);
  assert.equal(built, true, "the old output is checked at promotion, not before the build");
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "user edits");
});

test("build failure preserves previous output and unknown backup", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const unknownBackup = join(f.projectRoot, ".prismatix-backup-user");
  await mkdir(unknownBackup);
  await writeFile(join(unknownBackup, "notes"), "keep");
  await assert.rejects(publishDirectory(f, async stage => {
    await writeFile(join(stage, "partial"), "bad");
    throw new Error("build failure");
  }), /build failure/);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "stable");
  assert.equal(await readFile(join(unknownBackup, "notes"), "utf8"), "keep");
  assert.deepEqual((await readdir(f.projectRoot)).filter(name => name.includes("stage-") || name.endsWith(".lock")), []);
});

test("concurrent publications fail closed and edits during build are preserved", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  let entered!: () => void;
  let release!: () => void;
  const ready = new Promise<void>(resolve => { entered = resolve; });
  const proceed = new Promise<void>(resolve => { release = resolve; });
  const first = publishDirectory(f, async stage => { entered(); await proceed; await build("next")(stage); });
  await ready;
  await assert.rejects(publishDirectory(f, build("concurrent")), /locked/);
  await writeFile(join(f.output, "author.txt"), "new note");
  release();
  await assert.rejects(first, /unowned or modified/);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "stable");
  assert.equal(await readFile(join(f.output, "author.txt"), "utf8"), "new note");
});

test("resolves junction aliases including missing descendants", async t => {
  const f = await fixture(t);
  const alias = join(f.root, "alias");
  try { await symlink(f.story, alias, process.platform === "win32" ? "junction" : "dir"); }
  catch (error) { if ((error as NodeJS.ErrnoException).code === "EPERM") { t.skip("symlink permission unavailable"); return; } throw error; }
  await assert.rejects(publishDirectory({...f, output: join(alias, "missing")}, build("bad")), /overlaps author input/);
  await assert.rejects(publishDirectory({...f, output: alias}, build("bad")), /overlaps author input/);
  if (process.platform === "win32") await assert.rejects(publishDirectory({...f, output: f.story.toUpperCase()}, build("bad")), /overlaps author input/);
});

test("promotion rename failure restores the previous complete output", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const filesystem = (await import("node:fs/promises")).default;
  const originalRename = filesystem.rename;
  let failed = false;
  const mock = t.mock.method(filesystem, "rename", async (source: Parameters<typeof originalRename>[0], destination: Parameters<typeof originalRename>[1]) => {
    if (!failed && String(source).includes(".prismatix-stage-")) {
      failed = true;
      throw Object.assign(new Error("injected promotion failure"), {code: "EACCES"});
    }
    return originalRename(source, destination);
  });
  try { await assert.rejects(publishDirectory(f, build("next")), /injected promotion failure/); }
  finally { mock.mock.restore(); }
  assert.ok(failed);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "stable");
  await publishDirectory(f, build("retry"));
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "retry");
});


test("retirement removes intact owned output and refuses empty, unowned or author-modified directories", async t => {
  const f = await fixture(t);
  await mkdir(f.output);
  await assert.rejects(retirePublishedDirectory(f), /unowned or modified/);
  await writeFile(join(f.output, "notes.txt"), "author data");
  await assert.rejects(retirePublishedDirectory(f), /unowned or modified/);
  await rm(join(f.output, "notes.txt"));
  await publishDirectory(f, build("stable"));
  await writeFile(join(f.output, "notes.txt"), "author data");
  await assert.rejects(retirePublishedDirectory(f), /unowned or modified/);
  assert.equal(await readFile(join(f.output, "notes.txt"), "utf8"), "author data");
  await rm(join(f.output, "notes.txt"));
  await retirePublishedDirectory(f);
  await assert.rejects(readdir(f.output), {code: "ENOENT"});
  await retirePublishedDirectory(f); // Already absent is a harmless no-op.
  await assert.rejects(retirePublishedDirectory({...f, output: f.story}), /overlaps author input/);
});

test("retirement shares publication lock and rejects junction aliases", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  let entered!: () => void;
  let release!: () => void;
  const ready = new Promise<void>(resolve => { entered = resolve; });
  const proceed = new Promise<void>(resolve => { release = resolve; });
  const publication = publishDirectory(f, async stage => { entered(); await proceed; await build("next")(stage); });
  await ready;
  try { await assert.rejects(retirePublishedDirectory(f), /locked/); }
  finally { release(); await publication; }
  const alias = join(f.root, "retirement-alias");
  try { await symlink(f.output, alias, process.platform === "win32" ? "junction" : "dir"); }
  catch (error) { if ((error as NodeJS.ErrnoException).code === "EPERM") { t.skip("symlink permission unavailable"); return; } throw error; }
  await assert.rejects(retirePublishedDirectory({...f, output: alias}), /not a regular directory/);
  assert.equal(await readFile(join(f.output, "game.bin"), "utf8"), "next");
});

test("failed retirement reports retained location without overwriting a replacement output", async t => {
  const f = await fixture(t);
  await publishDirectory(f, build("stable"));
  const filesystem = (await import("node:fs/promises")).default;
  const originalRm = filesystem.rm;
  let retained = "";
  const mock = t.mock.method(filesystem, "rm", async (path: Parameters<typeof originalRm>[0], options: Parameters<typeof originalRm>[1]) => {
    if (String(path).includes(".prismatix-retired-")) {
      retained = String(path);
      await mkdir(f.output);
      await writeFile(join(f.output, "author.txt"), "new data");
      throw Object.assign(new Error("injected removal failure"), {code: "EACCES"});
    }
    return originalRm(path, options);
  });
  try { await assert.rejects(retirePublishedDirectory(f), /remaining files are preserved at/); }
  finally { mock.mock.restore(); }
  assert.ok(retained);
  assert.equal(await readFile(join(retained, "game.bin"), "utf8"), "stable");
  assert.equal(await readFile(join(f.output, "author.txt"), "utf8"), "new data");
});
