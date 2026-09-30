import {readFile} from "node:fs/promises";
import {fileURLToPath} from "node:url";

import type {AuthoringDiagnostic, ExtensionCommand, JsonValue, StoryCommandCompletion,
  StoryCompileResult} from "@prismatix/authoring-sdk";
import {
  CompletionItemKind, DiagnosticSeverity, FoldingRangeKind, MarkupKind, SymbolKind,
  type CompletionItem, type CompletionList, type Diagnostic, type DocumentSymbol,
  type FoldingRange, type Hover, type Location, type Position, type Range,
} from "vscode-languageserver";
import type {TextDocument} from "vscode-languageserver-textdocument";

import {completionContext, parseDocument, targetAt, type Columns, type CompletionContext,
  type LineTarget, type ParsedDocument} from "./analysis.js";
import {characterMarkdown, commandMarkdown, commandSignature, expressionMarkdown, fileUri,
  macroMarkdown, previewMarkdown} from "./markdown.js";
import {findProjectRoot, normalizeFsPath, ProjectModel, withoutRewriteEchoes, type CharacterInfo,
  type LabelInfo, type SceneSource} from "./project.js";
import {bundledSdk, type Sdk, type SdkMode} from "./sdk.js";
import {actorPresets, commandArgumentRole, inlineTags, isFlowCommand, positions,
  speakerOptionNames, speakerOptionRole, type ArgumentRole} from "./story-roles.js";

export interface ServiceOptions {
  readonly sdkMode: SdkMode;
}

/** Completion lists larger than this are filtered by the typed prefix and
 * marked incomplete, so a 30,000-file voice folder stays responsive. */
const maximumCompletionItems = 2000;

const builtinRoutes = ["title", "hud", "backlog", "save", "load", "gallery", "settings", "video"];

interface DocumentState {
  readonly version: number;
  readonly parsed: ParsedDocument;
}

interface DocumentScope {
  readonly fsPath?: string;
  readonly model?: ProjectModel;
  readonly virtualPath: string;
  readonly scene?: SceneSource;
  readonly sdk: Sdk;
}

export interface DocumentInfo {
  readonly projectRoot?: string;
  readonly sceneId?: string;
  readonly locale?: string;
  readonly path?: string;
  readonly sdk?: string;
}

function fsPathOf(uri: string): string | undefined {
  if (!uri.startsWith("file:")) return undefined;
  try { return normalizeFsPath(fileURLToPath(uri)); } catch { return undefined; }
}

function range(line: number, [start, end]: Columns): Range {
  return {start: {line, character: start}, end: {line, character: end}};
}

function lineRange(line: number): Range {
  return {start: {line, character: 0}, end: {line, character: 0}};
}

function valueText(value: JsonValue): string {
  const text = typeof value === "string" ? value : JSON.stringify(value);
  return /[\s\]"']/u.test(text) ? JSON.stringify(text) : text;
}

function severity(value: AuthoringDiagnostic["severity"]): DiagnosticSeverity {
  return value === "error" ? DiagnosticSeverity.Error
    : value === "warning" ? DiagnosticSeverity.Warning : DiagnosticSeverity.Information;
}

/** Converts an SDK diagnostic to LSP; the SDK's 1-based line and UTF-16
 * column map directly onto LSP positions. */
export function toLspDiagnostic(diagnostic: AuthoringDiagnostic, fallbackLine = 0): Diagnostic {
  const span = diagnostic.span;
  const detail = diagnostic.hint ?? diagnostic.details;
  return {
    range: span === undefined ? lineRange(fallbackLine) : {
      start: {line: span.start.line - 1, character: span.start.column - 1},
      end: {line: span.end.line - 1, character: span.end.column - 1},
    },
    severity: severity(diagnostic.severity),
    code: diagnostic.code,
    source: "prismatix",
    message: detail === undefined ? diagnostic.message : `${diagnostic.message}\n${detail}`,
  };
}

function identityMarkerLine(lines: readonly string[]): number {
  const index = lines.findIndex((line) => line.trim().startsWith(";") && line.includes("@prismatix-ids"));
  return Math.max(index, 0);
}

/**
 * The language features for `.pxstory`, independent of the LSP transport so
 * they can be tested directly. All Story parsing and compilation goes
 * through the Authoring SDK; this class only maps results to editor terms.
 */
export class PxstoryService {
  private readonly projects = new Map<string, Promise<ProjectModel>>();
  private readonly documents = new Map<string, DocumentState>();
  private readonly stubs = new Map<string, {readonly labels: readonly LabelInfo[]; readonly text: string}>();
  private readonly sidecars = new Map<string, string | null>();

  constructor(private options: ServiceOptions, private readonly log: (message: string) => void = () => {}) {}

  setOptions(options: ServiceOptions): void {
    const reload = options.sdkMode !== this.options.sdkMode;
    this.options = options;
    if (reload) this.projects.clear();
  }

  // ---- projects -----------------------------------------------------------

  projectRootFor(uri: string): string | undefined {
    const fsPath = fsPathOf(uri);
    return fsPath === undefined ? undefined : findProjectRoot(fsPath);
  }

