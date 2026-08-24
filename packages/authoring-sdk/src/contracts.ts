import {Ajv2020, type ErrorObject, type ValidateFunction} from "ajv/dist/2020.js";

import {canonicalJson} from "./canonical-json.js";
import {contractHashes, contractManifest, contractSchemas} from "./generated/contracts.js";
import {validateSemantics, validateUiPropertyUsage, type UiTypeRegistryMetadata} from "./semantic.js";
import type {AuthoringDiagnostic, JsonValue, Result} from "./types.js";

const maximumDocumentBytes = 16 * 1024 * 1024;
const ajv = new Ajv2020({allErrors: true, strict: true, allowUnionTypes: true});
for (const schema of Object.values(contractSchemas)) ajv.addSchema(schema);

interface ContractDescriptor {
  readonly id: string;
  readonly schema: string;
  readonly schemaRevision: number;
  readonly migration: string;
}

const descriptors = new Map<string, ContractDescriptor>();
for (const value of contractManifest.contracts as readonly unknown[]) {
  const descriptor = value as ContractDescriptor;
  descriptors.set(descriptor.id, descriptor);
}

const validators = new Map<string, ValidateFunction>();

function diagnostic(code: string, message: string, path?: string, details?: string): AuthoringDiagnostic {
  return {
    severity: "error",
    code,
    message,
    ...(path === undefined ? {} : {path}),
    ...(details === undefined ? {} : {details}),
  };
}

function formatAjvError(error: ErrorObject): string {
  const location = error.instancePath.length === 0 ? "/" : error.instancePath;
  return `${location}: ${error.message ?? error.keyword}`;
}

function validatorFor(contractId: string): ValidateFunction {
  const cached = validators.get(contractId);
  if (cached) return cached;
  const descriptor = descriptors.get(contractId);
  if (!descriptor) throw new RangeError(`Unknown PrismatiX contract: ${contractId}`);
  const schema = contractSchemas[descriptor.schema];
  if (!schema) throw new Error(`Missing generated schema: ${descriptor.schema}`);
  const validator = ajv.compile(schema);
  validators.set(contractId, validator);
  return validator;
}

export function validateDocument<T>(contractId: string, value: unknown, path?: string): Result<T> {
  const validator = validatorFor(contractId);
  if (validator(value)) {
    const diagnostics = validateSemantics(contractId, value, path);
    return diagnostics.length === 0
      ? {value: value as T, diagnostics: [], valid: true}
      : {diagnostics, valid: false};
  }
  const diagnostics = (validator.errors ?? []).map((error) =>
    diagnostic("PXSDKJSON1004", "Document does not match its canonical contract", path, formatAjvError(error)),
  );
  return {diagnostics, valid: false};
}

export function validateUiDocument<T>(value: unknown, registry: UiTypeRegistryMetadata, path?: string): Result<T> {
  const base = validateDocument<T>("ui", value, path);
  if (!base.valid) return base;
  const diagnostics = validateUiPropertyUsage(value, registry, path);
  return diagnostics.length === 0
    ? base
    : {diagnostics, valid: false};
}

export function parseDocument<T>(contractId: string, text: string, path?: string): Result<T> {
  if (Buffer.byteLength(text, "utf8") > maximumDocumentBytes) {
    return {diagnostics: [diagnostic("PXSDKJSON1001", "Document exceeds the 16 MiB limit", path)], valid: false};
  }
  if (text.startsWith("\uFEFF")) {
    return {diagnostics: [diagnostic("PXSDKJSON1002", "Canonical UTF-8 documents must not contain a BOM", path)], valid: false};
  }
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch (error) {
    return {diagnostics: [diagnostic("PXSDKJSON1003", "Document is not valid JSON", path, String(error))], valid: false};
  }
  return validateDocument<T>(contractId, value, path);
}

