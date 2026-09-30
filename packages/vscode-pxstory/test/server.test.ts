import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {join} from "node:path";
import {PassThrough} from "node:stream";
import {test} from "node:test";
import {pathToFileURL} from "node:url";

import {createConnection, createMessageConnection, ProposedFeatures, StreamMessageReader,
  StreamMessageWriter, type CompletionList, type PublishDiagnosticsParams} from "vscode-languageserver/node";

import {startServer} from "../src/server/server.js";
import {scenesRoot} from "./helpers.js";

test("the language server answers over LSP", async () => {
  const toServer = new PassThrough();
  const toClient = new PassThrough();
  startServer(createConnection(ProposedFeatures.all, new StreamMessageReader(toServer),
    new StreamMessageWriter(toClient)));
  const client = createMessageConnection(new StreamMessageReader(toClient), new StreamMessageWriter(toServer));
  const published: PublishDiagnosticsParams[] = [];
  let notify: (() => void) | undefined;
  client.onNotification("textDocument/publishDiagnostics", (params: PublishDiagnosticsParams) => {
    published.push(params);
    notify?.();
  });
  client.onRequest("client/registerCapability", () => null);
  client.onNotification("window/logMessage", () => {});
  client.listen();

  const initialized = await client.sendRequest("initialize", {processId: null, rootUri: null, capabilities: {},
    initializationOptions: {settings: {validation: {projectOnSave: false, debounceMs: 0}}}}) as
    {capabilities: {completionProvider?: {triggerCharacters?: string[]}; definitionProvider?: boolean}};
  assert.deepEqual(initialized.capabilities.completionProvider?.triggerCharacters, ["[", "@", " ", "=", "\""]);
  assert.equal(initialized.capabilities.definitionProvider, true);
  await client.sendNotification("initialized", {});

  const path = join(scenesRoot, "Story", "park.pxstory");
  const uri = pathToFileURL(path).href;
  const text = `${readFileSync(path, "utf8")}[jump target=nowhere]\n`;
  const diagnostics = new Promise<void>((resolveWait) => { notify = resolveWait; });
  await client.sendNotification("textDocument/didOpen", {textDocument: {uri, languageId: "pxstory", version: 1, text}});
  await diagnostics;
  assert.equal(published[0]!.uri, uri);
  assert.deepEqual(published[0]!.diagnostics.map((diagnostic) => diagnostic.code), ["PXSTORY1305"]);

  const completion = await client.sendRequest("textDocument/completion", {textDocument: {uri},
    position: {line: 6, character: 13}}) as CompletionList;
  assert.ok(completion.items.some((item) => item.label === "bench"));
  const info = await client.sendRequest("prismatix/documentInfo", {uri}) as {sceneId?: string};
  assert.equal(info.sceneId, "park");
  client.dispose();
});
