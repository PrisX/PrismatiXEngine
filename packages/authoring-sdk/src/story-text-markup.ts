/**
 * Inline dialogue markup: `[br]`, `[ruby=reading]base[/ruby]` and the style
 * tags `[font=path]`, `[size=N|N%]`, `[color=#RRGGBB(AA)]`, `[b]` and `[i]`,
 * each closed by its `[/name]`. The Player keeps the markup in dialogue,
 * backlog and saves; this checks it the way the Player will read it.
 * Reveal tags `[w 500ms]`, `[speed 50%]`...`[/speed]` and `[nowait]`...
 * `[/nowait]` only time the typewriter; the Player drops them from the text.
 */

export interface TextMarkupIssue {
  readonly code: string;
  readonly severity: "error" | "warning";
  readonly message: string;
}

export interface TextMarkupReport {
  readonly issues: readonly TextMarkupIssue[];
  /** `[font=...]` values, in authored order, for resource resolution. */
  readonly fonts: readonly string[];
}

const styleTags = new Set(["font", "size", "color", "b", "i"]);
const valuedTags = new Set(["font", "size", "color"]);

/** `N` logical pixels (1..400) or `N%` of the enclosing size (10..400). */
export function validTextSize(value: string): boolean {
  const match = /^(\d+(?:\.\d+)?)(%?)$/u.exec(value);
  if (match === null) return false;
  const amount = Number(match[1]);
  return match[2] === "%" ? amount >= 10 && amount <= 400 : amount >= 1 && amount <= 400;
}

export function validTextColor(value: string): boolean {
  return /^#(?:[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})$/u.test(value);
}

export function inspectTextMarkup(text: string): TextMarkupReport {
  const issues: TextMarkupIssue[] = [];
  const fonts: string[] = [];
  const open: string[] = [];
  const revealOpen: string[] = [];
  let inRuby = false;
  for (let index = text.indexOf("["); index >= 0; index = text.indexOf("[", index + 1)) {
    const close = text.indexOf("]", index);
    if (close < 0) break;
    const tag = text.slice(index + 1, close);
    // Reveal tags: [w 500ms], [speed 50%]...[/speed], [nowait]...[/nowait].
    const reveal = revealTag(tag);
    if (reveal !== undefined) {
      if (reveal.error !== undefined) {
        issues.push({code: "PXSTORY1185", severity: "error", message: reveal.error});
      } else if (inRuby) {
        issues.push({code: "PXSTORY1184", severity: "warning",
          message: `[${tag}] inside a ruby base is shown as written; put it around [ruby]`});
      } else if (reveal.closes !== undefined) {
        const opened = revealOpen.lastIndexOf(reveal.closes);
        if (opened < 0) issues.push({code: "PXSTORY1182", severity: "warning",
          message: `[${tag}] closes no open [${reveal.closes}]`});
        else revealOpen.splice(opened, 1);
      } else if (reveal.opens !== undefined) revealOpen.push(reveal.opens);
      continue;
    }
    const closing = tag.startsWith("/");
    const equals = tag.indexOf("=");
    const name = (closing ? tag.slice(1) : equals < 0 ? tag : tag.slice(0, equals));
    const value = equals < 0 ? undefined : tag.slice(equals + 1);
    if (tag === "br") continue;
    if (name === "ruby" && !closing && value !== undefined) { inRuby = true; continue; }
    if (tag === "/ruby") { inRuby = false; continue; }
    if (!styleTags.has(name) || (closing && value !== undefined) ||
        (!closing && valuedTags.has(name) !== (value !== undefined))) {
      issues.push({code: "PXSTORY1180", severity: "warning",
        message: `[${tag}] is not a text tag; the Player hides it`});
      continue;
    }
    if (inRuby) {
      issues.push({code: "PXSTORY1184", severity: "warning",
        message: `[${tag}] inside a ruby base is shown as written; put style tags around [ruby]`});
      continue;
    }
    if (closing) {
      const opened = open.lastIndexOf(name);
      if (opened < 0) issues.push({code: "PXSTORY1182", severity: "warning",
        message: `[/${name}] closes no open [${name}]`});
      else open.splice(opened, 1);
      continue;
    }
    if (name === "size" && !validTextSize(value!))
      issues.push({code: "PXSTORY1181", severity: "error",
        message: `[size=${value}] must be 1-400 pixels or 10%-400%`});
    else if (name === "color" && !validTextColor(value!))
      issues.push({code: "PXSTORY1181", severity: "error",
        message: `[color=${value}] must be #RRGGBB or #RRGGBBAA`});
    else if (name === "font" && value!.trim() === "")
      issues.push({code: "PXSTORY1181", severity: "error", message: "[font=] needs a font file"});
    else if (name === "font") fonts.push(value!);
    open.push(name);
  }
  for (const name of new Set([...open, ...revealOpen]))
    issues.push({code: "PXSTORY1182", severity: "warning",
      message: `[${name}] is not closed; it lasts to the end of the line`});
  return {issues, fonts};
}

/** `[w 500ms]` (up to 60s), `[speed N%]` (10%-1000%), `[/speed]`, `[nowait]`, `[/nowait]`. */
function revealTag(tag: string): {readonly opens?: string; readonly closes?: string;
  readonly error?: string} | undefined {
  if (tag === "nowait") return {opens: "nowait"};
  if (tag === "/nowait") return {closes: "nowait"};
  if (tag === "/speed") return {closes: "speed"};
  const wait = /^w (.*)$/u.exec(tag);
  if (wait !== null) {
    const match = /^(\d+(?:\.\d+)?)(ms|s)?$/u.exec(wait[1]!);
    const milliseconds = match === null ? NaN : Number(match[1]) * (match[2] === "s" ? 1000 : 1);
    return Number.isFinite(milliseconds) && milliseconds <= 60_000 ? {}
      : {error: `[${tag}] must pause 0-60s, for example [w 500ms]`};
  }
  const speed = /^speed (.*)$/u.exec(tag);
  if (speed !== null) {
    const match = /^(\d+(?:\.\d+)?)%$/u.exec(speed[1]!);
    return match !== null && Number(match[1]) >= 10 && Number(match[1]) <= 1000 ? {opens: "speed"}
      : {error: `[${tag}] must be a reveal speed of 10%-1000%, for example [speed 50%]`};
  }
  return undefined;
}
