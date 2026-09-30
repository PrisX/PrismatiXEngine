import {spawn, type ChildProcess} from "node:child_process";
import {existsSync} from "node:fs";
import {join} from "node:path";

import * as vscode from "vscode";
import {LanguageClient, TransportKind, type LanguageClientOptions,
  type ServerOptions} from "vscode-languageclient/node";

import {CliNotFoundError, describePreviewFailure, formatDiagnostic, parseCliLine, previewArguments,
  resolveCli, shellArguments, validateArguments, type CliDiagnostic, type CliInvocation} from "./cli-support.js";

interface DocumentInfo {
  readonly projectRoot?: string;
  readonly sceneId?: string;
  readonly locale?: string;
  readonly path?: string;
}

let client: LanguageClient | undefined;
let preview: {process: ChildProcess; stopping: boolean; root: string} | undefined;
let previewStatus: vscode.StatusBarItem | undefined;

function settings(): vscode.WorkspaceConfiguration {
  return vscode.workspace.getConfiguration("prismatix");
}

function plainSettings(): unknown {
  return JSON.parse(JSON.stringify(settings())) as unknown;
}

function cliFor(root: string): CliInvocation {
  return resolveCli(root, settings().get<string>("cli.nodePath") || "node",
    settings().get<string>("cli.path") || undefined);
}

function spawnCli(invocation: CliInvocation, args: readonly string[], root: string, ipc: boolean): ChildProcess {
  const all = [...invocation.args, ...args];
  return spawn(invocation.command, invocation.shell ? shellArguments(all) : all, {
    cwd: root, shell: invocation.shell, windowsHide: true,
    stdio: ipc ? ["ignore", "pipe", "pipe", "ipc"] : ["ignore", "pipe", "pipe"],
  });
}

/** Splits a stream into lines and feeds each to `onLine`. */
function lines(stream: NodeJS.ReadableStream | null, onLine: (line: string) => void): void {
  let buffered = "";
  stream?.setEncoding("utf8");
  stream?.on("data", (chunk: string) => {
    buffered += chunk;
    let newline: number;
    while ((newline = buffered.indexOf("\n")) >= 0) {
      onLine(buffered.slice(0, newline).replace(/\r$/u, ""));
      buffered = buffered.slice(newline + 1);
    }
  });
  stream?.on("end", () => { if (buffered.length > 0) onLine(buffered); });
}

async function documentInfo(document: vscode.TextDocument): Promise<DocumentInfo> {
  if (client === undefined) return {};
  return client.sendRequest<DocumentInfo>("prismatix/documentInfo", {uri: document.uri.toString()});
}

/** The project of the active .pxstory editor, else a workspace folder with a manifest. */
async function chooseProject(): Promise<string | undefined> {
  const editor = vscode.window.activeTextEditor;
  if (editor?.document.languageId === "pxstory") {
    const info = await documentInfo(editor.document);
    if (info.projectRoot !== undefined) return info.projectRoot;
  }
  const roots = (vscode.workspace.workspaceFolders ?? [])
    .map((folder) => folder.uri.fsPath)
    .filter((root) => existsSync(join(root, "prismatix.json")) || existsSync(join(root, "project.pxproject")));
  if (roots.length <= 1) return roots[0];
  return vscode.window.showQuickPick(roots, {placeHolder: "PrismatiX project to validate"});
}

function diagnosticUri(root: string, diagnostic: CliDiagnostic): vscode.Uri {
  const path = diagnostic.span?.path ?? diagnostic.path;
  if (path === undefined || path === "project.pxproject") {
    const manifest = existsSync(join(root, "prismatix.json")) ? "prismatix.json" : "project.pxproject";
    return vscode.Uri.file(join(root, manifest));
  }
  return /^[A-Za-z]:[\\/]|^\//u.test(path) ? vscode.Uri.file(path) : vscode.Uri.file(join(root, ...path.split("/")));
}

function toVscodeDiagnostic(diagnostic: CliDiagnostic): vscode.Diagnostic {
  const span = diagnostic.span;
  const range = span === undefined ? new vscode.Range(0, 0, 0, 0)
    : new vscode.Range(span.start.line - 1, span.start.column - 1, span.end.line - 1, span.end.column - 1);
  const detail = diagnostic.hint ?? diagnostic.details;
  const result = new vscode.Diagnostic(range, detail === undefined ? diagnostic.message : `${diagnostic.message}\n${detail}`,
    diagnostic.severity === "error" ? vscode.DiagnosticSeverity.Error : vscode.DiagnosticSeverity.Warning);
  result.code = diagnostic.code;
  result.source = "prismatix validate";
  return result;
}

