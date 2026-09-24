import type {ProjectAssetDescriptor} from "./resources.js";

export type DiagnosticSeverity = "error" | "warning" | "information";

export interface SourcePosition {
  readonly line: number;
  readonly column: number;
  readonly offset: number;
}

export interface SourceSpan {
  readonly path: string;
  readonly start: SourcePosition;
  readonly end: SourcePosition;
}

export interface AuthoringDiagnostic {
  readonly severity: DiagnosticSeverity;
  readonly code: string;
  readonly message: string;
  readonly path?: string;
  readonly span?: SourceSpan;
  readonly details?: string;
  readonly documentId?: string;
  readonly sourceId?: string;
  readonly hint?: string;
  readonly cause?: string;
}

export interface Result<T> {
  readonly value?: T;
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}

export type JsonPrimitive = null | boolean | number | string;
export type JsonValue = JsonPrimitive | JsonValue[] | {[key: string]: JsonValue};

export interface RuntimeIrOperation {
  readonly operationId: string;
  readonly sourceId: string;
  readonly sourceLine: number;
  readonly kind: string;
  readonly text: string;
  readonly arguments: Readonly<Record<string, string>>;
}

export interface RuntimeIrDocument {
  readonly format: "PrismatiXRuntimeIR";
  readonly schemaRevision: 2;
  readonly documentId: string;
  readonly committedRevision: number;
  readonly operations: readonly RuntimeIrOperation[];
}

export interface SourceMapEntry {
  readonly operationId: string;
  readonly sourceId: string;
  readonly sourceUri: string;
  readonly startLine: number;
  readonly startColumn: number;
  readonly endLine: number;
  readonly endColumn: number;
}

export interface RuntimeSourceMap {
  readonly format: "PrismatiXSourceMap";
  readonly schemaRevision: 2;
  readonly documentId: string;
  readonly mappings: readonly SourceMapEntry[];
}

export type StoryNodeKind = "label" | "speaker" | "dialogue" | "narration" | "command" | "comment";

export interface StoryArgument {
  readonly name?: string;
  readonly raw: string;
  readonly value: JsonValue;
  readonly span: SourceSpan;
}

export interface StoryNode {
  readonly id: string;
  /** Author-sidecar override for an existing fallback identity. */
  readonly externalSourceId?: string;
  readonly kind: StoryNodeKind;
  readonly span: SourceSpan;
  readonly raw: string;
  readonly name?: string;
  /** A quoted @"name" speaker is presentation text, not a character alias. */
  readonly literalSpeaker?: boolean;
  readonly text?: string;
  readonly arguments?: readonly StoryArgument[];
}

export interface StoryDocument {
  readonly path: string;
  readonly nodes: readonly StoryNode[];
  readonly diagnostics: readonly AuthoringDiagnostic[];
}

export interface ExtensionParameter {
  readonly name: string;
  readonly displayName?: string;
  readonly description?: string;
  readonly type: "null" | "boolean" | "integer" | "number" | "string" | "vec2" | "rect" | "color" | "uuid" | "resource" | "token" | "array" | "object";
  readonly required?: boolean;
  readonly default?: JsonValue;
  readonly enum?: readonly JsonValue[];
  readonly range?: {readonly minimum?: number; readonly maximum?: number; readonly step?: number};
  readonly resourceFilter?: string;
  readonly editorHint?: string;
}

export interface ExtensionSafety {
  readonly previewSafe: boolean;
  readonly deterministic: boolean;
  readonly seekSafe: boolean;
  readonly rollbackSafe: boolean;
}

export interface ExtensionCommand {
  readonly id: string;
  readonly displayName?: string;
  readonly description?: string;
  readonly category?: string;
  readonly await?: boolean;
  readonly rollback?: "reversible" | "boundary" | "transient";
  readonly capabilities?: readonly string[];
  readonly safety?: ExtensionSafety;
  readonly parameters: readonly ExtensionParameter[];
  readonly allowAdditionalParameters?: boolean;
}

export interface ExtensionAction {
  readonly id: string;
  readonly displayName?: string;
  readonly description?: string;
  readonly category?: string;
  readonly reentry?: "allow" | "ignoreWhileRunning" | "restart";
  readonly capabilities?: readonly string[];
  readonly safety?: ExtensionSafety;
  readonly parameters: readonly ExtensionParameter[];
}

export interface ExtensionManifest {
  readonly format: "PrismatiXExtension";
  readonly schemaRevision: 2;
  readonly language: "javascript";
  readonly id: string;
  readonly version: string;
  readonly requiredEngineVersion: string;
  readonly entry: string;
  /** ES modules that the entry is allowed to import, relative to this manifest. */
  readonly modules?: readonly string[];
  readonly capabilities: readonly ("runtime" | "animation" | "ui" | "audio" | "video" | "persistence" | "input" | "render")[];
  readonly safety?: ExtensionSafety;
  readonly commands: readonly ExtensionCommand[];
  readonly actions: readonly ExtensionAction[];
}

export interface CharacterExpression {
  readonly id: string;
  readonly name: string;
  readonly aliases: readonly string[];
  readonly assetId: string;
}

export interface CharacterDocument {
  readonly format: "PrismatiXCharacter";
  readonly schemaRevision: 2;
  readonly id: string;
  readonly displayName: string;
  /** Optional Story speaker names by locale; falls back to displayName. */
  readonly displayNames?: Readonly<Record<string, string>>;
  readonly aliases: readonly string[];
  readonly defaultExpressionId?: string | null;
  readonly expressions: readonly CharacterExpression[];
}

export interface GameVariable {
  readonly name: string;
  readonly type: "boolean" | "integer" | "number" | "string";
  readonly default: JsonPrimitive;
  readonly scope: "session" | "profile";
}

export interface GameDocument {
  readonly format: "PrismatiXGame";
  readonly schemaRevision: 2;
  readonly variables: readonly GameVariable[];
  readonly gallery?: readonly JsonValue[];
  readonly unlockables?: readonly JsonValue[];
  readonly inputBindings?: readonly JsonValue[];
  readonly inputActions?: readonly GameInputAction[];
}

/** Portable named input actions; hosts may bind additional behavior to an id. */
export interface GameInputAction {
  readonly id: string;
  readonly context: string;
  readonly keys?: readonly string[];
  /** Player route opened when this action is pressed in its context. */
  readonly route?: string;
}

export interface StoryCompileContext {
  readonly documentId: string;
  readonly committedRevision?: number;
  readonly locale?: string;
  readonly characters?: readonly CharacterDocument[];
  readonly game?: GameDocument;
  readonly extensions?: readonly ExtensionManifest[];
  /** Optional author aliases. Paths remain valid without appearing here. */
  readonly resources?: Readonly<Record<string, string>>;
  /** Stable-identity catalog used to normalize bare UUIDs and asset tokens. */
  readonly resourceAssets?: readonly ProjectAssetDescriptor[];
}

export interface StoryCompileResult {
  readonly runtimeIr?: RuntimeIrDocument;
  readonly sourceMap?: RuntimeSourceMap;
  /** Project-relative binary/text resources required by the emitted program. */
  readonly resourceDependencies?: readonly string[];
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}
