import {spawn} from "node:child_process";
import {fileURLToPath} from "node:url";

const packageRoot = fileURLToPath(new URL("../../", import.meta.url));
const fakeTty = fileURLToPath(new URL("../../test/fixtures/fake-tty.mjs", import.meta.url));

export const keys = {enter: "\r", down: "\x1b[B", up: "\x1b[A", escape: "\x1b", ctrlC: "\x03", backspace: "\x7f"};

/**
 * Whether `output` (everything written since the last keystroke) ends with a
 * prompt that is listening: a prompt draws its active (◆) or error (▲) frame
 * only after it has attached its key listener, and an answered (◇) or
 * cancelled (■) frame means the next prompt has not drawn yet.
 */
function promptListening(output: string): boolean {
  const listening = Math.max(output.lastIndexOf("◆"), output.lastIndexOf("▲"));
  return listening >= 0 && listening > Math.max(output.lastIndexOf("◇"), output.lastIndexOf("■"));
}

/**
 * Runs the CLI as if in a terminal and types `steps` one at a time. Each step
 * waits until the CLI has reacted to the previous one and a prompt is
 * listening again, so keys never reach the CLI between prompts (where they
 * would be dropped) however slowly a loaded machine draws.
 */
export async function driveCli(args: readonly string[], steps: readonly string[], options: {
  readonly cwd?: string; readonly env?: NodeJS.ProcessEnv; readonly settleMs?: number; readonly stepTimeoutMs?: number;
} = {}): Promise<{code: number | null; stdout: string; stderr: string}> {
  const child = spawn(process.execPath, ["--import", `file:///${fakeTty.replaceAll("\\", "/")}`,
    `${packageRoot}dist/src/cli.js`, ...args], {
    cwd: options.cwd ?? packageRoot, env: {...process.env, FORCE_COLOR: "0", LANG: "en_US.UTF-8", ...options.env},
    stdio: ["pipe", "pipe", "pipe"], windowsHide: true,
  });
  let stdout = "";
  let stderr = "";
  let sinceKeystroke = "";
  let lastOutput = Date.now();
  let closed = false;
  child.stdout.setEncoding("utf8").on("data", (chunk: string) => { stdout += chunk; lastOutput = Date.now(); });
  child.stderr.setEncoding("utf8").on("data", (chunk: string) => {
    stderr += chunk; sinceKeystroke += chunk; lastOutput = Date.now();
  });
  const exited = new Promise<number | null>((accept) => child.once("close", (code) => { closed = true; accept(code); }));
  const settle = options.settleMs ?? 100;
  // Creating a project installs its tools before the last prompt; allow for a slow machine.
  const stepTimeout = options.stepTimeoutMs ?? 240_000;
  for (const [index, step] of steps.entries()) {
    const started = Date.now();
    while (!promptListening(sinceKeystroke) || Date.now() - lastOutput < settle) {
      if (closed) break;
      if (Date.now() - started > stepTimeout) {
        child.kill();
        throw new Error(`No prompt was listening for step ${index + 1} (${JSON.stringify(step)}) after ` +
          `${stepTimeout} ms; screen so far:\n${finalScreen(stderr)}`);
      }
      await new Promise((accept) => setTimeout(accept, 25));
    }
    if (closed) break;
    sinceKeystroke = "";
    child.stdin.write(step);
    lastOutput = Date.now();
  }
  let timer: NodeJS.Timeout | undefined;
  const code = await Promise.race([exited, new Promise<null>((accept) => {
    timer = setTimeout(() => { child.kill(); accept(null); }, 300_000);
  })]);
  clearTimeout(timer);
  return {code, stdout, stderr};
}

/** The screen a terminal would show: applies the cursor-up/clear redraws of the prompts. */
export function finalScreen(transcript: string): string {
  const lines: string[] = [""];
  let row = 0;
  let column = 0;
  const pattern = /\x1b\[(\d*)([A-Za-z])|\x1b\[\?25[hl]|\r|\n|[^\x1b\r\n]+/gu;
  for (const match of transcript.matchAll(pattern)) {
    const token = match[0];
    if (token === "\n") { row += 1; column = 0; lines[row] ??= ""; continue; }
    if (token === "\r") { column = 0; continue; }
    if (token.startsWith("\x1b[?")) continue;
    if (match[2] !== undefined) {
      const count = Number(match[1] || "1");
      if (match[2] === "A") row = Math.max(0, row - count);
      else if (match[2] === "J") { lines[row] = (lines[row] ?? "").slice(0, column); lines.length = row + 1; }
      else if (match[2] === "K") lines[row] = (lines[row] ?? "").slice(0, column);
      continue;
    }
    const line = lines[row] ?? "";
    lines[row] = line.slice(0, column) + token + line.slice(column + token.length);
    column += token.length;
  }
  return lines.join("\n");
}
