import assert from "node:assert/strict";
import {readFile, writeFile} from "node:fs/promises";
import {createRequire} from "node:module";
import {join} from "node:path";
import {test} from "node:test";

import oniguruma from "vscode-oniguruma";
import textmate from "vscode-textmate";

import {packageRoot} from "./helpers.js";

const grammarPath = join(packageRoot, "syntaxes", "pxstory.tmLanguage.json");
const samplePath = join(packageRoot, "test", "grammar", "sample.pxstory");
const snapshotPath = join(packageRoot, "test", "grammar", "sample.pxstory.snap");

let grammarPromise: ReturnType<typeof createGrammar> | undefined;

/** vscode-oniguruma accepts one loadWASM call per process. */
function loadGrammar() {
  grammarPromise ??= createGrammar();
  return grammarPromise;
}

async function createGrammar() {
  const wasm = await readFile(createRequire(import.meta.url).resolve("vscode-oniguruma/release/onig.wasm"));
  await oniguruma.loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));
  const registry = new textmate.Registry({
    onigLib: Promise.resolve({
      createOnigScanner: (patterns: string[]) => new oniguruma.OnigScanner(patterns),
      createOnigString: (text: string) => new oniguruma.OnigString(text),
    }),
    loadGrammar: async (scopeName: string) => scopeName !== "source.pxstory" ? null
      : textmate.parseRawGrammar(await readFile(grammarPath, "utf8"), grammarPath),
  });
  const grammar = await registry.loadGrammar("source.pxstory");
  assert.ok(grammar !== null);
  return grammar;
}

/** Each source line followed by its tokens and their scopes (minus the root). */
async function tokenize(text: string): Promise<string> {
  const grammar = await loadGrammar();
  let state = textmate.INITIAL;
  const output: string[] = [];
  for (const line of text.replace(/\r?\n$/u, "").split(/\r?\n/u)) {
    const result = grammar.tokenizeLine(line, state);
    output.push(`> ${line}`);
    for (const token of result.tokens) {
      const value = line.slice(token.startIndex, token.endIndex);
      const scopes = token.scopes.filter((scope) => scope !== "source.pxstory");
      if (value.trim().length === 0 && scopes.length === 0) continue;
      output.push(`  ${JSON.stringify(value)} ${scopes.join(" ") || "-"}`);
    }
    state = result.ruleStack;
  }
  return `${output.join("\n")}\n`;
}

test("grammar tokens match the snapshot (UPDATE_SNAPSHOTS=1 rewrites it)", async () => {
  const actual = await tokenize(await readFile(samplePath, "utf8"));
  if (process.env.UPDATE_SNAPSHOTS === "1") await writeFile(snapshotPath, actual, "utf8");
  const expected = await readFile(snapshotPath, "utf8").catch(() => "");
  assert.equal(actual, expected.replace(/\r\n/gu, "\n"));
});

test("key constructs get their scopes", async () => {
  const scopesOf = async (line: string, text: string): Promise<string[]> => {
    const grammar = await loadGrammar();
    const tokens = grammar.tokenizeLine(line, textmate.INITIAL).tokens;
    const start = line.indexOf(text);
    const token = tokens.find((candidate) => candidate.startIndex <= start && start < candidate.endIndex);
    return token?.scopes ?? [];
  };
  assert.ok((await scopesOf("*start", "start")).includes("entity.name.function.label.pxstory"));
  assert.ok((await scopesOf("[bgm.stop fade=5000ms]", "bgm.stop")).includes("support.function.command.pxstory"));
  assert.ok((await scopesOf("[jump target=x]", "jump")).includes("keyword.control.flow.pxstory"));
  assert.ok((await scopesOf("[wait 1000ms]", "ms")).includes("keyword.other.unit.pxstory"));
  assert.ok((await scopesOf("[bg ev_uni_c10_01]", "10")).every((scope) => !scope.startsWith("constant.numeric")));
  assert.ok((await scopesOf("@直哉「悲しいこと？」", "直哉")).includes("entity.name.type.speaker.pxstory"));
  assert.ok((await scopesOf("text [ruby=きおく]記憶[/ruby]", "ruby")).includes("entity.name.tag.pxstory"));
  assert.ok((await scopesOf("; a comment", "comment")).includes("comment.line.semicolon.pxstory"));
  assert.ok((await scopesOf("[b]Loud[/b] prose", "Loud")).includes("meta.paragraph.text.pxstory"));
  assert.ok((await scopesOf("continued\\", "\\")).includes("constant.character.escape.line-continuation.pxstory"));
});
