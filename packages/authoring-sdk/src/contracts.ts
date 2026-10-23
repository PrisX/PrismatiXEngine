import {Ajv2020, type ErrorObject, type ValidateFunction} from "ajv/dist/2020.js";

import {canonicalJson} from "./canonical-json.js";
import {contractHashes, contractManifest, contractSchemas} from "./generated/contracts.js";
import {extensionShapeDiagnostics, validateSemantics, validateUiPropertyUsage, type UiTypeRegistryMetadata} from "./semantic.js";
import type {AuthoringDiagnostic, JsonValue, Result} from "./types.js";

const maximumDocumentBytes = 16 * 1024 * 1024;
const maximumSaveMigrationBytes = 4 * 1024 * 1024;
// Compiled aggregate artifacts share the native RuntimeIR/SourceMap 64 MiB
// budget. Save migrations have a smaller native reader limit.
export function contractDocumentByteLimit(contractId: string): number {
  if (contractId === "saveMigration") return maximumSaveMigrationBytes;
  return ["runtimeIr", "sourceMap"].includes(contractId) ? 64 * 1024 * 1024 : maximumDocumentBytes;
}
const ajv = new Ajv2020({allErrors: true, strict: true, allowUnionTypes: true});
for (const schema of Object.values(contractSchemas)) ajv.addSchema(schema);

interface ContractDescriptor {
  readonly id: string;
  readonly schema: string;
  readonly schemaRevision: number;
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
  const explained = contractId === "extension" ? extensionShapeDiagnostics(value, path) : [];
  // The plain-language reasons replace the schema's cross-field errors they explain.
  const errors = (validator.errors ?? []).filter((error) => explained.length === 0 || !explainedAjvError(error));
  const diagnostics = [...explained, ...errors.map((error) =>
    diagnostic("PXSDKJSON1004", "Document does not match its canonical contract", path, formatAjvError(error)))];
  return {diagnostics, valid: false};
}

function explainedAjvError(error: ErrorObject): boolean {
  if (!error.schemaPath.startsWith("#/allOf/")) return false;
  return ["if", "false schema", "maxItems"].includes(error.keyword) ||
    (error.keyword === "required" && (error.params as {missingProperty?: string}).missingProperty === "studio");
}

/**
 * Validates a value against a definition of a contract schema that is not a
 * document of its own, by schema $id and JSON pointer, e.g.
 * `validateContractDefinition("extension-registry.schema.json#/$defs/version", value)`.
 */
export function validateContractDefinition<T>(reference: string, value: unknown, path?: string): Result<T> {
  const absolute = reference.startsWith("https://") ? reference : `https://prismatix.dev/contracts/${reference}`;
  let validator = validators.get(absolute);
  if (validator === undefined) {
    validator = ajv.getSchema(absolute);
    if (validator === undefined) throw new RangeError(`Unknown PrismatiX contract definition: ${reference}`);
    validators.set(absolute, validator);
  }
  if (validator(value)) return {value: value as T, diagnostics: [], valid: true};
  return {valid: false, diagnostics: (validator.errors ?? []).map((error) =>
    diagnostic("PXSDKJSON1004", "Document does not match its canonical contract", path, formatAjvError(error)))};
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
  const limit = contractDocumentByteLimit(contractId);
  if (Buffer.byteLength(text, "utf8") > limit) {
    return {diagnostics: [diagnostic("PXSDKJSON1001", `Document exceeds the ${limit / 1024 / 1024} MiB limit`, path)], valid: false};
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
    const text = canonicalJson(value);
    if (Buffer.byteLength(text, "utf8") > contractDocumentByteLimit(contractId))
      return {diagnostics: [diagnostic("PXSDKJSON1001", "Serialized document exceeds its native byte limit")], valid: false};
    return {value: text, diagnostics: [], valid: true};
  } catch (error) {
    return {diagnostics: [diagnostic("PXSDKJSON1005", "Document cannot be serialized deterministically", undefined, String(error))], valid: false};
  }
}

export {contractHashes, contractManifest, contractSchemas};
