import {spawn} from "node:child_process";
import {fileURLToPath} from "node:url";

const packageRoot = fileURLToPath(new URL("../../", import.meta.url));
const fakeTty = fileURLToPath(new URL("../../test/fixtures/fake-tty.mjs", import.meta.url));

export const keys = {enter: "\r", down: "\x1b[B", up: "\x1b[A", escape: "\x1b", ctrlC: "\x03", backspace: "\x7f"};

/**
 * Runs the CLI as if in a terminal and types `steps` one at a time, each after
 * the screen has been quiet for a moment (the previous prompt has drawn).
 */
export async function driveCli(args: readonly string[], steps: readonly string[], options: {
  readonly cwd?: string; readonly env?: NodeJS.ProcessEnv; readonly settleMs?: number;
} = {}): Promise<{code: number | null; stdout: string; stderr: string}> {
  const child = spawn(process.execPath, ["--import", `file:///${fakeTty.replaceAll("\\", "/")}`,
    `${packageRoot}dist/src/cli.js`, ...args], {
    cwd: options.cwd ?? packageRoot, env: {...process.env, FORCE_COLOR: "0", LANG: "en_US.UTF-8", ...options.env},
    stdio: ["pipe", "pipe", "pipe"], windowsHide: true,
  });
  let stdout = "";
  let stderr = "";
  let lastOutput = Date.now();
  child.stdout.setEncoding("utf8").on("data", (chunk: string) => { stdout += chunk; lastOutput = Date.now(); });
  child.stderr.setEncoding("utf8").on("data", (chunk: string) => { stderr += chunk; lastOutput = Date.now(); });
  const exited = new Promise<number | null>((accept) => child.once("close", accept));
  const settle = options.settleMs ?? 250;
  for (const step of steps) {
    // Wait for the prompt to appear and the screen to settle before typing.
    const started = Date.now();
    while (Date.now() - lastOutput < settle || stderr.length === 0) {
      if (Date.now() - started > 30_000) break;
      await new Promise((accept) => setTimeout(accept, 25));
    }
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