export function serializeDocument<T extends JsonValue>(contractId: string, value: T): Result<string> {
  const validation = validateDocument<T>(contractId, value);
  if (!validation.valid) return validation as Result<string>;
  try {
    return {value: canonicalJson(value), diagnostics: [], valid: true};
  } catch (error) {
    return {diagnostics: [diagnostic("PXSDKJSON1005", "Document cannot be serialized deterministically", undefined, String(error))], valid: false};
  }
}

export function migrateDocument(contractId: string, value: unknown): Result<unknown> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    return {diagnostics: [diagnostic("PXSDKMIG1001", "Migration input must be an object")], valid: false};
  }
  const source = value as Record<string, unknown>;
  if (contractId === "animation" && source.version === 4 && source.schemaRevision === undefined) {
    const migrated = {...source, schemaRevision: 1};
    return validateDocument(contractId, migrated);
  }
  if (contractId === "ui" && source.schemaRevision === 1) {
    const nodes = Array.isArray(source.nodes) ? source.nodes.map((value) => {
      const node = value as Record<string, unknown>;
      const layout = (node.layout ?? {}) as Record<string, unknown>;
      const content = (node.content ?? {}) as Record<string, unknown>;
      const appearance = (node.appearance ?? {}) as Record<string, unknown>;
      const interaction = (node.interaction ?? {}) as Record<string, unknown>;
      const accessibility = (node.accessibility ?? {}) as Record<string, unknown>;
      const migratedNode: Record<string, unknown> = {
        id: node.id,
        parentId: node.parentId ?? null,
        order: node.order ?? 0,
        kind: node.kind,
        ...(node.runtimeType === undefined ? {} : {runtimeType: node.runtimeType}),
        name: node.name,
        visible: node.visible ?? true,
        locked: node.locked ?? false,
        layout: {
          mode: layout.mode ?? "free",
          x: layout.x ?? 0, y: layout.y ?? 0,
          width: layout.width ?? 0, height: layout.height ?? 0,
          anchorX: layout.anchorX ?? 0, anchorY: layout.anchorY ?? 0,
          anchorRight: layout.anchorRight ?? layout.anchorX ?? 0,
          anchorBottom: layout.anchorBottom ?? layout.anchorY ?? 0,
          pivotX: layout.pivotX ?? 0, pivotY: layout.pivotY ?? 0,
          margin: layout.margin ?? 0, alignment: layout.alignment ?? "start",
          sizeRule: layout.sizeRule ?? "fixed",
        },
        ...(typeof content.text === "string" ? {text: content.text} : {}),
        ...(content.assetId === undefined ? {} : {assetId: content.assetId}),
        appearance: {
          ...(appearance.backgroundColor === undefined ? {} : {backgroundColor: appearance.backgroundColor}),
          ...(appearance.textColor === undefined ? {} : {textColor: appearance.textColor}),
          ...(appearance.opacity === undefined ? {} : {opacity: appearance.opacity}),
          ...(appearance.styleToken === undefined ? {} : {styleToken: appearance.styleToken}),
        },
        ...(interaction.onClick === null || interaction.onClick === undefined ? {} : {onClick: interaction.onClick}),
        ...(typeof accessibility.label === "string" ? {accessibilityLabel: accessibility.label} : {}),
        ...(typeof accessibility.role === "string" ? {accessibilityRole: accessibility.role} : {}),
        runtimeProperties: node.runtimeProperties ?? {},
        bindings: node.bindings ?? {},
      };
      return migratedNode;
    }) : [];
    const migrated = {
      format: source.format,
      schemaRevision: 2,
      id: source.id,
      revision: source.revision,
      name: source.name,
      width: source.width,
      height: source.height,
      rootId: source.rootId,
      nodes,
      theme: source.theme ?? [],
      behaviorGraph: source.behaviorGraph ?? {nodes: [], links: [], groups: []},
      behaviorTriggers: source.behaviorTriggers ?? [],
      visualStateGroups: source.visualStateGroups ?? [],
      ...(source.animations === undefined ? {} : {animations: source.animations}),
    };
    return validateDocument(contractId, migrated);
  }
  return validateDocument(contractId, value);
}

export {contractHashes, contractManifest, contractSchemas};
