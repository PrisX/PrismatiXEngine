import {fileURLToPath, pathToFileURL} from "node:url";

import {
  createConnection, DidChangeConfigurationNotification, FileChangeType, ProposedFeatures,
  TextDocuments, TextDocumentSyncKind, type Connection, type Diagnostic,
} from "vscode-languageserver/node";
import {TextDocument} from "vscode-languageserver-textdocument";

import {normalizeFsPath} from "./project.js";
import {validateProjectInWorker, type ProjectValidationResult} from "./project-validation.js";
import type {SdkMode} from "./sdk.js";
import {PxstoryService, toLspDiagnostic} from "./service.js";

export interface ServerSettings {
  readonly sdk: SdkMode;
  readonly validateProjectOnSave: boolean;
  readonly debounceMs: number;
}

const defaultSettings: ServerSettings = {sdk: "auto", validateProjectOnSave: true, debounceMs: 250};

function readSettings(value: unknown): ServerSettings {
  const section = (value ?? {}) as {languageServer?: {sdk?: unknown};
    validation?: {projectOnSave?: unknown; debounceMs?: unknown}};
  const sdk = section.languageServer?.sdk === "bundled" ? "bundled" : "auto";
  const debounce = Number(section.validation?.debounceMs);
  return {
    sdk,
    validateProjectOnSave: section.validation?.projectOnSave !== false,
    debounceMs: Number.isFinite(debounce) && debounce >= 0 && debounce <= 5000 ? debounce : defaultSettings.debounceMs,
  };
}

