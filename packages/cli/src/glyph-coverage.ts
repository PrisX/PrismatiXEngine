// Warns about Story characters that no font of the locale's fontChain can
// draw. The Player would show them as empty boxes (tofu), which nothing else
// catches before players see it: a Japanese name such as 凪 in a Traditional
// Chinese font is a typical case.
import {readFile} from "node:fs/promises";
import type {AuthoringDiagnostic, BuildArtifact} from "@prismatix/authoring-sdk";

/** Sorted, merged [first, last] code point ranges a font maps to glyphs. */
type Coverage = readonly (readonly [number, number])[];

function u16(view: DataView, offset: number): number { return view.getUint16(offset); }
function u32(view: DataView, offset: number): number { return view.getUint32(offset); }

/** Reads the cmap of a TrueType/OpenType font (or the first face of a collection). */
export function fontCoverage(bytes: Uint8Array): Coverage | undefined {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.byteLength < 12) return undefined;
  let base = 0;
  if (u32(view, 0) === 0x74746366 /* ttcf */) base = u32(view, 12);
  const tables = u16(view, base + 4);
  let cmap = -1;
  for (let index = 0; index < tables; index += 1) {
    const record = base + 12 + index * 16;
    if (u32(view, record) === 0x636d6170 /* cmap */) cmap = u32(view, record + 8);
  }
  if (cmap < 0) return undefined;
  const ranges: [number, number][] = [];
  const subtables = u16(view, cmap + 2);
  // Prefer the full-repertoire format 12 table; format 4 covers the BMP.
  let best = -1;
  let bestFormat = 0;
  for (let index = 0; index < subtables; index += 1) {
    const offset = cmap + u32(view, cmap + 4 + index * 8 + 4);
    const format = u16(view, offset);
    if ((format === 12 && bestFormat !== 12) || (format === 4 && bestFormat === 0)) {
      best = offset;
      bestFormat = format;
    }
  }
  if (best < 0) return undefined;
  if (bestFormat === 12) {
    const groups = u32(view, best + 12);
    for (let index = 0; index < groups; index += 1) {
      const group = best + 16 + index * 12;
      if (u32(view, group + 8) !== 0) ranges.push([u32(view, group), u32(view, group + 4)]);
    }
  } else {
    const segments = u16(view, best + 6) / 2;
    const ends = best + 14;
    const starts = ends + segments * 2 + 2;
    const deltas = starts + segments * 2;
    const rangeOffsets = deltas + segments * 2;
    for (let index = 0; index < segments; index += 1) {
      const end = u16(view, ends + index * 2);
      const start = u16(view, starts + index * 2);
      const delta = u16(view, deltas + index * 2);
      const rangeOffset = u16(view, rangeOffsets + index * 2);
      if (start === 0xffff) continue;
      // Code points whose glyph id comes out 0 are unmapped.
      let runStart = -1;
      for (let code = start; code <= end; code += 1) {
        let glyph: number;
        if (rangeOffset === 0) glyph = (code + delta) & 0xffff;
        else {
          const address = rangeOffsets + index * 2 + rangeOffset + (code - start) * 2;
          glyph = address + 1 < bytes.byteLength ? u16(view, address) : 0;
          if (glyph !== 0) glyph = (glyph + delta) & 0xffff;
        }
        if (glyph !== 0 && runStart < 0) runStart = code;
        if ((glyph === 0 || code === end) && runStart >= 0) {
          ranges.push([runStart, glyph === 0 ? code - 1 : code]);
          runStart = -1;
        }
      }
    }
  }
  ranges.sort((left, right) => left[0] - right[0]);
  return ranges;
}

function covers(coverage: Coverage, code: number): boolean {
  let low = 0;
  let high = coverage.length - 1;
  while (low <= high) {
    const middle = (low + high) >> 1;
    const [first, last] = coverage[middle]!;
    if (code < first) high = middle - 1;
    else if (code > last) low = middle + 1;
    else return true;
  }
  return false;
}

// Characters every renderer handles without a glyph, or that never draw.
function ignorable(code: number): boolean {
  return code < 0x80 || code === 0x3000 || (code >= 0xfe00 && code <= 0xfe0f) ||
    (code >= 0x200b && code <= 0x200f) || code === 0xfeff;
}

/**
 * Story text (dialogue, narration, speakers, choices, chapter titles) per
 * locale, checked against that locale's fontChain. One warning per locale
 * lists each missing character with its first use.
 */
export async function storyGlyphWarnings(artifact: BuildArtifact,
  locales: Readonly<Record<string, unknown>>,
  readProjectFile: (path: string) => Promise<Uint8Array>): Promise<AuthoringDiagnostic[]> {
  const warnings: AuthoringDiagnostic[] = [];
  const fonts = new Map<string, Coverage | undefined>();
  for (const [locale, entry] of Object.entries(artifact.localeArtifacts)) {
    let document = locales[locale] as {fontChain?: unknown} | undefined;
    if (document === undefined) {
      // A single-locale project lists no supportedLocales; read its file.
      try {
        const text = new TextDecoder().decode(await readProjectFile(`Content/Localization/${locale}.json`));
        document = JSON.parse(text.replace(/^﻿/u, "")) as {fontChain?: unknown};
      } catch { continue; }
    }
    const chain = Array.isArray(document?.fontChain)
      ? document!.fontChain.filter((path): path is string => typeof path === "string") : [];
    if (chain.length === 0) continue;
    const coverages: Coverage[] = [];
    for (const path of chain) {
      if (!fonts.has(path)) {
        try { fonts.set(path, fontCoverage(await readProjectFile(path))); }
        catch { fonts.set(path, undefined); }
      }
      const coverage = fonts.get(path);
      if (coverage !== undefined) coverages.push(coverage);
    }
    if (coverages.length === 0) continue;
    const where = new Map(entry.sourceMap.mappings.map((mapping) =>
      [mapping.operationId, `${mapping.sourceUri}:${mapping.startLine}`]));
    const missing = new Map<string, string>();
    for (const operation of entry.runtimeIr.operations) {
      const args = operation.arguments as Record<string, unknown>;
      for (const key of ["text", "speaker", "title"]) {
        const value = args[key];
        if (typeof value !== "string") continue;
        for (const character of value) {
          const code = character.codePointAt(0)!;
          if (ignorable(code) || missing.has(character)) continue;
          if (!coverages.some((coverage) => covers(coverage, code)))
            missing.set(character, where.get(operation.operationId) ?? operation.operationId);
        }
      }
    }
    if (missing.size > 0)
      warnings.push({severity: "warning", code: "PXCLI1060",
        message: `${missing.size} Story character(s) have no glyph in any ${locale} fontChain font and would ` +
          "show as empty boxes; add a font that covers them to the locale's fontChain",
        details: [...missing].slice(0, 40).map(([character, first]) =>
          `${character} U+${character.codePointAt(0)!.toString(16).toUpperCase().padStart(4, "0")} first at ${first}`)
          .join("\n")} as AuthoringDiagnostic);
  }
  return warnings;
}
