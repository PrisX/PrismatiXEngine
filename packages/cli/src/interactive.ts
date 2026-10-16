// The interactive front of the CLI: the `create` wizard and the menu shown by
// `prismatix` without a command. Both only ask questions; cli.ts runs the
// chosen command, so every answer maps to an ordinary command line.
import {readFileSync} from "node:fs";
import {readdir, readFile, stat} from "node:fs/promises";
import {basename, join, relative, resolve} from "node:path";
import {builtinRoutes} from "./eject.js";
import type {PackageManager} from "./starter.js";
import {CANCEL, type Cancel, color, isCancel, log, select, text} from "./tui.js";

export const cliVersion: string = (JSON.parse(readFileSync(new URL("../../package.json", import.meta.url), "utf8")) as
  {version: string}).version;

const chinese = /^zh/iu.test(process.env.LC_ALL || process.env.LANG?.replace(/[._].*$/u, "") ||
  Intl.DateTimeFormat().resolvedOptions().locale);
/** Picks the Traditional Chinese or English wording for the user's system language. */
export const say = (zh: string, en: string): string => chinese ? zh : en;

const exists = (path: string): Promise<boolean> => stat(path).then(() => true, () => false);

async function usableTarget(path: string): Promise<string | undefined> {
  try {
    const info = await stat(path);
    if (!info.isDirectory()) return say("這個位置已經有檔案了", "A file already exists there");
    if ((await readdir(path)).length > 0) return say("資料夾不是空的，換個名字吧", "That folder is not empty; pick another name");
  } catch { /* A new folder. */ }
  return undefined;
}

/** A folder name from the title: ASCII titles keep their words, others get my-game. */
function folderFrom(title: string): string {
  const slug = title.trim().toLowerCase().replace(/[^a-z0-9._-]+/gu, "-").replace(/^[-.]+|-+$/gu, "");
  return slug.length >= 2 ? slug.slice(0, 64) : "my-game";
}

export interface CreateAnswers {
  readonly target: string;
  readonly name: string;
  readonly template: "minimal" | "adv";
  readonly width: number;
  readonly height: number;
  readonly locale: string;
  readonly packageManager: PackageManager;
}