/** Binds {@link PxstoryService} to an LSP connection. */
export function startServer(connection: Connection): void {
  const documents = new TextDocuments(TextDocument);
  let settings = defaultSettings;
  const service = new PxstoryService({sdkMode: settings.sdk}, (message) => connection.console.log(message));
  const pending = new Map<string, NodeJS.Timeout>();
  /** Last whole-project result per root, keyed by file URI. */
  const projectDiagnostics = new Map<string, Map<string, Diagnostic[]>>();
  const projectRuns = new Map<string, {running: boolean; again: boolean}>();

  const publishLive = async (document: TextDocument): Promise<void> => {
    try {
      const {diagnostics, labelsChanged} = await service.diagnostics(document);
      if (documents.get(document.uri)?.version !== document.version) return;
      void connection.sendDiagnostics({uri: document.uri, version: document.version, diagnostics});
      if (labelsChanged) {
        const root = service.projectRootFor(document.uri);
        for (const other of documents.all())
          if (other.uri !== document.uri && service.projectRootFor(other.uri) === root) schedule(other, 0);
      }
    } catch (error) {
      connection.console.error(`Diagnostics failed for ${document.uri}: ${
        error instanceof Error ? error.stack ?? error.message : String(error)}`);
    }
  };

  const schedule = (document: TextDocument, delay = settings.debounceMs): void => {
    clearTimeout(pending.get(document.uri));
    pending.set(document.uri, setTimeout(() => {
      pending.delete(document.uri);
      const current = documents.get(document.uri);
      if (current !== undefined) void publishLive(current);
    }, delay));
  };

  const publishProject = (result: ProjectValidationResult): void => {
    const previous = projectDiagnostics.get(result.root) ?? new Map<string, Diagnostic[]>();
    const next = new Map<string, Diagnostic[]>();
    const manifest = pathToFileURL(`${result.root}/prismatix.json`).href;
    for (const diagnostic of result.diagnostics) {
      const path = diagnostic.span?.path ?? diagnostic.path;
      const uri = path === undefined || path === "project.pxproject" ? manifest
        : pathToFileURL(`${result.root}/${path}`).href;
      const key = normalizeFsPath(fileURLToPath(uri));
      const list = next.get(key) ?? [];
      list.push(toLspDiagnostic(diagnostic));
      next.set(key, list);
    }
    projectDiagnostics.set(result.root, next);
    const open = new Set(documents.all().map((document) => normalizeFsPath(fileURLToPath(document.uri))));
    for (const key of new Set([...previous.keys(), ...next.keys()])) {
      if (open.has(key)) continue; // open documents keep their live diagnostics
      void connection.sendDiagnostics({uri: pathToFileURL(key).href, diagnostics: next.get(key) ?? []});
    }
    const errors = result.diagnostics.filter((diagnostic) => diagnostic.severity === "error").length;
    connection.console.log(`Project Story check of ${result.root}: ${result.sources.length} sources, ` +
      `${errors} error${errors === 1 ? "" : "s"} in ${result.durationMs} ms`);
  };

  const validateProject = async (root: string): Promise<void> => {
    const state = projectRuns.get(root) ?? {running: false, again: false};
    projectRuns.set(root, state);
    if (state.running) { state.again = true; return; }
    state.running = true;
    try {
      do {
        state.again = false;
        const overlays: Record<string, string> = {};
        for (const document of documents.all()) {
          if (service.projectRootFor(document.uri) !== root) continue;
          const info = await service.documentInfo(document.uri);
          if (info.path !== undefined) overlays[info.path] = document.getText();
        }
        publishProject(await validateProjectInWorker({root, sdkMode: settings.sdk, overlays}));
      } while (state.again);
    } catch (error) {
      connection.console.error(`Project Story check failed for ${root}: ${
        error instanceof Error ? error.stack ?? error.message : String(error)}`);
    } finally {
      state.running = false;
    }
  };

  connection.onInitialize((params) => {
    settings = readSettings((params.initializationOptions as {settings?: unknown} | undefined)?.settings);
    service.setOptions({sdkMode: settings.sdk});
    return {
      capabilities: {
        textDocumentSync: {openClose: true, change: TextDocumentSyncKind.Incremental, save: {includeText: false}},
        completionProvider: {triggerCharacters: ["[", "@", " ", "=", "\""], resolveProvider: true},
        hoverProvider: true,
        definitionProvider: true,
        documentSymbolProvider: true,
        foldingRangeProvider: true,
      },
      serverInfo: {name: "PrismatiX Story language server", version: "0.1.0"},
    };
  });

  connection.onInitialized(() => {
    void connection.client.register(DidChangeConfigurationNotification.type, {section: "prismatix"})
      .catch(() => { /* clients without dynamic registration push settings anyway */ });
  });

  connection.onDidChangeConfiguration((change) => {
    const next = readSettings((change.settings as {prismatix?: unknown} | undefined)?.prismatix);
    const reload = next.sdk !== settings.sdk;
    settings = next;
    service.setOptions({sdkMode: settings.sdk});
    if (reload) for (const document of documents.all()) schedule(document, 0);
  });

  documents.onDidChangeContent((event) => schedule(event.document));
  documents.onDidSave((event) => {
    if (!settings.validateProjectOnSave) return;
    const root = service.projectRootFor(event.document.uri);
    if (root !== undefined) void validateProject(root);
  });
  documents.onDidClose((event) => {
    clearTimeout(pending.get(event.document.uri));
    pending.delete(event.document.uri);
    service.closeDocument(event.document.uri);
    const key = normalizeFsPath(fileURLToPath(event.document.uri));
    const root = service.projectRootFor(event.document.uri);
    const retained = root === undefined ? undefined : projectDiagnostics.get(root)?.get(key);
    void connection.sendDiagnostics({uri: event.document.uri, diagnostics: retained ?? []});
  });

  connection.onDidChangeWatchedFiles(async (event) => {
    const affected = await service.filesChanged(event.changes.map((change) => ({uri: change.uri,
      created: change.type === FileChangeType.Created, deleted: change.type === FileChangeType.Deleted})));
    if (affected.size === 0) return;
    for (const document of documents.all()) {
      const root = service.projectRootFor(document.uri);
      if (root !== undefined && affected.has(root)) schedule(document);
    }
  });

  connection.onCompletion((params) => {
    const document = documents.get(params.textDocument.uri);
    return document === undefined ? null : service.completion(document, params.position);
  });
  connection.onCompletionResolve((item) => service.resolveCompletion(item));
  connection.onHover((params) => {
    const document = documents.get(params.textDocument.uri);
    return document === undefined ? null : service.hover(document, params.position);
  });
  connection.onDefinition((params) => {
    const document = documents.get(params.textDocument.uri);
    return document === undefined ? null : service.definition(document, params.position);
  });
  connection.onDocumentSymbol((params) => {
    const document = documents.get(params.textDocument.uri);
    return document === undefined ? [] : service.documentSymbols(document);
  });
  connection.onFoldingRanges((params) => {
    const document = documents.get(params.textDocument.uri);
    return document === undefined ? [] : service.foldingRanges(document);
  });
  connection.onRequest("prismatix/documentInfo", (params: {uri: string}) => service.documentInfo(params.uri));

  documents.listen(connection);
  connection.listen();
}

/** Entry point: the transport (--node-ipc from the VS Code client, or
 * --stdio for other editors) comes from the command line. */
export function main(): void {
  startServer(createConnection(ProposedFeatures.all));
}
