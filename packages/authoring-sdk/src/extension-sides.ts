// The two sides of an extension (docs/studio-extensions.md). One manifest,
// one id, one version and one package hold a game side (`entry` and the
// fields beside it, which the Player runs) and, from schemaRevision 2, a
// Studio side (`studio`, which PrismatiX Studio loads in a sandboxed frame).
// A game only ever sees the game side: a Studio-only extension is skipped and
// a manifest with both reaches the Player as revision 1 without `studio`.
//
// Pure JavaScript, like extension-package.ts.

import type {ExtensionManifest, ExtensionStudioBuiltinAssetKind, GameExtensionManifest} from "./types.js";

export type ExtensionSide = "game" | "studio";

/** The schemaRevision that allows `studio` and a manifest without a game side. */
export const STUDIO_EXTENSION_SCHEMA_REVISION = 2;

/** Asset kinds an inspector may name without the extension declaring them. */
export const studioBuiltinAssetKinds: readonly ExtensionStudioBuiltinAssetKind[] = ["image", "audio", "video", "font", "text"];

/** True when the manifest has a game side (an `entry`). */
export function hasGameSide(manifest: Pick<ExtensionManifest, "entry">): manifest is GameExtensionManifest {
  return typeof manifest.entry === "string";
}

/** The sides a manifest has, game first. */
export function extensionSides(manifest: Pick<ExtensionManifest, "entry" | "studio">): ExtensionSide[] {
  return [...(typeof manifest.entry === "string" ? ["game" as const] : []),
    ...(manifest.studio !== undefined ? ["studio" as const] : [])];
}

/**
 * The manifest a game receives for an extension with a game side: revision 1
 * without `studio`, which is what the Player reads. A revision-1 manifest is
 * returned as it is.
 */
export function gameExtensionManifest<T extends GameExtensionManifest>(manifest: T): T {
  if (manifest.schemaRevision === 1 && manifest.studio === undefined) return manifest;
  const {studio: _studio, ...rest} = manifest;
  return {...rest, schemaRevision: 1} as T;
}

/** A contribution's fully qualified id: `<extension id>:<contribution id>`, e.g. `owen/live2d:motions`. */
export function qualifiedContributionId(extensionId: string, contributionId: string): string {
  return `${extensionId}:${contributionId}`;
}

/** A provider id of `optionsFrom`, split at its last ":"; undefined when it is not fully qualified. */
export function parseContributionId(value: string): {readonly extension: string; readonly id: string} | undefined {
  const match = /^((?:[A-Za-z0-9][A-Za-z0-9._-]*\/)?[A-Za-z0-9][A-Za-z0-9._-]*):([A-Za-z0-9][A-Za-z0-9._-]*)$/u.exec(value);
  return match === null ? undefined : {extension: match[1]!, id: match[2]!};
}

/** True for a well-formed RFC 6901 JSON pointer ("" or "/a/b", with "~" only as "~0" or "~1"). */
export function isJsonPointer(value: string): boolean {
  return value === "" || (value.startsWith("/") && !/~(?![01])/u.test(value));
}
