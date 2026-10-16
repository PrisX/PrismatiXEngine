// A small terminal UI for the interactive parts of the CLI: prompts, a
// spinner and framed log lines in the style of modern scaffolding tools.
// It has no dependencies, so `create` keeps working from an offline cache.
// Everything renders to stderr; stdout stays free for command results.
import {emitKeypressEvents} from "node:readline";

export const CANCEL: unique symbol = Symbol("prismatix.cancel");
export type Cancel = typeof CANCEL;
export const isCancel = (value: unknown): value is Cancel => value === CANCEL;

const output = process.stderr;
const input = process.stdin;

/** Prompts need a keyboard and a screen; scripts, CI and --json never prompt. */
export function canPrompt(): boolean {
  return Boolean(input.isTTY && output.isTTY) && !process.argv.includes("--json") &&
    (process.env.CI === undefined || process.env.CI === "" || process.env.CI === "false");
}

const colorEnabled = ((): boolean => {
  if (process.env.NO_COLOR !== undefined && process.env.NO_COLOR !== "") return false;
  if (process.env.FORCE_COLOR !== undefined && process.env.FORCE_COLOR !== "0") return true;
  return Boolean(output.isTTY) && process.env.TERM !== "dumb";
})();
const paint = (open: number, close: number) => (text: string): string =>
  colorEnabled ? `\x1b[${open}m${text}\x1b[${close}m` : text;
export const color = {
  bold: paint(1, 22), dim: paint(2, 22), italic: paint(3, 23), inverse: paint(7, 27),
  strike: paint(9, 29), red: paint(31, 39), green: paint(32, 39), yellow: paint(33, 39),
  blue: paint(34, 39), magenta: paint(35, 39), cyan: paint(36, 39), gray: paint(90, 39),
};

const symbol = {
  start: "┌", bar: "│", end: "└", active: "◆", done: "◇", cancel: "■", warn: "▲", error: "■",
  on: "●", off: "○", corner: "╮", cornerEnd: "╯", tee: "├", line: "─",
};