/** Asks for everything `create` needs. Values already given as options are not asked again. */
export async function createWizard(given: {
  readonly name?: string; readonly locale?: string; readonly template?: string;
  readonly width?: number; readonly height?: number; readonly packageManager?: PackageManager;
  readonly detected: PackageManager; readonly pnpmFound: boolean;
}): Promise<CreateAnswers | Cancel> {
  const name = given.name ?? await text({
    message: say("遊戲叫什麼名字？", "What is your game called?"),
    defaultValue: say("我的視覺小說", "My Visual Novel"),
    validate: (value) => value.trim().length === 0 || value.length > 512
      ? say("請輸入 1–512 個字", "Use 1–512 characters") : undefined,
  });
  if (isCancel(name)) return CANCEL;

  const folder = await text({
    message: say("要建立在哪個資料夾？", "Where should the project go?"),
    defaultValue: folderFrom(name),
    validate: (value) => value.trim().length === 0 ? say("請輸入資料夾名稱", "Enter a folder name") : undefined,
  });
  if (isCancel(folder)) return CANCEL;
  const target = resolve(folder.trim());
  const problem = await usableTarget(target);
  if (problem !== undefined) { log.error(`${problem}${say("：", ": ")}${target}`); return CANCEL; }

  const template = given.template === "minimal" || given.template === "adv" ? given.template : await select({
    message: say("從哪個範本開始？", "Which template?"),
    options: [
      {value: "minimal" as const, label: say("基本", "Minimal"),
        hint: say("標題畫面＋內建的對話框、存讀檔、設定", "title screen + built-in dialogue, save/load, settings")},
      {value: "adv" as const, label: "ADV",
        hint: say("另外複製 HUD、存讀檔、設定、回憶錄的 TSX 讓你改", "also editable HUD, save/load, settings, backlog TSX")},
    ],
  });
  if (isCancel(template)) return CANCEL;

  let width = given.width;
  let height = given.height;
  if (width === undefined || height === undefined) {
    const resolution = await select({
      message: say("解析度", "Resolution"),
      options: [
        {value: "1280x720", label: "1280 × 720", hint: "HD"},
        {value: "1920x1080", label: "1920 × 1080", hint: "Full HD"},
        {value: "custom", label: say("自訂…", "Custom…")},
      ],
    });
    if (isCancel(resolution)) return CANCEL;
    const size = resolution !== "custom" ? resolution : await text({
      message: say("寬 × 高（像素）", "Width × height in pixels"),
      defaultValue: "1600x900",
      validate: (value) => {
        const match = /^\s*(\d+)\s*[x×*,]\s*(\d+)\s*$/iu.exec(value);
        return match && [match[1], match[2]].every((part) => Number(part) >= 1 && Number(part) <= 16384)
          ? undefined : say("例如 1600x900（1–16384）", "For example 1600x900 (1–16384)");
      },
    });
    if (isCancel(size)) return CANCEL;
    const [, w, h] = /(\d+)\s*[x×*,]\s*(\d+)/iu.exec(size)!;
    width = Number(w); height = Number(h);
  }

  let locale = given.locale;
  if (locale === undefined) {
    const choice = await select({
      message: say("劇本的語言", "Story language"),
      options: [
        {value: "zh-TW", label: "繁體中文", hint: "zh-TW"},
        {value: "ja", label: "日本語", hint: "ja"},
        {value: "en", label: "English", hint: "en"},
        {value: "zh-CN", label: "简体中文", hint: "zh-CN"},
        {value: "other", label: say("其他…", "Other…")},
      ],
      initialValue: chinese ? "zh-TW" : "en",
    });
    if (isCancel(choice)) return CANCEL;
    const tag = choice !== "other" ? choice : await text({
      message: say("語言代碼（BCP 47，例如 ko、fr、zh-HK）", "Language tag (BCP 47, such as ko, fr, zh-HK)"),
      validate: (value) => /^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$/u.test(value.trim())
        ? undefined : say("不是有效的語言代碼", "Not a valid language tag"),
    });
    if (isCancel(tag)) return CANCEL;
    locale = tag.trim();
  }

  let packageManager = given.packageManager;
  if (packageManager === undefined) {
    if (!given.pnpmFound) packageManager = "npm";
    else {
      const manager = await select({
        message: say("用哪個套件管理器安裝工具？", "Install the project's tools with"),
        options: [
          {value: "npm" as const, label: "npm", hint: given.detected === "npm" ? say("目前使用", "current") : undefined},
          {value: "pnpm" as const, label: "pnpm", hint: given.detected === "pnpm" ? say("目前使用", "current") : undefined},
        ],
        initialValue: given.detected,
      });
      if (isCancel(manager)) return CANCEL;
      packageManager = manager;
    }
  }
  return {target, name, template, width, height, locale, packageManager};
}

/** How the user runs a project script: `npm run dev` or `pnpm dev`. */
export function runScript(manager: PackageManager, script: string): string {
  return manager === "pnpm" ? `pnpm ${script}` : `npm run ${script}`;
}

/** Folder to `cd` into from the current directory, quoted when it has spaces. */
export function changeDirectory(target: string): string {
  const path = relative(process.cwd(), target) || ".";
  return /\s/u.test(path) ? `cd "${path}"` : `cd ${path}`;
}

interface ProjectSummary { readonly root: string; readonly name: string; readonly hasJourneys: boolean }

async function currentProject(): Promise<ProjectSummary | undefined> {
  const root = process.cwd();
  for (const manifest of ["prismatix.json", "project.pxproject"]) {
    const path = join(root, manifest);
    if (!await exists(path)) continue;
    let name = basename(root);
    try {
      const parsed = JSON.parse(await readFile(path, "utf8")) as {name?: unknown};
      if (typeof parsed.name === "string" && parsed.name.trim()) name = parsed.name;
    } catch { /* The command reports a broken manifest itself. */ }
    return {root, name, hasJourneys: await exists(join(root, "tests", "journeys"))};
  }
  return undefined;
}

/**
 * The menu for `prismatix` without a command. Returns the command line to
 * run, so a choice behaves exactly like typing that command.
 */
