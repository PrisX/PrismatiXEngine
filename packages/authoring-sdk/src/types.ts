import type {ProjectAssetDescriptor} from "./resources.js";
import type {StoryAssetIndex} from "./story-assets.js";
import type {StoryMacroLibrary} from "./story-macros.js";
import type {StoryPortraitIndex} from "./portrait.js";

export type DiagnosticSeverity = "error" | "warning" | "information";

export interface SourcePosition {
  readonly line: number;
  readonly column: number;
  /** UTF-8 byte offset in the authored source; column counts UTF-16 code units. */
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
  readonly schemaRevision: 1;
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
  readonly schemaRevision: 1;
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
  /** An unquoted display name (`@湊`): a character with that display name
   * speaks, otherwise it is a literal speaker like `@"湊"`. */
  readonly displaySpeaker?: boolean;
  readonly text?: string;
  readonly arguments?: readonly StoryArgument[];
  /** Inline `@speaker[...]` options that apply to one dialogue line. */
  readonly options?: readonly StoryArgument[];
  /** Set on lines expanded from a macro: its name and where it is defined. */
  readonly macroOrigin?: string;
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
  readonly schemaRevision: 1;
  /** TypeScript sources are bundled to JavaScript by the PrismatiX CLI. */
  readonly language: "javascript" | "typescript";
  readonly id: string;
  readonly version: string;
  readonly requiredEngineVersion: string;
  /** A .js entry, or a .ts entry that the CLI bundles (with its imports) into one .js module. */
  readonly entry: string;
  /** ES modules that the entry is allowed to import, relative to this manifest. */
  readonly modules?: readonly string[];
  readonly capabilities: readonly ExtensionCapability[];
  /**
   * The platforms the extension runs on; omitted means every platform. An
   * extension with `native` libraries runs only where it has one, never on
   * the Web.
   */
  readonly platforms?: readonly ExtensionPlatform[];
  /** Signed C ABI libraries (layer 2); see docs/native-extensions.md. */
  readonly native?: ExtensionNative;
  readonly safety?: ExtensionSafety;
  readonly commands: readonly ExtensionCommand[];
  readonly actions: readonly ExtensionAction[];
}

export type ExtensionCapability = "runtime" | "animation" | "ui" | "audio" | "video" | "persistence" | "input" | "render" |
  "raw.mesh" | "raw.texture" | "raw.target" | "native";

export type ExtensionPlatform = "win32-x64" | "linux-x64" | "darwin-x64" | "darwin-arm64" | "android-arm64" | "web";
/** The platforms a native library can be built for. */
export type NativeExtensionPlatform = "win32-x64" | "linux-x64" | "darwin-x64" | "darwin-arm64" | "android-arm64";

export interface ExtensionNativeFile {
  readonly path: string;
  /** Filled by pack. */
  readonly sha256?: string;
}

/**
 * A file the game's maker supplies (a vendor runtime that may not be
 * redistributed, such as Live2D's Cubism Core). It is never in a package,
 * never pinned by the manifest and not covered by the signature; packing a
 * game copies it from NativeUserFiles/ or PRISMATIX_NATIVE_USER_FILES into
 * the library's Plugins folder, where the library finds it through
 * `plugin_directory`.
 */
export interface ExtensionNativeUserFile {
  /** A file name (no folders). */
  readonly name: string;
  /** Where to get it, shown when it is missing and when installing. */
  readonly hint?: string;
  /** When given, the only accepted SHA-256 values (an allow list). */
  readonly sha256s?: readonly string[];
}

export interface ExtensionNativeLibrary {
  /** The .dll, .so or .dylib, relative to the manifest (`native/<platform>/<file>`). */
  readonly path: string;
  /** Filled by pack. */
  readonly sha256?: string;
  /** ECDSA P-256 over the native descriptor (`nativeSignatureMessage`), filled by pack. */
  readonly signature?: string;
  /** Libraries it loads from its own folder (pinned by SHA-256 and covered by the signature). */
  readonly files?: readonly ExtensionNativeFile[];
  /** Files the game's maker supplies; see ExtensionNativeUserFile. */
  readonly userFiles?: readonly ExtensionNativeUserFile[];
}