async function validateProject(output: vscode.OutputChannel, collection: vscode.DiagnosticCollection): Promise<void> {
  const root = await chooseProject();
  if (root === undefined) {
    void vscode.window.showWarningMessage("PrismatiX: open a .pxstory file or a folder with prismatix.json first.");
    return;
  }
  let invocation: CliInvocation;
  try { invocation = cliFor(root); }
  catch (error) {
    if (error instanceof CliNotFoundError) { void vscode.window.showErrorMessage(`PrismatiX: ${error.message}`); return; }
    throw error;
  }
  await vscode.workspace.saveAll(false);
  const args = validateArguments(root, settings().get<string[]>("cli.validateArgs") ?? []);
  output.appendLine(`$ ${invocation.command} ${[...invocation.args, ...args].join(" ")}  (${invocation.description})`);
  const diagnostics: CliDiagnostic[] = [];
  let summary: Record<string, unknown> | undefined;
  const exitCode = await vscode.window.withProgress({location: vscode.ProgressLocation.Notification,
    title: "PrismatiX: validating project", cancellable: true}, (progress, token) =>
    new Promise<number | null>((resolveExit) => {
      const child = spawnCli(invocation, args, root, false);
      token.onCancellationRequested(() => child.kill());
      lines(child.stdout, (line) => {
        const parsed = parseCliLine(line);
        if (parsed.kind === "event" && parsed.event.valid === true) summary = parsed.event;
        output.appendLine(line);
      });
      lines(child.stderr, (line) => {
        const parsed = parseCliLine(line);
        if (parsed.kind === "diagnostic") {
          diagnostics.push(parsed.diagnostic);
          output.appendLine(formatDiagnostic(parsed.diagnostic));
        } else {
          if (parsed.kind === "event") progress.report({message: parsed.text});
          output.appendLine(parsed.text);
        }
      });
      child.once("error", (error) => { output.appendLine(String(error)); resolveExit(null); });
      child.once("close", (code) => resolveExit(code));
    }));
  collection.clear();
  const byFile = new Map<string, {uri: vscode.Uri; items: vscode.Diagnostic[]}>();
  for (const diagnostic of diagnostics) {
    const uri = diagnosticUri(root, diagnostic);
    const entry = byFile.get(uri.toString()) ?? {uri, items: []};
    entry.items.push(toVscodeDiagnostic(diagnostic));
    byFile.set(uri.toString(), entry);
  }
  for (const {uri, items} of byFile.values()) collection.set(uri, items);
  const errors = diagnostics.filter((diagnostic) => diagnostic.severity === "error").length;
  if (exitCode === 0) {
    void vscode.window.showInformationMessage(`PrismatiX: project is valid${summary?.resourceFiles === undefined
      ? "" : ` (${String(summary.resourceFiles)} resource files)`}${diagnostics.length > 0
      ? `, ${diagnostics.length} warning${diagnostics.length === 1 ? "" : "s"}` : ""}.`);
  } else {
    const choice = await vscode.window.showErrorMessage(errors > 0
      ? `PrismatiX: validation found ${errors} error${errors === 1 ? "" : "s"}.`
      : `PrismatiX: validate exited with code ${exitCode ?? "unknown"}.`, "Show Problems", "Show Output");
    if (choice === "Show Problems") void vscode.commands.executeCommand("workbench.actions.view.problems");
    if (choice === "Show Output") output.show(true);
  }
}

async function stopPreview(): Promise<void> {
  const running = preview;
  if (running === undefined) return;
  running.stopping = true;
  const exited = new Promise<void>((resolveExit) => {
    if (running.process.exitCode !== null) resolveExit();
    else running.process.once("exit", () => resolveExit());
  });
  // The CLI closes its PreviewHost cleanly on this message; kill() on Windows
  // would skip its shutdown path, so it is only the last resort.
  if (running.process.connected) running.process.send({type: "prismatix-launcher-shutdown"}, () => {});
  else running.process.kill();
  const escalation = setTimeout(() => running.process.kill(), 35_000);
  await exited;
  clearTimeout(escalation);
}