export async function projectMenu(): Promise<readonly string[] | Cancel> {
  const project = await currentProject();
  if (project === undefined) {
    log.info(say("這裡不是 PrismatiX 專案。", "This folder is not a PrismatiX project."));
    const choice = await select({
      message: say("要做什麼？", "What would you like to do?"),
      options: [
        {value: "create", label: say("建立新遊戲", "Create a new game"), hint: "prismatix create"},
        {value: "help", label: say("查看所有指令", "Show all commands"), hint: "prismatix --help"},
      ],
    });
    if (isCancel(choice)) return CANCEL;
    return choice === "create" ? ["create"] : ["--help"];
  }
  log.info(`${color.bold(project.name)}  ${color.dim(project.root)}`);
  const choice = await select({
    message: say("要做什麼？", "What would you like to do?"),
    options: [
      {value: "dev", label: say("開啟預覽", "Open the live preview"), hint: "prismatix dev"},
      {value: "validate", label: say("檢查專案", "Check the project"), hint: "prismatix validate"},
      {value: "run", label: say("建置並遊玩", "Build and play"), hint: "prismatix run"},
      {value: "build", label: say("建置可發行版本", "Build a distribution"), hint: "prismatix build"},
      ...project.hasJourneys
        ? [{value: "test", label: say("執行自動測試", "Run test journeys"), hint: "prismatix test"}] : [],
      {value: "character", label: say("新增角色", "Add a character"), hint: "prismatix add character"},
      {value: "scene", label: say("新增場景", "Add a scene"), hint: "prismatix add scene"},
      {value: "eject", label: say("複製內建畫面來修改", "Customize a built-in screen"), hint: "prismatix eject ui"},
      {value: "help", label: say("查看所有指令", "Show all commands"), hint: "prismatix --help"},
    ],
  });
  if (isCancel(choice)) return CANCEL;
  switch (choice) {
    case "dev": case "validate": case "run": case "build": return [choice, "."];
    case "test": return ["test"];
    case "help": return ["--help"];
    case "character": {
      const alias = await text({
        message: say("角色代號（劇本裡用 @代號 說話）", "Character alias (speaks as @alias in the story)"),
        validate: (value) => /^[A-Za-z_][A-Za-z0-9_-]{0,63}$/u.test(value.trim())
          ? undefined : say("請用英文字母、數字、_ 或 -，例如 hikari", "Use letters, digits, _ or -, such as hikari"),
      });
      if (isCancel(alias)) return CANCEL;
      const display = await text({message: say("顯示名稱", "Display name"), defaultValue: alias.trim()});
      if (isCancel(display)) return CANCEL;
      const folder = await text({
        message: say("立繪資料夾（每張圖一個表情；留空就先不放圖）",
          "Folder of expression images (leave empty for no sprite yet)"),
      });
      if (isCancel(folder)) return CANCEL;
      if (folder.trim() !== "" && !await exists(resolve(folder.trim()))) {
        log.error(say(`找不到資料夾：${resolve(folder.trim())}`, `No such folder: ${resolve(folder.trim())}`));
        return CANCEL;
      }
      return ["add", "character", alias.trim(), "--name", display.trim(),
        ...folder.trim() === "" ? [] : ["--from", folder.trim()]];
    }
    case "scene": {
      const id = await text({
        message: say("場景代號", "Scene id"),
        validate: (value) => /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/u.test(value.trim())
          ? undefined : say("請用英文字母、數字、_ 或 -，例如 chapter2", "Use letters, digits, _ or -, such as chapter2"),
      });
      if (isCancel(id)) return CANCEL;
      const title = await text({message: say("場景標題（存檔裡顯示）", "Scene title (shown in saves)"), defaultValue: id.trim()});
      if (isCancel(title)) return CANCEL;
      return ["add", "scene", id.trim(), "--title", title.trim()];
    }
    case "eject": {
      const route = await select({
        message: say("要修改哪個畫面？", "Which screen?"),
        options: [...builtinRoutes.map((value) => ({value: value as string, label: value})),
          {value: "all", label: say("全部", "All of them")}],
      });
      if (isCancel(route)) return CANCEL;
      return ["eject", "ui", route];
    }
    default: return CANCEL;
  }
}