export interface ExtensionNative {
  /** The major PxHostApi ABI version the libraries are built against. */
  readonly abi: number;
  /** Service names the libraries register; the extension's script may call only these. */
  readonly services: readonly string[];
  /** Uncompressed P-256 public key (hex), filled by pack from --signing-key. */
  readonly publicKey?: string;
  readonly libraries: Readonly<Partial<Record<NativeExtensionPlatform, ExtensionNativeLibrary>>>;
}

export interface CharacterExpression {
  readonly id: string;
  readonly name: string;
  readonly aliases: readonly string[];
  readonly assetId: string;
  /** Face shown in the message window while this expression speaks. */
  readonly faceAssetId?: string;
}

export interface CharacterDocument {
  readonly format: "PrismatiXCharacter";
  readonly schemaRevision: 1;
  readonly id: string;
  readonly displayName: string;
  /** Optional Story speaker names by locale; falls back to displayName. */
  readonly displayNames?: Readonly<Record<string, string>>;
  /** Colour of the speaker name in the message window, `#RRGGBB(AA)`. */
  readonly nameColor?: string;
  readonly aliases: readonly string[];
  /** On-stage size used when a Story line gives none (see `CharacterStage`). */
  readonly stage?: CharacterStage;
  readonly defaultExpressionId?: string | null;
  readonly expressions: readonly CharacterExpression[];
}

/**
 * A character's default placement. A fresh `[show]` without `scale=` / `y=`
 * uses these instead of 1 and 0; a character already on stage keeps its own.
 */
export interface CharacterStage {
  /** Sprite scale, > 0 (1 = native pixels). */
  readonly scale?: number;
  /** Vertical offset in pixels from standing on the bottom edge; positive is down. */
  readonly y?: number;
}

export interface GameVariable {
  readonly name: string;
  readonly type: "boolean" | "integer" | "number" | "string";
  readonly default: JsonPrimitive;
  readonly scope: "session" | "profile";
}

interface GameDocumentBase {
  readonly format: "PrismatiXGame";
  readonly variables: readonly GameVariable[];
  readonly gallery?: readonly JsonValue[];
  readonly unlockables?: readonly JsonValue[];
  readonly inputBindings?: readonly JsonValue[];
}

export type GameDocument = GameDocumentBase & {readonly schemaRevision: 1;
  readonly inputActions?: readonly GameInputAction[]};

/** Portable named input actions; hosts may bind additional behavior to an id. */
export interface GameInputAction {
  readonly id: string;
  readonly context: string;
  readonly keys?: readonly string[];
  /** Wheel movement needed in one frame; omit threshold for any nonzero movement. */
  readonly wheel?: {readonly direction: "up" | "down"; readonly threshold?: number} |
                   {readonly direction: "none"};
  /** Player route opened when this action is pressed in its context. */
  readonly route?: string;
  /** Dispatch this command through the Player UI action sink. */
  readonly uiAction?: {readonly command: string; readonly argument?: string};
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
  /** Short-name lookup for resources under the project's `storyAssets` roots. */
  readonly storyAssets?: StoryAssetIndex;
  /** Project-wide macros from the `storyMacros` files; see createStoryMacroLibrary. */
  readonly storyMacros?: StoryMacroLibrary;
  /** Layered portrait descriptors for `[face]`; see createStoryPortraitIndex. */
  readonly portraits?: StoryPortraitIndex;
}

export interface StoryCompileResult {
  readonly runtimeIr?: RuntimeIrDocument;
  readonly sourceMap?: RuntimeSourceMap;
  /** Project-relative binary/text resources required by the emitted program. */
  readonly resourceDependencies?: readonly string[];
  /** First authored command responsible for each resolved resource. */
  readonly resourceLocations?: readonly {readonly source: string;
    readonly span: SourceSpan}[];
  readonly diagnostics: readonly AuthoringDiagnostic[];
  readonly valid: boolean;
}
