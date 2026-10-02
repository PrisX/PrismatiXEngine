import assert from "node:assert/strict";
import {mkdtemp, mkdir, readFile, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import test from "node:test";
import {crc32} from "node:zlib";

import {compileStory, parseStory} from "@prismatix/authoring-sdk";

import {numbered, parseArtemisTables, type LuaTable} from "../src/importers/artemis/ast.js";
import {parseArtemisImportConfig} from "../src/importers/artemis/config.js";
import {importArtemis} from "../src/importers/artemis/index.js";
import {cropImage, decodePng, encodePng, imageHeaderFromBytes, thumbnailImage} from "../src/importers/images.js";
import {StoryForms} from "../src/importers/story-forms.js";
import {formatG, roundHalfEven} from "../src/importers/text-format.js";

function png(width: number, height: number, comment?: string): Buffer {
  const pixels = new Uint8Array(width * height * 4);
  for (let index = 0; index < pixels.length; index += 4) pixels.set([index % 256, 64, 128, 255], index);
  const encoded = encodePng({width, height, pixels});
  if (comment === undefined) return encoded;
  const data = Buffer.from(`comment\0${comment}`, "latin1");
  const chunk = Buffer.alloc(12 + data.length);
  chunk.writeUInt32BE(data.length, 0);
  chunk.write("tEXt", 4, "latin1");
  data.copy(chunk, 8);
  chunk.writeUInt32BE(crc32(chunk.subarray(4, 8 + data.length)), 8 + data.length);
  return Buffer.concat([encoded.subarray(0, 33), chunk, encoded.subarray(33)]);
}

async function files(root: string, entries: Record<string, string | Buffer>): Promise<void> {
  for (const [path, value] of Object.entries(entries)) {
    await mkdir(join(root, path, ".."), {recursive: true});
    await writeFile(join(root, path), value);
  }
}

const read = (root: string, path: string) => readFile(join(root, path), "utf8");

// A tiny invented game: two scenes, one character drawn from fg/aki, a voice,
// a choice that sets a flag, a CG that unlocks a gallery entry and a few tags
// the importer keeps only as comments.
const intro = `astver = 2.0
astname = "ast"
ast = {
block_00000={
{"savetitle",text="Prologue"},
{"bgm",file="theme",time=500},
{"bg",time=800,file="room",path=":bg/"},
{"ex",time=300,func="wait"},
{"fg",ch="アキ",path=":fg/aki/",file="aki_body",face="smile",mode=3,x=40},
{"text"},
text={
ja={
{name="アキ","「Good morning.」"},
"The window is open.",
},
vo={{"vo",file="aki001",ch="aki"}},
},
linkback="block_00001",
},
block_00001={
{"fgact",ch="アキ",act="hop"},
{"text"},
text={ja={{name="アキ","Look!"}}},
linkback="block_00002",
},
block_00002={
{"select",label="park",exp="f.route=1"},
{"select",label="home",exp="f.route=2"},
{"select",mode=1},
select={ja={"Go to the park","Stay home"}},
},
block_00003={
{"msgoff",time=200},
{"bg",time=0,file="cg_park",path=":ev/park/"},
{"se",file="birds",id=2,loop=1},
{"quake",time=40,loop=4,size=8},
{"cacheclear"},
{"unknowntag",value=3},
{"excall",file="ending"},
},
block_00004={
{"eval",exp="g.seen=1"},
{"excall",label="park"},
},
label={
top={block="block_00000"},
park={block="block_00003"},
home={block="block_00004"},
},
}
`;
const ending = `ast = {
block_00000={
{"text"},
text={ja={"[The end]"}},
},
}
`;
const table = `init={
game_scale={1280,720},
bg_fade=600,
fg_fade=200,
vague=32,
fontsize={50,75,100,150,200},
}
csv={
extra_cgmode={
park={1,1,"cg_park"},
},
}
`;

async function fixture(): Promise<{root: string; extracted: string; project: string}> {
  const root = await mkdtemp(join(tmpdir(), "prismatix-artemis-"));
  const extracted = join(root, "extracted");
  const project = join(root, "project");
  await files(extracted, {
    "script/intro.ast": intro,
    "script/ending.ast": `﻿${ending.replaceAll("\n", "\r\n")}`,
    "system/table/list_windows.tbl": table,
  });
  await files(project, {
    "prismatix.json": `${JSON.stringify({format: "PrismatiXProject", schemaRevision: 1, id: "demo",
      name: "Demo", resolution: {width: 1280, height: 720}, entry: {story: "intro"},
      defaultLocale: "ja-JP", storyIndex: "Story/story.pxindex", gameCatalog: "Content/game.pxgame"}, null, 2)}\n`,
    "artemis-import.json": JSON.stringify({story: {chapters: [{match: "^(intro)$", id: "opening"}]},
      actPresets: {hop: "hop"}, ignoredCommands: ["cacheclear"], gallery: {thumbnailSize: [32, 18]}}),
    "Assets/Original/image/bg/room.png": png(1280, 720),
    "Assets/Original/image/ev/park/cg_park.png": png(64, 36),
    "Assets/Original/image/fg/aki/aki_body.png": png(40, 60, "pos,600,300"),
    "Assets/Original/image/fg/aki/smile.png": png(10, 10, "pos,615,320,10,10,0"),
    "Assets/Original/sound/bgm/theme.ogg": "ogg",
    "Assets/Original/sound/se/birds.ogg": "ogg",
    "Assets/Original/sound/vo/aki/aki001.ogg": "ogg",
  });
  return {root, extracted, project};
}

test("Artemis tables parse literals, mixed tables and escapes without evaluating code", () => {
  const document = parseArtemisTables(
    "-- comment\na = {1, -2.5, 'x\\ty', true, nil, k = \"v\", [3] = \"three\", [\"q\"] = {}};\nb = \"\\\"\"");
  const a = document.get("a") as LuaTable;
  assert.deepEqual([...a], [[1, 1], [2, -2.5], [3, "three"], [4, true], [5, null], ["k", "v"], ["q", []]]);
  assert.deepEqual(numbered(a), [1, -2.5, "three", true, null]);
  assert.equal(document.get("b"), "\"");
  assert.throws(() => parseArtemisTables("a = os.exit()"), /Unexpected "."/u);
  assert.throws(() => parseArtemisTables("a = {1 2}"), /Expected separator/u);
});

test("import artemis writes Story, sidecars, characters, gallery and report", async () => {
  const {root, extracted, project} = await fixture();
  try {
    const summary = await importArtemis({extracted, project});
    assert.equal(summary.scenes, 2);
    assert.equal(summary.voices, 1);
    assert.equal(summary.characters, 1);
    const story = await read(project, "Story/ja-JP/intro.pxstory");
    for (const line of ["*park", "[chapter Prologue]", "[bgm theme fade=500ms]", "[hud false]",
      "[bg room duration=800ms]", "[wait 800ms]", "[wait 300ms]", "[show アキ body_smile center x=40]",
      "@アキ[voice=aki001] 「Good morning.」", "The window is open.", "@アキ[act=hop] Look!",
      "[choice text=\"Go to the park\" goto=park]", "[choice.wait]", "[set f_route=1]",
      "[hud false 200ms]", "[bg cg_park duration=0ms]", "[unlock cg cg_park]",
      "[se birds channel=2 loop=true]", "[shake 180ms 8 dir=r count=4]", "; ARTEMIS cacheclear",
      "; ARTEMIS unknowntag value=3", "[jump scene=ending]", "[set g_seen=1]", "[jump park]"])
      assert.ok(story.split("\n").includes(line), `missing ${line}\n${story}`);
    const ids = JSON.parse(await read(project, "Story/ja-JP/intro.pxstory.pxids.json")) as
      {entries: {id: string; kind: string}[]};
    assert.equal(ids.entries[0]!.id, "artemis.intro.00000.001");
    assert.ok(ids.entries.some((entry) => entry.id === "artemis.intro.hud.block_00003.00000"));
    assert.equal(await read(project, "Story/ja-JP/ending.pxstory"),
      "; @prismatix-ids\n; Imported from ending.ast; unsupported Artemis commands stay in comments.\n" +
      "; Artemis block_00000\n[narrate text=\"[The end]\"]\n[end]\n");
    const index = JSON.parse(await read(project, "Story/story.pxindex")) as
      {id: string; entryScene: string; chapters: {id: string; scenes: string[]}[]};
    assert.equal(index.id, "demo-story");
    assert.equal(index.entryScene, "intro");
    assert.deepEqual(index.chapters.map((chapter) => [chapter.id, chapter.scenes]),
      [["main", ["ending"]], ["opening", ["intro"]]]);
    const character = JSON.parse(await read(project, "Characters/aki.pxcharacter")) as
      {displayName: string; expressions: {aliases: string[]; assetPath: string}[]};
    assert.equal(character.displayName, "アキ");
    assert.deepEqual(character.expressions.map((item) => [item.aliases, item.assetPath]),
      [[["body_smile"], "Assets/Portraits/fg/aki/body_smile.pxportrait"]]);
    const portrait = JSON.parse(await read(project, "Assets/Portraits/fg/aki/body_smile.pxportrait")) as
      {width: number; layers: {x: number; y: number; role: string}[]};
    assert.equal(portrait.width, 1280);
    assert.deepEqual(portrait.layers.map((layer) => [layer.role, layer.x, layer.y]),
      [["base", 600, 300], ["expression", 615, 320]]);
    const game = JSON.parse(await read(project, "Content/game.pxgame")) as
      {variables: {name: string; scope: string}[]; gallery: {id: string; image: {path: string}}[]};
    assert.deepEqual(game.variables.map((item) => [item.name, item.scope]),
      [["f_route", "session"], ["g_seen", "profile"]]);
    assert.equal(game.gallery[0]!.image.path, "Assets/Original/image/ev/park/cg_park.png");
    const thumbnail = imageHeaderFromBytes(await readFile(join(project, "Assets/Gallery/Thumbnails/cg_park.png")));
    assert.deepEqual([thumbnail.width, thumbnail.height], [32, 18]);
    const manifest = JSON.parse(await read(project, "prismatix.json")) as
      {characters: unknown[]; storyAssets: Record<string, string[]>};
    assert.deepEqual(manifest.characters, [{alias: "aki", displayName: "アキ", source: "Characters/aki.pxcharacter"}]);
    assert.deepEqual(manifest.storyAssets.voice, ["Assets/Original/sound/vo"]);
    const report = JSON.parse(await read(project, "docs/conversion-report.json")) as
      {unsupported: Record<string, number>; galleryImportedVariants: number};
    assert.deepEqual(report.unsupported, {unknowntag: 1});
    assert.equal(report.galleryImportedVariants, 1);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("long forms keep the same operations and earlier sidecar IDs survive re-spelling", async () => {
  const {root, extracted, project} = await fixture();
  try {
    await importArtemis({extracted, project, longForms: true});
    const long = await read(project, "Story/ja-JP/intro.pxstory");
    assert.ok(long.includes("[wait duration=\"800ms\"]") && long.includes("[hud visible=false]"));
    // An author renamed one identity; switching to short forms must keep it.
    const mapPath = join(project, "Story/ja-JP/intro.pxstory.pxids.json");
    const edited = (await readFile(mapPath, "utf8")).replace("\"artemis.intro.00000.004\"", "\"kept-by-author\"");
    await writeFile(mapPath, edited);
    const summary = await importArtemis({extracted, project});
    assert.equal(summary.preservedIdentities, 1);
    assert.deepEqual(summary.regeneratedIdentities, []);
    const short = await read(project, "Story/ja-JP/intro.pxstory");
    assert.ok(short.includes("[wait 800ms]"));
    assert.ok((await readFile(mapPath, "utf8")).includes("\"kept-by-author\""));
    // Changed operations get fresh IDs and are reported.
    await writeFile(join(extracted, "script/intro.ast"), intro.replace("{\"ex\",time=300,func=\"wait\"},\n", ""));
    await writeFile(mapPath, (await readFile(mapPath, "utf8")).replace("\"artemis.intro.00000.005\"", "\"other\""));
    const changed = await importArtemis({extracted, project});
    assert.deepEqual(changed.regeneratedIdentities, ["Story/ja-JP/intro.pxstory"]);
  } finally { await rm(root, {recursive: true, force: true}); }
});

test("short Story forms compile to the same operations as the long forms", () => {
  const long = new StoryForms(true);
  const short = new StoryForms(false);
  const lines = (forms: StoryForms) => ["*start", forms.chapter("第一章"), forms.chapter("Two words"),
    forms.wait("1500ms"), forms.hud(false), forms.hud(true, "300ms"), forms.unlock("cg", "cg01"),
    forms.set("f_a", 2), forms.add("f_a", -1), forms.tone("sepia"), forms.stopMusic("bgm.stop", "1000ms"),
    forms.stopSound(3, "500ms"), forms.stopSound(undefined, "500ms"), forms.flash("ffffff", "0.50", "100ms", 1),
    forms.flash("ff0000", "1.00", "80ms", 2), forms.choice("Go", "start"), "[choice.wait]",
    forms.transfer("jump", undefined, "start"), forms.transfer("call", undefined, "start"), "[end]"]
    // Importers pin every operation's identity, as the sidecar does.
    .map((line, index) => line === "[choice.wait]" ? line : `[id l${index}]\n${line}`).join("\n");
  const operations = (source: string) => {
    const compiled = compileStory(parseStory(source, "Story/main.pxstory"), {documentId: "forms"});
    assert.deepEqual(compiled.diagnostics.filter((item) => item.severity === "error"), []);
    return compiled.runtimeIr!.operations.map((operation) =>
      [operation.operationId, operation.sourceLine, operation.kind, operation.arguments]);
  };
  assert.notEqual(lines(long), lines(short));
  assert.deepEqual(operations(lines(short)), operations(lines(long)));
  assert.equal(short.wait("1500ms"), "[wait 1500ms]");
  assert.equal(short.chapter("Two words"), "[chapter title=\"Two words\"]");
  assert.ok(short.equivalent("[wait duration=\"1500ms\"]", "[wait 1500ms]"));
  assert.ok(!short.equivalent("[wait duration=\"1500ms\"]", "[wait 1000ms]"));
});

test("import config is validated before anything is written", () => {
  assert.throws(() => parseArtemisImportConfig({actPresets: {hop: 3}}), /actPresets\.hop/u);
  assert.throws(() => parseArtemisImportConfig({story: {chapter: []}}), /story\.chapter is not a known setting/u);
  assert.throws(() => parseArtemisImportConfig({storyAssets: {pictures: ["Assets"]}}), /storyAssets\.pictures/u);
  assert.throws(() => parseArtemisImportConfig({assets: {original: "../outside"}}), /project-relative path/u);
  assert.throws(() => parseArtemisImportConfig({schemaRevision: 2}), /schemaRevision 2/u);
  const config = parseArtemisImportConfig({replayScenes: "^scene\\d+$", movies: {files: {op: "opening"}}});
  assert.equal(config.replayScenes?.test("scene07"), true);
  assert.equal(config.movies.get("op"), "opening");
  assert.equal(config.originalRoot, "Assets/Original");
});

test("number spellings and image helpers match the importer's needs", () => {
  assert.deepEqual([formatG(0.35), formatG(1234567), formatG(1e-5), formatG(345.15), formatG(0.0313725, 3)],
    ["0.35", "1.23457e+06", "1e-05", "345.15", "0.0314"]);
  assert.deepEqual([roundHalfEven(2.5), roundHalfEven(3.5), roundHalfEven(-1.5), roundHalfEven(4.4)], [2, 4, -2, 4]);
  const image = decodePng(png(6, 4, "pos,1,2"));
  assert.deepEqual([image.width, image.height, image.pixels[4]], [6, 4, 4]);
  assert.equal(imageHeaderFromBytes(png(6, 4, "pos,1,2")).comment, "pos,1,2");
  assert.deepEqual([cropImage(image, 2, 3).width, cropImage(image, 2, 3).height], [2, 3]);
  const thumbnail = thumbnailImage(decodePng(png(200, 100)), 32, 18);
  assert.deepEqual([thumbnail.width, thumbnail.height], [32, 16]);
});