  async project(root: string): Promise<ProjectModel> {
    const key = normalizeFsPath(root);
    let pending = this.projects.get(key);
    if (pending === undefined) {
      const started = performance.now();
      pending = ProjectModel.load(key, this.options.sdkMode).then((model) => {
        this.log(`Loaded ${key} in ${Math.round(performance.now() - started)} ms ` +
          `(${model.scenes.size} Story sources, ${model.characterInfos.length} characters, ` +
          `${model.knownFiles.length} packaged files, SDK: ${model.sdk.source}${
            model.sdk.version === undefined ? "" : ` ${model.sdk.version}`})`);
        for (const diagnostic of model.loadDiagnostics)
          this.log(`${diagnostic.code}: ${diagnostic.message}${diagnostic.details === undefined ? "" : ` (${diagnostic.details})`}`);
        return model;
      });
      this.projects.set(key, pending);
    }
    return pending;
  }

  loadedProjects(): Promise<ProjectModel[]> {
    return Promise.all(this.projects.values());
  }

  invalidateProject(root: string): void {
    this.projects.delete(normalizeFsPath(root));
    for (const key of this.stubs.keys()) if (key.startsWith(`${normalizeFsPath(root)}|`)) this.stubs.delete(key);
  }

  private async scope(document: TextDocument): Promise<DocumentScope> {
    const fsPath = fsPathOf(document.uri);
    const root = fsPath === undefined ? undefined : findProjectRoot(fsPath);
    if (fsPath === undefined || root === undefined)
      return {...(fsPath === undefined ? {} : {fsPath}), virtualPath: "Story/untitled.pxstory", sdk: bundledSdk};
    const model = await this.project(root);
    const virtualPath = model.toVirtual(fsPath) ?? "Story/untitled.pxstory";
    const scene = model.scenes.get(virtualPath);
    return {fsPath, model, virtualPath, ...(scene === undefined ? {} : {scene}), sdk: model.sdkApi};
  }

  private parsed(document: TextDocument, scope: DocumentScope): ParsedDocument {
    const cached = this.documents.get(document.uri);
    if (cached !== undefined && cached.version === document.version &&
        cached.parsed.path === scope.virtualPath) return cached.parsed;
    const parsed = parseDocument(scope.sdk, document.getText(), scope.virtualPath);
    this.documents.set(document.uri, {version: document.version, parsed});
    return parsed;
  }

  closeDocument(uri: string): void {
    this.documents.delete(uri);
  }

  async documentInfo(uri: string): Promise<DocumentInfo> {
    const fsPath = fsPathOf(uri);
    const root = fsPath === undefined ? undefined : findProjectRoot(fsPath);
    if (fsPath === undefined || root === undefined) return {};
    const model = await this.project(root);
    const path = model.toVirtual(fsPath);
    const scene = path === undefined ? undefined : model.scenes.get(path);
    return {projectRoot: model.root, ...(path === undefined ? {} : {path}),
      ...(scene === undefined ? {} : {sceneId: scene.sceneId, locale: scene.locale}),
      sdk: model.sdk.source};
  }

