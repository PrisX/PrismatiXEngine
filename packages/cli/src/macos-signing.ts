// Signing a packed macOS .app. Apple silicon runs nothing unsigned, and a
// bundle whose files changed after its executable was signed reads as
// "damaged", so every packed .app is at least signed ad hoc:
//
//   on a Mac          codesign --sign - (Xcode Command Line Tools)
//   anywhere else     rcodesign sign (apple-codesign), when installed
//
// With a Developer ID Application certificate (PRISMATIX_MACOS_P12 and
// PRISMATIX_MACOS_P12_PASSWORD) rcodesign signs with the hardened runtime on
// any host, and with an App Store Connect API key (PRISMATIX_MACOS_NOTARY_KEY)
// it also notarizes and staples the ticket. rcodesign is never downloaded:
// PRISMATIX_RCODESIGN names it, else it is looked up on PATH.

import {access, chmod, constants, mkdtemp, rm, writeFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {delimiter, join} from "node:path";

export type Run = (command: string, args: readonly string[]) => Promise<{code: number; stdout: string; stderr: string}>;

export interface MacSigningCredentials {
  readonly p12?: string;
  readonly p12Password?: string;
  readonly notaryKey?: string;
}

export function macSigningCredentials(environment: NodeJS.ProcessEnv = process.env): MacSigningCredentials {
  const value = (name: string) => environment[name] === undefined || environment[name] === "" ? undefined : environment[name];
  const p12 = value("PRISMATIX_MACOS_P12");
  const p12Password = value("PRISMATIX_MACOS_P12_PASSWORD");
  const notaryKey = value("PRISMATIX_MACOS_NOTARY_KEY");
  return {...(p12 === undefined ? {} : {p12}), ...(p12Password === undefined ? {} : {p12Password}),
    ...(notaryKey === undefined ? {} : {notaryKey})};
}

/** rcodesign from PRISMATIX_RCODESIGN or PATH; undefined when neither has it. */
export async function findRcodesign(environment: NodeJS.ProcessEnv = process.env,
  platform: NodeJS.Platform = process.platform): Promise<string | undefined> {
  const executable = async (path: string) => {
    try { await access(path, platform === "win32" ? constants.F_OK : constants.X_OK); return true; }
    catch { return false; }
  };
  const explicit = environment.PRISMATIX_RCODESIGN;
  if (explicit) {
    if (await executable(explicit)) return explicit;
    throw new Error(`PRISMATIX_RCODESIGN names ${explicit}, which is not an executable file`);
  }
  const names = platform === "win32"
    ? (environment.PATHEXT ?? ".EXE;.CMD;.BAT").split(";").filter(Boolean).map(extension => `rcodesign${extension.toLowerCase()}`)
    : ["rcodesign"];
  for (const directory of (environment.PATH ?? environment.Path ?? "").split(platform === "win32" ? ";" : delimiter)) {
    if (!directory) continue;
    for (const name of names) if (await executable(join(directory, name))) return join(directory, name);
  }
  return undefined;
}

export interface MacSigningOutcome {
  /** "developer-id" (hardened runtime), "adhoc", or "none" when no signing tool was available. */
  readonly signature: "developer-id" | "adhoc" | "none";
  readonly notarized: boolean;
  readonly tool?: "codesign" | "rcodesign";
  readonly warnings: readonly string[];
}

export const rcodesignInstallHint =
  "install rcodesign (apple-codesign: `cargo install apple-codesign`, or a release binary from " +
  "https://github.com/indygreg/apple-platform-rs/releases) and put it on PATH or in PRISMATIX_RCODESIGN";

/**
 * Signs `app` with rcodesign: with the Developer ID certificate when the
 * credentials name one (then notarizing when they name an API key), else ad
 * hoc. `macHost` keeps ad hoc signing on a Mac for `codesign`, which the
 * caller runs. The certificate password goes through a private temporary
 * file, never the command line.
 */
export async function signMacAppBundle(app: string, run: Run, options: {
  readonly environment?: NodeJS.ProcessEnv;
  readonly macHost?: boolean;
  readonly onProgress?: (message: string) => void;
  /** How messages name the app (its published place rather than a staging folder). */
  readonly label?: string;
  /** Whether the Player inside already carries a valid ad hoc signature (the linker's). */
  readonly playerSigned?: boolean;
} = {}): Promise<MacSigningOutcome | undefined> {
  const environment = options.environment ?? process.env;
  const label = options.label ?? app;
  const credentials = macSigningCredentials(environment);
  if (credentials.p12 === undefined) {
    if (credentials.notaryKey !== undefined)
      throw new Error("PRISMATIX_MACOS_NOTARY_KEY needs a Developer ID certificate in PRISMATIX_MACOS_P12; " +
        "Apple notarizes only Developer ID signed apps");
    if (options.macHost === true) return undefined;
    const rcodesign = await findRcodesign(environment);
    if (rcodesign === undefined)
      return {signature: "none", notarized: false, warnings: [
        `${label} is not signed as a bundle` +
        (options.playerSigned === true ? " (its Player keeps the linker's ad hoc signature, so it can run)" : "") +
        ": players who download it see it refused as damaged or from an unidentified developer until they " +
        `clear the quarantine. To sign it ad hoc from this computer, ${rcodesignInstallHint}; or sign it on a Mac ` +
        "(codesign --force --deep --sign - <app>). docs/shipping.md, Packing for other platforms, explains both."]};
    options.onProgress?.(`Signing ${label} ad hoc with rcodesign`);
    const signed = await run(rcodesign, ["sign", app]);
    if (signed.code !== 0) throw new Error(`rcodesign sign ${label} failed: ${(signed.stderr || signed.stdout).trim()}`);
    return {signature: "adhoc", notarized: false, tool: "rcodesign", warnings: []};
  }
  if (credentials.p12Password === undefined)
    throw new Error("PRISMATIX_MACOS_P12 is set but PRISMATIX_MACOS_P12_PASSWORD is not");
  const rcodesign = await findRcodesign(environment);
  if (rcodesign === undefined) throw new Error(`Developer ID signing uses rcodesign: ${rcodesignInstallHint}`);
  const secrets = await mkdtemp(join(tmpdir(), "prismatix-sign-"));
  try {
    const password = join(secrets, "p12-password");
    await writeFile(password, credentials.p12Password, {mode: 0o600, flag: "wx"});
    await chmod(password, 0o600);
    options.onProgress?.(`Signing ${label} with the Developer ID certificate (hardened runtime)`);
    const signed = await run(rcodesign, ["sign", "--p12-file", credentials.p12, "--p12-password-file", password,
      "--code-signature-flags", "runtime", app]);
    if (signed.code !== 0) throw new Error(`rcodesign sign ${label} failed: ${(signed.stderr || signed.stdout).trim()}`);
  } finally {
    await rm(secrets, {recursive: true, force: true});
  }
  if (credentials.notaryKey === undefined)
    return {signature: "developer-id", notarized: false, tool: "rcodesign", warnings: [
      `${label} is signed with your Developer ID but not notarized; set PRISMATIX_MACOS_NOTARY_KEY to notarize it, ` +
      "or players see a warning the first time they open it."]};
  options.onProgress?.(`Notarizing ${label} with Apple (this waits for Apple's answer, usually a few minutes)`);
  const notarized = await run(rcodesign, ["notary-submit", "--api-key-file", credentials.notaryKey, "--staple", app]);
  if (notarized.code !== 0)
    throw new Error(`rcodesign notary-submit ${label} failed: ${(notarized.stderr || notarized.stdout).trim()}`);
  return {signature: "developer-id", notarized: true, tool: "rcodesign", warnings: []};
}