const ansi = /\x1b\[[0-9;?]*[A-Za-z]/gu;

/** Terminal columns a string occupies: CJK and emoji are two wide. */
export function displayWidth(text: string): number {
  let width = 0;
  for (const character of text.replace(ansi, "")) {
    const code = character.codePointAt(0)!;
    if (code < 0x20 || (code >= 0x300 && code <= 0x36f) || code === 0x200d || (code >= 0xfe00 && code <= 0xfe0f))
      continue;
    const wide = (code >= 0x1100 && code <= 0x115f) || (code >= 0x2e80 && code <= 0xa4cf) ||
      (code >= 0xac00 && code <= 0xd7a3) || (code >= 0xf900 && code <= 0xfaff) ||
      (code >= 0xfe30 && code <= 0xfe4f) || (code >= 0xff00 && code <= 0xff60) ||
      (code >= 0xffe0 && code <= 0xffe6) || (code >= 0x1f300 && code <= 0x1faff) ||
      (code >= 0x20000 && code <= 0x3fffd);
    width += wide ? 2 : 1;
  }
  return width;
}

/** Rows a block occupies once the terminal wraps its long lines. */
function screenRows(text: string): number {
  const columns = Math.max(20, output.columns ?? 80);
  return text.split("\n").reduce((rows, line) =>
    rows + Math.max(1, Math.ceil(displayWidth(line) / columns)), 0);
}

const write = (text: string): void => { output.write(text); };

export function intro(title: string): void {
  write(`${color.gray(symbol.start)}  ${color.inverse(` ${title} `)}\n`);
}

export function outro(message: string): void {
  write(`${color.gray(symbol.bar)}\n${color.gray(symbol.end)}  ${message}\n\n`);
}

export const log = {
  message(message: string, mark = color.gray(symbol.bar)): void {
    const [first = "", ...rest] = message.split("\n");
    write(`${color.gray(symbol.bar)}\n${mark}  ${first}\n${rest.map((line) =>
      `${color.gray(symbol.bar)}  ${line}\n`).join("")}`);
  },
  info(message: string): void { log.message(message, color.blue(symbol.on)); },
  step(message: string): void { log.message(message, color.green(symbol.done)); },
  success(message: string): void { log.message(message, color.green(symbol.active)); },
  warn(message: string): void { log.message(message, color.yellow(symbol.warn)); },
  error(message: string): void { log.message(message, color.red(symbol.error)); },
};

/** A framed block, used for "next steps". */
export function note(title: string, lines: readonly string[]): void {
  const width = Math.max(displayWidth(title) + 2, ...lines.map(displayWidth)) + 2;
  const pad = (line: string) => `${line}${" ".repeat(width - displayWidth(line))}`;
  const bar = color.gray(symbol.bar);
  write(`${bar}\n${color.green(symbol.done)}  ${title} ${color.gray(
    `${symbol.line.repeat(Math.max(1, width - displayWidth(title) - 1))}${symbol.corner}`)}\n`);
  write(`${bar}  ${" ".repeat(width)}${bar}\n`);
  for (const line of lines) write(`${bar}  ${pad(line)}${bar}\n`);
  write(`${bar}  ${" ".repeat(width)}${bar}\n`);
  write(`${color.gray(`${symbol.tee}${symbol.line.repeat(width + 2)}${symbol.cornerEnd}`)}\n`);
}

/** Final line for a cancelled flow. */
export function cancelled(message: string): void {
  write(`${color.gray(symbol.end)}  ${color.red(message)}\n\n`);
}

type Status = "active" | "error" | "submit" | "cancel";
interface Key {
  readonly name?: string; readonly ctrl?: boolean; readonly meta?: boolean; readonly shift?: boolean;
  readonly sequence?: string;
}

/**
 * Runs one prompt: redraws `render(status)` in place after every key until
 * `onKey` submits or the user cancels with Ctrl+C or Esc.
 */
function runPrompt<T>(render: (status: Status) => string,
  onKey: (text: string | undefined, key: Key) => {submit: T} | "error" | void): Promise<T | Cancel> {
  return new Promise((accept) => {
    let rows = 0;
    let status: Status = "active";
    const draw = (): void => {
      const frame = render(status);
      if (rows > 0) write(`${rows > 1 ? `\x1b[${rows - 1}A` : ""}\r\x1b[J`);
      write(frame);
      rows = screenRows(frame);
    };
    const finish = (value: T | Cancel): void => {
      input.removeListener("keypress", keypress);
      if (input.isTTY) input.setRawMode(false);
      input.pause();
      // A paused stdin still holds the event loop open; let the CLI exit.
      (input as {unref?: () => void}).unref?.();
      write("\n\x1b[?25h");
      accept(value);
    };
    const keypress = (text: string | undefined, key: Key = {}): void => {
      if ((key.ctrl && key.name === "c") || key.name === "escape") {
        status = "cancel"; draw(); finish(CANCEL); return;
      }
      const result = onKey(text, key);
      if (result !== undefined && result !== "error") { status = "submit"; draw(); finish(result.submit); return; }
      status = result === "error" ? "error" : "active";
      draw();
    };
    emitKeypressEvents(input);
    if (input.isTTY) input.setRawMode(true);
    (input as {ref?: () => void}).ref?.();
    input.on("keypress", keypress);
    input.resume();
    write("\x1b[?25l");
    draw();
  });
}

const header = (status: Status, message: string): string => {
  const mark = status === "active" ? color.cyan(symbol.active)
    : status === "error" ? color.yellow(symbol.warn)
      : status === "cancel" ? color.red(symbol.cancel) : color.green(symbol.done);
  return `${color.gray(symbol.bar)}\n${mark}  ${message}\n`;
};
const barFor = (status: Status): string => status === "error" ? color.yellow(symbol.bar)
  : status === "active" ? color.cyan(symbol.bar) : color.gray(symbol.bar);
const endFor = (status: Status): string => status === "error" ? color.yellow(symbol.end) : color.cyan(symbol.end);

export interface TextOptions {
  readonly message: string;
  /** Shown dimmed and used when the answer is left empty. */
  readonly defaultValue?: string;
  readonly initialValue?: string;
  readonly validate?: (value: string) => string | undefined;
}

export function text(options: TextOptions): Promise<string | Cancel> {
  const characters = [...(options.initialValue ?? "")];
  let cursor = characters.length;
  let error = "";
  const current = (): string => characters.join("") || (options.defaultValue ?? "");
  return runPrompt<string>((status) => {
    const top = header(status, options.message);
    const value = characters.join("");
    if (status === "submit") return `${top}${color.gray(symbol.bar)}  ${color.dim(current())}`;
    if (status === "cancel") return `${top}${color.gray(symbol.bar)}  ${color.strike(color.dim(value))}`;
    let field: string;
    if (value.length === 0) {
      const placeholder = options.defaultValue ?? "";
      field = placeholder ? `${color.inverse(placeholder[0]!)}${color.dim(placeholder.slice(1))}` : color.inverse(" ");
    } else {
      field = characters.slice(0, cursor).join("") +
        color.inverse(characters[cursor] ?? " ") + characters.slice(cursor + 1).join("");
    }
    const footer = status === "error" ? `${endFor(status)}  ${color.yellow(error)}` : endFor(status);
    return `${top}${barFor(status)}  ${field}\n${footer}`;
  }, (typed, key) => {
    if (key.name === "return" || key.name === "enter") {
      const problem = options.validate?.(current());
      if (problem !== undefined) { error = problem; return "error"; }
      return {submit: current()};
    }
    if (key.name === "backspace") { if (cursor > 0) characters.splice(--cursor, 1); return; }
    if (key.name === "delete") { characters.splice(cursor, 1); return; }
    if (key.name === "left") { cursor = Math.max(0, cursor - 1); return; }
    if (key.name === "right") { cursor = Math.min(characters.length, cursor + 1); return; }
    if (key.name === "home" || (key.ctrl && key.name === "a")) { cursor = 0; return; }
    if (key.name === "end" || (key.ctrl && key.name === "e")) { cursor = characters.length; return; }
    if (key.ctrl && key.name === "u") { characters.splice(0, cursor); cursor = 0; return; }
    if (key.name === "tab" && characters.length === 0 && options.defaultValue) {
      characters.push(...options.defaultValue); cursor = characters.length; return;
    }
    if (typed !== undefined && !key.ctrl && !key.meta) {
      const printable = [...typed].filter((character) => character >= " " && character !== "\x7f");
      characters.splice(cursor, 0, ...printable);
      cursor += printable.length;
    }
  });
}

export interface SelectOption<T> { readonly value: T; readonly label: string; readonly hint?: string | undefined }

export function select<T>(options: {readonly message: string; readonly options: readonly SelectOption<T>[];
  readonly initialValue?: T}): Promise<T | Cancel> {
  let index = Math.max(0, options.options.findIndex((option) => option.value === options.initialValue));
  return runPrompt<T>((status) => {
    const top = header(status, options.message);
    const chosen = options.options[index]!;
    if (status === "submit") return `${top}${color.gray(symbol.bar)}  ${color.dim(chosen.label)}`;
    if (status === "cancel") return `${top}${color.gray(symbol.bar)}  ${color.strike(color.dim(chosen.label))}`;
    const lines = options.options.map((option, position) => position === index
      ? `${barFor(status)}  ${color.green(symbol.on)} ${option.label}${option.hint ? `  ${color.dim(option.hint)}` : ""}`
      : `${barFor(status)}  ${color.dim(`${symbol.off} ${option.label}`)}`);
    return `${top}${lines.join("\n")}\n${endFor(status)}`;
  }, (typed, key) => {
    const count = options.options.length;
    if (key.name === "up" || key.name === "k" || (key.shift && key.name === "tab")) index = (index + count - 1) % count;
    else if (key.name === "down" || key.name === "j" || key.name === "tab") index = (index + 1) % count;
    else if (typed !== undefined && /^[1-9]$/u.test(typed) && Number(typed) <= count) index = Number(typed) - 1;
    else if (key.name === "return" || key.name === "enter") return {submit: options.options[index]!.value};
  });
}

export function confirm(options: {readonly message: string; readonly initialValue?: boolean;
  readonly active?: string; readonly inactive?: string}): Promise<boolean | Cancel> {
  let value = options.initialValue ?? true;
  const yes = options.active ?? "Yes";
  const no = options.inactive ?? "No";
  return runPrompt<boolean>((status) => {
    const top = header(status, options.message);
    if (status === "submit") return `${top}${color.gray(symbol.bar)}  ${color.dim(value ? yes : no)}`;
    if (status === "cancel") return `${top}${color.gray(symbol.bar)}  ${color.strike(color.dim(value ? yes : no))}`;
    const option = (on: boolean, label: string) => on === value
      ? `${color.green(symbol.on)} ${label}` : color.dim(`${symbol.off} ${label}`);
    return `${top}${barFor(status)}  ${option(true, yes)} ${color.dim("/")} ${option(false, no)}\n${endFor(status)}`;
  }, (typed, key) => {
    if (key.name === "left" || key.name === "right" || key.name === "tab" || key.name === "h" || key.name === "l")
      value = !value;
    else if (typed === "y" || typed === "Y") return {submit: value = true};
    else if (typed === "n" || typed === "N") return {submit: value = false};
    else if (key.name === "return" || key.name === "enter") return {submit: value};
  });
}

/** A spinner line; without a terminal it prints plain start and finish lines. */
export function spinner(): {start(message: string): void; message(message: string): void;
  stop(message: string, failed?: boolean): void} {
  const frames = ["◒", "◐", "◓", "◑"];
  const animated = Boolean(output.isTTY);
  let timer: NodeJS.Timeout | undefined;
  let text = "";
  let frame = 0;
  const render = (): void => {
    write(`\r\x1b[2K${color.magenta(frames[frame = (frame + 1) % frames.length]!)}  ${text}`);
  };
  return {
    start(message: string): void {
      text = message;
      if (!animated) { write(`${message}…\n`); return; }
      write(`${color.gray(symbol.bar)}\n\x1b[?25l`);
      render();
      timer = setInterval(render, 90);
    },
    message(message: string): void { text = message; if (!animated) write(`${message}…\n`); },
    stop(message: string, failed = false): void {
      if (timer !== undefined) clearInterval(timer);
      timer = undefined;
      if (!animated) { write(`${message}\n`); return; }
      write(`\r\x1b[2K${failed ? color.red(symbol.error) : color.green(symbol.done)}  ${message}\n\x1b[?25h`);
    },
  };
}