  /** Applies file-system changes. Returns the project roots whose documents
   * should be re-validated. */
  async filesChanged(changes: readonly {readonly uri: string; readonly deleted: boolean;
    readonly created: boolean}[]): Promise<Set<string>> {
    const affected = new Set<string>();
    const refreshFiles = new Set<ProjectModel>();
    for (const change of changes) {
      const fsPath = fsPathOf(change.uri);
      if (fsPath === undefined) continue;
      if (fsPath.endsWith(".pxids.json")) this.sidecars.delete(fsPath);
      for (const [root, pending] of this.projects) {
        const model = await pending;
        const path = model.toVirtual(fsPath);
        if (path === undefined) continue;
        if (/^(?:\.prismatix|\.git|node_modules|dist)\//u.test(path)) continue;
        const configuration = path === model.manifestVirtualPath || path === model.storyIndexPath ||
          path.endsWith(".pxcharacter") || path.endsWith(".pxgame") || path.endsWith(".pxextension") ||
          /^Content\/Localization\/[^/]+\.json$/u.test(path) ||
          path === model.project?.gameCatalog;
        if (configuration) {
          this.invalidateProject(root);
          affected.add(root);
          continue;
        }
        if (path.endsWith(".pxstory") && model.scenes.has(path)) {
          const open = [...this.documents.values()].some((state) => state.parsed.path === path);
          if (!open && await model.reloadLabels(path)) affected.add(root);
        }
        if (change.created || change.deleted) {
          model.invalidateFileCache();
          if (model.resourceIncludes.some((include) => path === include || path.startsWith(`${include}/`)))
            refreshFiles.add(model);
          affected.add(root);
        }
      }
    }
    for (const model of refreshFiles) model.refreshFiles();
    return affected;
  }

  // ---- diagnostics --------------------------------------------------------

  private async sidecar(model: ProjectModel, path: string): Promise<string | undefined> {
    const fsPath = normalizeFsPath(model.toFsPath(`${path}.pxids.json`));
    if (!this.sidecars.has(fsPath)) {
      try { this.sidecars.set(fsPath, await readFile(fsPath, "utf8")); }
      catch { this.sidecars.set(fsPath, null); }
    }
    return this.sidecars.get(fsPath) ?? undefined;
  }

  /** Other scenes are compiled from label-only stand-ins: cross-scene jumps
   * resolve exactly as in a full build while only this document compiles. */
  private stub(model: ProjectModel, path: string, labels: readonly LabelInfo[]): string {
    const key = `${model.root}|${path}`;
    const cached = this.stubs.get(key);
    if (cached !== undefined && cached.labels === labels) return cached.text;
    const text = labels.map((label) => `*${label.name}`).join("\n");
    this.stubs.set(key, {labels, text});
    return text;
  }

  private async compile(scope: DocumentScope, parsed: ParsedDocument): Promise<StoryCompileResult> {
    const {model, scene} = scope;
    if (model !== undefined && model.valid && scene !== undefined) {
      await model.ensureLabels(scene.locale);
      const sourceFiles: Record<string, string> = Object.create(null) as Record<string, string>;
      for (const other of model.sceneSources(scene.locale)) {
        if (other.path === scene.path) {
          sourceFiles[other.path] = parsed.text;
          const map = await this.sidecar(model, other.path);
          if (map !== undefined) sourceFiles[`${other.path}.pxids.json`] = map;
        } else {
          const labels = this.openLabels(other.path) ?? model.labelsOf(other.path);
          if (labels !== undefined) sourceFiles[other.path] = this.stub(model, other.path, labels);
        }
      }
      return model.sdkApi.compileStoryProject(model.compileContext(scene.locale, sourceFiles));
    }
    const locale = model?.defaultLocale;
    return scope.sdk.compileStory(parsed.document, {
      documentId: model?.projectId ?? "untitled",
      ...(locale === undefined ? {} : {locale}),
      ...(model !== undefined && model.valid ? {
        characters: model.characters, extensions: model.extensions,
        resources: model.resourcesFor(model.defaultLocale), resourceAssets: model.resourceAssets,
        ...(model.game === undefined ? {} : {game: model.game}),
        ...(model.storyAssets === undefined ? {} : {storyAssets: model.storyAssets}),
      } : {}),
    });
  }

  private openLabels(path: string): readonly LabelInfo[] | undefined {
    for (const state of this.documents.values())
      if (state.parsed.path === path) return state.parsed.labels;
    return undefined;
  }

  /** Diagnostics for one open document. `labelsChanged` tells the caller to
   * re-check other open scenes whose cross-scene targets may have changed. */
  async diagnostics(document: TextDocument): Promise<{diagnostics: Diagnostic[]; labelsChanged: boolean}> {
    const scope = await this.scope(document);
    const parsed = this.parsed(document, scope);
    const labelsChanged = scope.model !== undefined && scope.scene !== undefined &&
      scope.model.setLabels(scope.virtualPath, parsed.labels);
    const result = await this.compile(scope, parsed);
    const markerLine = identityMarkerLine(parsed.lines);
    const diagnostics: Diagnostic[] = [];
    for (const diagnostic of withoutRewriteEchoes(result.diagnostics)) {
      const path = diagnostic.span?.path ?? diagnostic.path;
      if (path === scope.virtualPath) diagnostics.push(toLspDiagnostic(diagnostic));
      else if (path === `${scope.virtualPath}.pxids.json`)
        diagnostics.push(toLspDiagnostic({...diagnostic, message: `${diagnostic.message} (${path})`}, markerLine));
      else if (path === undefined) diagnostics.push(toLspDiagnostic(diagnostic));
    }
    const model = scope.model;
    if (model !== undefined) {
      for (const location of result.resourceLocations ?? []) {
        if (location.span.path !== scope.virtualPath || model.fileExists(location.source)) continue;
        diagnostics.push(toLspDiagnostic({severity: "error", code: "PXBUILD1044",
          message: `Project dependency source is missing: ${location.source}`, span: location.span}));
      }
      for (const diagnostic of model.loadDiagnostics)
        if (diagnostic.code === "PXLSP0001") diagnostics.push(toLspDiagnostic(diagnostic));
      if (model.valid && scope.scene === undefined)
        diagnostics.push({range: lineRange(0), severity: DiagnosticSeverity.Information, source: "prismatix",
          code: "PXLSP0010", message: `This Story file is not listed in ${model.storyIndexPath}; ` +
            "it is checked on its own and cross-scene jumps are not resolved."});
    }
    return {diagnostics, labelsChanged};
  }

  // ---- completion ---------------------------------------------------------

  async completion(document: TextDocument, position: Position): Promise<CompletionList> {
    const scope = await this.scope(document);
    const parsed = this.parsed(document, scope);
    const lineText = parsed.lines[position.line] ?? "";
    const context = completionContext(scope.sdk, lineText, position.character);
    const items = this.completionItems(scope, parsed, context, position.line);
    if (items.length <= maximumCompletionItems) return {isIncomplete: false, items};
    const prefix = "prefix" in context ? context.prefix.toLowerCase() : "";
    const filtered = items.filter((item) =>
      (item.filterText ?? item.label).toLowerCase().startsWith(prefix));
    return {isIncomplete: true, items: filtered.slice(0, maximumCompletionItems)};
  }

  private extensionCommands(scope: DocumentScope): ExtensionCommand[] {
    return scope.model?.extensions.flatMap((extension) => extension.commands) ?? [];
  }

  private commandCompletions(scope: DocumentScope): readonly StoryCommandCompletion[] {
    return scope.sdk.storyCommandCompletions(this.extensionCommands(scope));
  }

  private completionItems(scope: DocumentScope, parsed: ParsedDocument, context: CompletionContext,
    line: number): CompletionItem[] {
    const item = (label: string, kind: CompletionItemKind, columns: Columns,
      extra: Partial<CompletionItem> & {insert?: string} = {}): CompletionItem => {
      const {insert, ...rest} = extra;
      return {label, kind, textEdit: {range: range(line, columns), newText: insert ?? label}, ...rest};
    };
    switch (context.kind) {
      case "none": return [];
      case "commandName": {
        const items: CompletionItem[] = [];
        const extensions = new Map(this.extensionCommands(scope).map((command) => [command.id, command]));
        for (const command of this.commandCompletions(scope)) {
          const extension = extensions.get(command.id);
          items.push(item(command.id, isFlowCommand(command.id) || ["if", "else", "endif", "return", "end",
            "choice.wait", "macro", "endmacro"].includes(command.id)
            ? CompletionItemKind.Keyword : CompletionItemKind.Function, context.columns, {
            detail: commandSignature(command),
            documentation: {kind: MarkupKind.Markdown,
              value: commandMarkdown(command, extension?.description ?? extension?.displayName)},
          }));
        }
        for (const macro of parsed.macros)
          items.push(item(macro.name, CompletionItemKind.Function, context.columns, {
            detail: `macro (${macro.parameters.map((parameter) => parameter.name).join(" ")})`,
            documentation: {kind: MarkupKind.Markdown, value: macroMarkdown(macro)},
          }));
        for (const tag of inlineTags)
          items.push(item(tag.label, CompletionItemKind.Property, context.columns,
            {insert: tag.insert, detail: tag.detail, sortText: `~${tag.label}`}));
        return items;
      }
      case "inlineTag":
        return inlineTags.map((tag) => item(tag.label, CompletionItemKind.Property, context.columns,
          {insert: tag.insert, detail: tag.detail}));
      case "inlineTagValue": {
        if (context.tag === "font" && scope.model !== undefined)
          return scope.model.filesFor("font").map((path) =>
            item(path, CompletionItemKind.File, context.columns, {insert: `${path}]`}));
        if (context.tag === "size")
          return ["75%", "150%", "200%", "40"].map((value) =>
            item(value, CompletionItemKind.Value, context.columns, {insert: `${value}]`}));
        return [];
      }
      case "speakerName":
        return this.characterItems(scope, context.columns, line, []);
      case "speakerOption": {
        const slot = context.name ?? `$${context.positionalIndex}`;
        const role = speakerOptionRole(slot, context.speaker);
        const items = this.roleItems(scope, parsed, role, context, line, "");
        if (context.name === undefined) {
          const used = new Set(context.args.map((argument) => argument.name).filter(Boolean));
          for (const name of speakerOptionNames)
            if (!used.has(name) && name !== "expression" && name !== "position")
              items.push(this.parameterItem(name, context.columns, line));
        }
        return items;
      }
      case "argument": {
        const extension = this.extensionCommands(scope).find((command) => command.id === context.command);
        const slot = context.name ?? `$${context.positionalIndex}`;
        const role = commandArgumentRole(context.command, slot, context.args, extension);
        const items = this.roleItems(scope, parsed, role, context, line, context.command);
        if (context.name === undefined) {
          const used = new Set(context.args.map((argument) => argument.name).filter(Boolean));
          const macro = parsed.macros.find((candidate) => candidate.name === context.command);
          const names = macro !== undefined ? macro.parameters.map((parameter) => parameter.name)
            : this.commandCompletions(scope).find((command) => command.id === context.command)
              ?.parameters.map((parameter) => parameter.name) ?? [];
          for (const name of names)
            if (!used.has(name)) items.push(this.parameterItem(name, context.columns, line));
          if (context.command === "set" && scope.model !== undefined)
            for (const variable of scope.model.variables)
              items.push(item(`${variable.name}=`, CompletionItemKind.Variable, context.columns, {
                detail: `${variable.type} (${variable.scope}), default ${JSON.stringify(variable.default)}`,
                filterText: variable.name,
              }));
        }
        return items;
      }
    }
  }

  private parameterItem(name: string, columns: Columns, line: number): CompletionItem {
    return {label: `${name}=`, kind: CompletionItemKind.Property, sortText: `~${name}`,
      filterText: name, textEdit: {range: range(line, columns), newText: `${name}=`},
      command: {title: "Suggest", command: "editor.action.triggerSuggest"}};
  }

  private characterItems(scope: DocumentScope, columns: Columns, line: number,
    extra: readonly string[]): CompletionItem[] {
    const model = scope.model;
    const items: CompletionItem[] = extra.map((value) => ({label: value, kind: CompletionItemKind.Keyword,
      textEdit: {range: range(line, columns), newText: value}}));
    if (model === undefined) return items;
    for (const character of model.characterInfos)
      for (const alias of character.aliases)
        items.push({label: alias, kind: CompletionItemKind.Class, detail: character.displayName,
          documentation: {kind: MarkupKind.Markdown, value: characterMarkdown(model, character)},
          textEdit: {range: range(line, columns), newText: alias}});
    for (const [name, character] of model.uniqueDisplayNames())
      items.push({label: name, kind: CompletionItemKind.Class, detail: character.aliases[0] ?? character.id,
        documentation: {kind: MarkupKind.Markdown, value: characterMarkdown(model, character)},
        textEdit: {range: range(line, columns), newText: name}});
    return items;
  }

  private expressionItems(model: ProjectModel, character: CharacterInfo, columns: Columns,
    line: number): CompletionItem[] {
    const items: CompletionItem[] = [];
    for (const expression of character.expressions)
      for (const alias of expression.aliases)
        items.push({label: alias, kind: CompletionItemKind.EnumMember, detail: expression.name,
          textEdit: {range: range(line, columns), newText: alias},
          data: {kind: "expression", root: model.root, character: character.id, expression: expression.id}});
    return items;
  }

  private roleItems(scope: DocumentScope, parsed: ParsedDocument, role: ArgumentRole,
    context: Extract<CompletionContext, {kind: "argument" | "speakerOption"}>, line: number,
    command: string): CompletionItem[] {
    const model = scope.model;
    const columns = context.columns;
    const plain = (values: readonly string[], kind: CompletionItemKind): CompletionItem[] =>
      values.map((value) => ({label: value, kind, textEdit: {range: range(line, columns), newText: value}}));
    switch (role.kind) {
      case "none": return [];
      case "values": return plain(role.values.map(valueText), CompletionItemKind.Value);
      case "position": return plain([...positions], CompletionItemKind.EnumMember);
      case "preset": return plain([...actorPresets], CompletionItemKind.EnumMember);
      case "character": return this.characterItems(scope, columns, line, role.extra ?? []);
      case "imageLayer": return plain([...role.extra ?? [], ...parsed.imageLayers], CompletionItemKind.Variable);
      case "route": return plain([...new Set([...builtinRoutes,
        ...Object.keys(model?.project?.uiEntryPoints ?? {})])], CompletionItemKind.EnumMember);
      case "variable": return model === undefined ? [] : model.variables.map((variable) => ({
        label: variable.name, kind: CompletionItemKind.Variable,
        detail: `${variable.type} (${variable.scope})`,
        textEdit: {range: range(line, columns), newText: variable.name}}));
      case "expression":
      case "expressionOrPosition": {
        const items = role.kind === "expressionOrPosition" ? plain([...positions], CompletionItemKind.EnumMember)
          : plain([...role.extra ?? []], CompletionItemKind.Keyword);
        const character = role.character === undefined ? undefined : model?.findCharacter(role.character);
        if (model !== undefined && character !== undefined)
          items.push(...this.expressionItems(model, character, columns, line));
        return items;
      }
      case "asset": {
        if (model === undefined) return [];
        if (role.category !== undefined && model.storyAssetRoots?.[role.category] !== undefined)
          return model.assetNames(role.category).map((asset) => ({label: asset.name,
            kind: CompletionItemKind.File, detail: asset.path,
            textEdit: {range: range(line, columns), newText: asset.name},
            data: {kind: "asset", root: model.root, path: asset.path}}));
        const files = role.extensions === undefined ? model.filesFor(role.category ?? "any")
          : model.knownFiles.filter((path) => role.extensions!.test(path));
        return files.map((path) => ({label: path, kind: CompletionItemKind.File,
          textEdit: {range: range(line, columns), newText: path},
          data: {kind: "asset", root: model.root, path}}));
      }
      case "scene": {
        if (model === undefined || scope.scene === undefined) return [];
        return model.sceneSources(scope.scene.locale).map((scene) => ({label: scene.sceneId,
          kind: CompletionItemKind.Module, detail: scene.title ?? scene.path,
          textEdit: {range: range(line, columns), newText: valueText(scene.sceneId)}}));
      }
      case "label": {
        const items: CompletionItem[] = [];
        const targetScene = role.scene === undefined ? scope.scene
          : model?.sceneById(role.scene, scope.scene?.locale ?? model.defaultLocale);
        const local = role.scene === undefined || targetScene?.path === scope.virtualPath;
        const labels = local ? parsed.labels : targetScene === undefined ? []
          : model?.labelsOf(targetScene.path) ?? [];
        for (const label of labels)
          items.push({label: label.name, kind: CompletionItemKind.Reference,
            detail: `${targetScene?.sceneId ?? "this scene"}, line ${label.line + 1}`,
            textEdit: {range: range(line, columns), newText: valueText(label.name)}});
        // Without scene=, offer other scenes as `scene:label`: the edit writes
        // both arguments, the way the compiler reads a cross-scene target.
        if (role.scene === undefined && model !== undefined && scope.scene !== undefined &&
            context.kind === "argument" && isFlowCommand(command) && context.quote === undefined) {
          const slotName = context.name ?? (command === "choice" ? "goto" : "target");
          const start = context.name === undefined ? columns[0] : columns[0] - context.name.length - 1;
          const typed = context.name === undefined ? "" : `${context.name}=`;
          for (const scene of model.sceneSources(scope.scene.locale)) {
            if (scene.path === scope.virtualPath) continue;
            items.push({label: `${scene.sceneId}:`, kind: CompletionItemKind.Module,
              detail: `start of scene ${scene.sceneId}`, filterText: `${typed}${scene.sceneId}:`,
              sortText: `~1${scene.sceneId}`,
              textEdit: {range: range(line, [start, columns[1]]), newText: `scene=${valueText(scene.sceneId)}`}});
            for (const label of model.labelsOf(scene.path) ?? [])
              items.push({label: `${scene.sceneId}:${label.name}`, kind: CompletionItemKind.Reference,
                detail: `line ${label.line + 1} of ${scene.path}`,
                filterText: `${typed}${scene.sceneId}:${label.name}`, sortText: `~2${scene.sceneId}:${label.name}`,
                textEdit: {range: range(line, [start, columns[1]]),
                  newText: `scene=${valueText(scene.sceneId)} ${slotName}=${valueText(label.name)}`}});
          }
        }
        return items;
      }
    }
  }

  /** Adds image previews to asset and expression completions lazily. */
  async resolveCompletion(item: CompletionItem): Promise<CompletionItem> {
    const data = item.data as {kind?: string; root?: string; path?: string; character?: string;
      expression?: string} | undefined;
    if (data?.root === undefined) return item;
    const model = await this.project(data.root);
    if (data.kind === "asset" && data.path !== undefined) {
      const preview = previewMarkdown(model, data.path);
      if (preview !== undefined) item.documentation = {kind: MarkupKind.Markdown, value: `${data.path}\n\n${preview}`};
    } else if (data.kind === "expression") {
      const character = model.characterInfos.find((candidate) => candidate.id === data.character);
      const expression = character?.expressions.find((candidate) => candidate.id === data.expression);
      if (character !== undefined && expression !== undefined)
        item.documentation = {kind: MarkupKind.Markdown, value: expressionMarkdown(model, character, expression)};
    }
    return item;
  }

  // ---- hover and definition -----------------------------------------------

  private async target(document: TextDocument, position: Position):
    Promise<{scope: DocumentScope; parsed: ParsedDocument; target: LineTarget} | undefined> {
    const scope = await this.scope(document);
    const parsed = this.parsed(document, scope);
    const lineText = parsed.lines[position.line] ?? "";
    const target = targetAt(scope.sdk, lineText, position.character);
    return target === undefined ? undefined : {scope, parsed, target};
  }

  private roleOf(scope: DocumentScope, target: LineTarget): ArgumentRole {
    if (target.kind === "argument") {
      const extension = this.extensionCommands(scope).find((command) => command.id === target.command);
      return commandArgumentRole(target.command, target.slot, target.args, extension);
    }
    if (target.kind === "speakerOption") return speakerOptionRole(target.slot, target.speaker);
    if (target.kind === "speakerName") return {kind: "character"};
    return {kind: "none"};
  }

  private labelLocation(scope: DocumentScope, parsed: ParsedDocument, name: string,
    sceneId: string | undefined): {location: Location; scene: string} | undefined {
    const model = scope.model;
    const scene = sceneId === undefined ? scope.scene
      : model?.sceneById(sceneId, scope.scene?.locale ?? model.defaultLocale);
    if ((sceneId === undefined || scene?.path === scope.virtualPath) && scope.fsPath !== undefined) {
      const label = parsed.labels.find((candidate) => candidate.name === name);
      return label === undefined ? undefined : {scene: scene?.sceneId ?? "this scene",
        location: {uri: fileUri(scope.fsPath), range: lineRange(label.line)}};
    }
    if (model === undefined || scene === undefined) return undefined;
    const label = (this.openLabels(scene.path) ?? model.labelsOf(scene.path))?.find((candidate) => candidate.name === name);
    return label === undefined ? undefined : {scene: scene.sceneId,
      location: {uri: fileUri(model.toFsPath(scene.path)), range: lineRange(label.line)}};
  }

  private characterLocation(model: ProjectModel, character: CharacterInfo, needle: string,
    afterKey?: string): Location {
    const text = model.characterSources.get(character.source) ?? "";
    const from = afterKey === undefined ? 0 : Math.max(text.indexOf(`"${afterKey}"`), 0);
    const quoted = JSON.stringify(needle);
    let offset = text.indexOf(quoted, from);
    if (offset < 0) offset = text.indexOf(quoted);
    const before = offset < 0 ? "" : text.slice(0, offset);
    const lineNumber = offset < 0 ? 0 : before.split("\n").length - 1;
    const character0 = offset < 0 ? 0 : offset - (before.lastIndexOf("\n") + 1);
    return {uri: fileUri(model.toFsPath(character.source)),
      range: {start: {line: lineNumber, character: character0},
        end: {line: lineNumber, character: character0 + (offset < 0 ? 0 : quoted.length)}}};
  }

  async definition(document: TextDocument, position: Position): Promise<Location[] | null> {
    const found = await this.target(document, position);
    if (found === undefined) return null;
    const {scope, parsed, target} = found;
    const model = scope.model;
    if (target.kind === "commandName") {
      const macro = parsed.macros.find((candidate) => candidate.name === target.command);
      return macro === undefined || scope.fsPath === undefined ? null
        : [{uri: fileUri(scope.fsPath), range: lineRange(macro.line)}];
    }
    if (target.kind === "label") return null;
    const role = this.roleOf(scope, target);
    const value = target.kind === "speakerName" ? target.name
      : typeof target.argument.value === "string" ? target.argument.value : String(target.argument.value);
    switch (role.kind) {
      case "label": {
        const location = this.labelLocation(scope, parsed, value, role.scene);
        return location === undefined ? null : [location.location];
      }
      case "scene": {
        const scene = model?.sceneById(value, scope.scene?.locale ?? model.defaultLocale);
        return model === undefined || scene === undefined ? null
          : [{uri: fileUri(model.toFsPath(scene.path)), range: lineRange(0)}];
      }
      case "character": {
        const character = model?.findCharacter(value);
        if (model === undefined || character === undefined) return null;
        return [this.characterLocation(model, character, value,
          character.aliases.includes(value) ? "aliases" : undefined)];
      }
      case "expression": case "expressionOrPosition": {
        const character = role.character === undefined ? undefined : model?.findCharacter(role.character);
        const expression = character?.expressions.find((candidate) =>
          candidate.aliases.includes(value) || candidate.id === value);
        if (model === undefined || character === undefined || expression === undefined) return null;
        return [this.characterLocation(model, character, value, "expressions")];
      }
      case "asset": {
        const resolved = model?.resolveAsset(role.category, value, scope.scene?.locale);
        if (model === undefined || resolved?.kind !== "resolved" || !model.fileExists(resolved.path)) return null;
        return [{uri: fileUri(model.toFsPath(resolved.path)), range: lineRange(0)}];
      }
      default: return null;
    }
  }

  async hover(document: TextDocument, position: Position): Promise<Hover | null> {
    const found = await this.target(document, position);
    if (found === undefined) return null;
    const {scope, parsed, target} = found;
    const model = scope.model;
    const markdown = (value: string): Hover => ({contents: {kind: MarkupKind.Markdown, value},
      range: range(position.line, target.columns)});
    if (target.kind === "commandName") {
      const macro = parsed.macros.find((candidate) => candidate.name === target.command);
      if (macro !== undefined) return markdown(macroMarkdown(macro));
      const command = this.commandCompletions(scope).find((candidate) => candidate.id === target.command);
      const extension = this.extensionCommands(scope).find((candidate) => candidate.id === target.command);
      return command === undefined ? null
        : markdown(commandMarkdown(command, extension?.description ?? extension?.displayName));
    }
    if (target.kind === "label") return null;
    const role = this.roleOf(scope, target);
    const value = target.kind === "speakerName" ? target.name
      : typeof target.argument.value === "string" ? target.argument.value : String(target.argument.value);
    switch (role.kind) {
      case "character": {
        if (target.kind === "speakerName" && target.literal) return markdown(`Speaker name \`${value}\` (not a character)`);
        const character = model?.findCharacter(value);
        if (model === undefined || character === undefined)
          return role.extra?.includes(value) ? null : markdown(`\`${value}\` is not a character of this project`);
        return markdown(characterMarkdown(model, character));
      }
      case "expression": case "expressionOrPosition": {
        if (positions.includes(value as typeof positions[number])) return markdown(`Stage position \`${value}\``);
        const character = role.character === undefined ? undefined : model?.findCharacter(role.character);
        const expression = character?.expressions.find((candidate) =>
          candidate.aliases.includes(value) || candidate.id === value);
        if (model === undefined || character === undefined || expression === undefined) return null;
        return markdown(expressionMarkdown(model, character, expression));
      }
      case "asset": {
        if (model === undefined) return null;
        const resolved = model.resolveAsset(role.category, value, scope.scene?.locale);
        const kind = role.category ?? "resource";
        if (resolved === undefined) return null;
        if (resolved.kind === "ambiguous")
          return markdown(`**${kind}** \`${value}\` matches several files:\n\n${resolved.candidates
            .map((candidate) => `- \`${candidate}\``).join("\n")}`);
        if (resolved.kind === "missing")
          return markdown(`No ${kind} asset named \`${value}\` under ${resolved.roots.map((root) => `\`${root}\``).join(", ")}`);
        const preview = previewMarkdown(model, resolved.path);
        const exists = model.fileExists(resolved.path);
        return markdown(`**${kind}** \`${resolved.path}\`${exists ? "" : " (missing)"}${
          preview === undefined ? "" : `\n\n${preview}`}`);
      }
      case "label": {
        const location = this.labelLocation(scope, parsed, value, role.scene);
        return markdown(location === undefined ? `Label \`${value}\` is not defined${
          role.scene === undefined ? " in this scene" : ` in scene \`${role.scene}\``}`
          : `Label \`${value}\` in scene \`${location.scene}\`, line ${location.location.range.start.line + 1}`);
      }
      case "scene": {
        const scene = model?.sceneById(value, scope.scene?.locale ?? model.defaultLocale);
        if (model === undefined || scene === undefined) return markdown(`Unknown Story scene \`${value}\``);
        const labels = model.labelsOf(scene.path) ?? [];
        return markdown(`**Scene** \`${scene.sceneId}\`${scene.title === undefined ? "" : ` — ${scene.title}`}\n\n` +
          `\`${scene.path}\`, ${labels.length} label${labels.length === 1 ? "" : "s"}`);
      }
      case "variable": {
        const variable = model?.variables.find((candidate) => candidate.name ===
          (target.kind === "argument" && target.argument.name !== undefined && target.command === "set"
            ? target.argument.name : value));
        return variable === undefined ? null : markdown(`**${variable.name}**: ${variable.type} ` +
          `(${variable.scope}), default \`${JSON.stringify(variable.default)}\``);
      }
      default: return null;
    }
  }

  // ---- outline and folding ------------------------------------------------

  async documentSymbols(document: TextDocument): Promise<DocumentSymbol[]> {
    const scope = await this.scope(document);
    const parsed = this.parsed(document, scope);
    const lastLine = Math.max(parsed.lines.length - 1, 0);
    const lineEnd = (line: number) => (parsed.lines[line] ?? "").length;
    const symbols: DocumentSymbol[] = [];
    for (const [index, label] of parsed.labels.entries()) {
      const next = parsed.labels[index + 1]?.line ?? lastLine + 1;
      const end = Math.max(label.line, next - 1);
      const labelLine = parsed.lines[label.line] ?? "";
      const start = Math.max(labelLine.indexOf(label.name), 0);
      symbols.push({name: label.name, kind: SymbolKind.Namespace, detail: "label",
        range: {start: {line: label.line, character: 0}, end: {line: end, character: lineEnd(end)}},
        selectionRange: range(label.line, [start, start + label.name.length])});
    }
    for (const macro of parsed.macros) {
      const header = parsed.lines[macro.line] ?? "";
      const start = Math.max(header.indexOf(macro.name), 0);
      symbols.push({name: macro.name, kind: SymbolKind.Function,
        detail: `macro ${macro.parameters.map((parameter) => parameter.name).join(" ")}`.trim(),
        range: {start: {line: macro.line, character: 0}, end: {line: macro.endLine, character: lineEnd(macro.endLine)}},
        selectionRange: range(macro.line, [start, start + macro.name.length])});
    }
    return symbols.sort((left, right) => left.range.start.line - right.range.start.line);
  }

  async foldingRanges(document: TextDocument): Promise<FoldingRange[]> {
    const scope = await this.scope(document);
    const parsed = this.parsed(document, scope);
    const ranges: FoldingRange[] = [];
    const lastContent = (from: number, to: number): number => {
      let line = to;
      while (line > from && (parsed.lines[line] ?? "").trim().length === 0) line -= 1;
      return line;
    };
    for (const [index, label] of parsed.labels.entries()) {
      const next = parsed.labels[index + 1]?.line ?? parsed.lines.length;
      const end = lastContent(label.line, next - 1);
      if (end > label.line) ranges.push({startLine: label.line, endLine: end, kind: FoldingRangeKind.Region});
    }
    for (const macro of parsed.macros)
      if (macro.endLine > macro.line) ranges.push({startLine: macro.line, endLine: macro.endLine});
    const conditions: number[] = [];
    for (const node of parsed.document.nodes) {
      if (node.kind !== "command") continue;
      const line = node.span.start.line - 1;
      if (node.name === "if") conditions.push(line);
      else if (node.name === "endif") {
        const start = conditions.pop();
        if (start !== undefined && line > start) ranges.push({startLine: start, endLine: line});
      }
    }
    let commentStart: number | undefined;
    for (let line = 0; line <= parsed.lines.length; line += 1) {
      const comment = (parsed.lines[line] ?? "").trim().startsWith(";");
      if (comment && commentStart === undefined) commentStart = line;
      if (!comment && commentStart !== undefined) {
        if (line - 1 > commentStart) ranges.push({startLine: commentStart, endLine: line - 1,
          kind: FoldingRangeKind.Comment});
        commentStart = undefined;
      }
    }
    return ranges;
  }
}