async function previewFromHere(output: vscode.OutputChannel): Promise<void> {
  const editor = vscode.window.activeTextEditor;
  if (editor === undefined || editor.document.languageId !== "pxstory") {
    void vscode.window.showWarningMessage("PrismatiX: Preview From Here needs an open .pxstory scene.");
    return;
  }
  const info = await documentInfo(editor.document);
  if (info.projectRoot === undefined || info.sceneId === undefined) {
    void vscode.window.showErrorMessage("PrismatiX: this file is not a scene in the project's Story index, " +
      "so the preview cannot start at it.");
    return;
  }
  const root = info.projectRoot;
  let invocation: CliInvocation;
  try { invocation = cliFor(root); }
  catch (error) {
    if (error instanceof CliNotFoundError) { void vscode.window.showErrorMessage(`PrismatiX: ${error.message}`); return; }
    throw error;
  }
  if (editor.document.isDirty) await editor.document.save();
  await stopPreview();
  const line = editor.selection.active.line + 1;
  const args = previewArguments(root, info.sceneId, line, settings().get<string[]>("cli.devArgs") ?? []);
  output.show(true);
  output.appendLine(`$ ${invocation.command} ${[...invocation.args, ...args].join(" ")}  (${invocation.description})`);
  const child = spawnCli(invocation, args, root, true);
  const running = {process: child, stopping: false, root};
  preview = running;
  const diagnostics: CliDiagnostic[] = [];
  const onLine = (line: string): void => {
    const parsed = parseCliLine(line);
    if (parsed.kind === "diagnostic") {
      diagnostics.push(parsed.diagnostic);
      output.appendLine(formatDiagnostic(parsed.diagnostic));
    } else output.appendLine(parsed.text);
  };
  lines(child.stdout, onLine);
  lines(child.stderr, onLine);
  previewStatus?.show();
  child.once("error", (error) => {
    output.appendLine(String(error));
    void vscode.window.showErrorMessage(`PrismatiX: could not start the CLI: ${error.message}`);
  });
  child.once("close", (code) => {
    if (preview === running) { preview = undefined; previewStatus?.hide(); }
    output.appendLine(`[preview exited with code ${code ?? "unknown"}]`);
    if (!running.stopping && code !== 0 && code !== null) {
      void vscode.window.showErrorMessage(`PrismatiX: ${describePreviewFailure(code, diagnostics)}`, "Show Output")
        .then((choice) => { if (choice === "Show Output") output.show(true); });
    }
  });
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  const output = vscode.window.createOutputChannel("PrismatiX");
  const serverOutput = vscode.window.createOutputChannel("PrismatiX Language Server", {log: true});
  const cliDiagnostics = vscode.languages.createDiagnosticCollection("prismatix-validate");
  previewStatus = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 50);
  previewStatus.text = "$(debug-stop) PrismatiX Preview";
  previewStatus.tooltip = "Stop the PrismatiX preview started by Preview From Here";
  previewStatus.command = "prismatix.stopPreview";
  context.subscriptions.push(output, serverOutput, cliDiagnostics, previewStatus);

  const module = context.asAbsolutePath(join("dist", "server.js"));
  const serverOptions: ServerOptions = {
    run: {module, transport: TransportKind.ipc},
    debug: {module, transport: TransportKind.ipc, options: {execArgv: ["--nolazy", "--inspect=6009"]}},
  };
  const watchers = [
    "**/*.{pxstory,pxcharacter,pxindex,pxgame,pxextension}",
    "**/{prismatix.json,project.pxproject}",
    "**/*.pxids.json",
    "**/Content/Localization/*.json",
  ].map((pattern) => vscode.workspace.createFileSystemWatcher(pattern));
  // Media only matters when it appears or disappears (short-name completion).
  watchers.push(vscode.workspace.createFileSystemWatcher("**/*", false, true, false));
  context.subscriptions.push(...watchers);
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{scheme: "file", language: "pxstory"}, {scheme: "untitled", language: "pxstory"}],
    synchronize: {configurationSection: "prismatix", fileEvents: watchers},
    initializationOptions: {settings: plainSettings()},
    outputChannel: serverOutput,
    markdown: {isTrusted: false, supportHtml: false},
  };
  client = new LanguageClient("pxstory", "PrismatiX Story", serverOptions, clientOptions);

  context.subscriptions.push(
    vscode.commands.registerCommand("prismatix.validateProject", () => validateProject(output, cliDiagnostics)),
    vscode.commands.registerCommand("prismatix.previewFromHere", () => previewFromHere(output)),
    vscode.commands.registerCommand("prismatix.stopPreview", () => stopPreview()),
  );
  await client.start();
}

export async function deactivate(): Promise<void> {
  await stopPreview();
  await client?.stop();
  client = undefined;
}
