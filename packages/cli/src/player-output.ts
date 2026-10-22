// The Player's console output while `prismatix run` plays a game. Bundled
// extension positions (`Content/Extensions/x/main.js:12:5`) are rewritten to
// the authored TypeScript lines; the extension's own log lines (errors,
// warnings, `ctx.log` and console output: `[javascript] ...`) also become
// records, so an editor can show them at their source.

export type ExtensionLogSeverity = "error" | "warning" | "information";

export interface ExtensionLogRecord {
  readonly severity: ExtensionLogSeverity;
  /** The first line of the message. */
  readonly message: string;
  /** The rest (a stack, with remapped positions), if any. */
  readonly details?: string;
  /** The first authored source position in the message or its stack. */
  readonly path?: string;
  readonly line?: number;
  readonly column?: number;
}

// spdlog's console pattern: `[%H:%M:%S.%e] [%l] [%t] %v`.
const logLine = /^\[\d{2}:\d{2}:\d{2}\.\d{3}\] \[(\w+)\] \[[^\]]*\] (.*)$/u;
const javascriptPrefix = "[javascript] ";
const ansi = /\u001b\[[0-9;]*m/gu;
const position = /([\w./@-]+\.(?:ts|tsx|mts|cts|js|mjs)):(\d+):(\d+)/u;

function severityOf(level: string): ExtensionLogSeverity {
  if (level === "error" || level === "critical" || level === "err") return "error";
  if (level === "warning" || level === "warn") return "warning";
  return "information";
}

/** The record for one `[javascript]` message (its first line plus continuation lines). */
export function extensionLogRecord(severity: ExtensionLogSeverity, text: string): ExtensionLogRecord {
  const lines = text.split("\n");
  const message = lines[0]!.trim();
  const details = lines.slice(1).map((line) => line.trimEnd()).filter((line) => line.trim() !== "").join("\n");
  // Prefer an authored TypeScript frame; a bundle frame means no source map.
  const candidates = [...text.matchAll(new RegExp(position.source, "gu"))]
    .filter((match) => !match[1]!.startsWith("<"));
  const located = candidates.find((match) => !/\.m?js$/u.test(match[1]!)) ?? candidates[0];
  return {severity, message, ...(details.length > 0 ? {details} : {}),
    ...(located === undefined ? {} : {path: located[1]!, line: Number(located[2]), column: Number(located[3])})};
}

/**
 * Splits the Player's output into lines, remaps each (for a terminal) and
 * gathers `[javascript]` messages with their continuation lines into records.
 * `flush` ends the message being gathered (call it when output pauses).
 */
export class PlayerOutput {
  private partial = "";
  private pending: {severity: ExtensionLogSeverity; lines: string[]} | undefined;

  constructor(private readonly remap: (text: string) => string,
    private readonly onLine: (line: string) => void,
    private readonly onRecord: (record: ExtensionLogRecord) => void) {}

  push(chunk: string): void {
    const text = this.partial + chunk;
    const lines = text.split(/\r?\n/u);
    this.partial = lines.pop() ?? "";
    for (const line of lines) this.line(line);
  }

  /** Ends the output: the last partial line and the gathered message. */
  end(): void {
    if (this.partial.length > 0) this.line(this.partial);
    this.partial = "";
    this.flush();
  }

  flush(): void {
    const pending = this.pending;
    this.pending = undefined;
    if (pending !== undefined) this.onRecord(extensionLogRecord(pending.severity, this.remap(pending.lines.join("\n"))));
  }

  private line(raw: string): void {
    const plain = raw.replace(ansi, "");
    this.onLine(this.remap(raw));
    const match = logLine.exec(plain);
    if (match === null) {
      if (this.pending !== undefined && plain.trim() !== "") this.pending.lines.push(plain);
      return;
    }
    this.flush();
    if (match[2]!.startsWith(javascriptPrefix))
      this.pending = {severity: severityOf(match[1]!.toLowerCase()), lines: [match[2]!.slice(javascriptPrefix.length)]};
  }
}
