import {spawn} from "node:child_process";
import {readFile, writeFile, mkdir, rm} from "node:fs/promises";
import {dirname} from "node:path";

// SDL separates native texture and sampler namespaces. WebGPU shares them.
export function webSpirv(bytes: Buffer): Buffer {
  if (bytes.length < 20 || bytes.length % 4 || bytes.readUInt32LE(0) !== 0x07230203)
    throw new Error("Invalid SPIR-V module");
  const result = Buffer.from(bytes), types = new Set<number>(), pointers = new Set<number>(), samplers = new Set<number>();
  const instructions: {offset: number; count: number; op: number}[] = [];
  for (let offset = 20; offset < bytes.length;) {
    const instruction = bytes.readUInt32LE(offset), count = instruction >>> 16, op = instruction & 65535;
    if (!count || offset + count * 4 > bytes.length) throw new Error("Invalid SPIR-V instruction");
    instructions.push({offset, count, op}); offset += count * 4;
  }
  for (const {offset, count, op} of instructions) if (op === 26 && count === 2) types.add(bytes.readUInt32LE(offset + 4));
  for (const {offset, count, op} of instructions)
    if (op === 32 && count === 4 && types.has(bytes.readUInt32LE(offset + 12))) pointers.add(bytes.readUInt32LE(offset + 4));
  for (const {offset, count, op} of instructions)
    if (op === 59 && count >= 4 && pointers.has(bytes.readUInt32LE(offset + 4))) samplers.add(bytes.readUInt32LE(offset + 8));
  for (const {offset, count, op} of instructions)
    if (op === 71 && count >= 4 && samplers.has(bytes.readUInt32LE(offset + 4)) && bytes.readUInt32LE(offset + 8) === 33)
      result.writeUInt32LE(bytes.readUInt32LE(offset + 12) + 16, offset + 12);
  return result;
}
async function run(executable: string, args: string[]): Promise<string> {
  return new Promise((accept, reject) => {
    const child = spawn(executable, args, {windowsHide: true, stdio: ["ignore", "pipe", "pipe"]});
    let output = "", errors = "";
    child.stdout.on("data", data => { output += data; });
    child.stderr.on("data", data => { errors = (errors + data).slice(-8192); });
    child.on("error", reject);
    child.on("close", code => code === 0 ? accept(output) : reject(new Error(`Shader conversion failed (${code}): ${errors}`)));
  });
}
export interface WebShader {
  glsl: string; wgsl: string; uniformSize: number;
  inputs: {name: string; location: number; type: string}[];
  textures: {name: string; binding: number}[];
  samplers: {name: string; binding: number}[];
}
/**
 * Rebinds a translated fragment shader for SDL_GPU's WebGPU backend (the
 * WebGPU Web Player). The WGSL above has texture i at @group(2) @binding(i)
 * and its sampler at i + 16 (see webSpirv); the backend expects texture i at
 * binding 2i and its sampler at 2i + 1. Trailing spaces are trimmed so the
 * output is stable text.
 */
export function sdlGpuWgsl(wgsl: string): string {
  return wgsl.replace(/@group\(2\)\s*@binding\((\d+)\)(\s*var\s+\w+\s*:\s*(\w+))/gu, (_, binding: string, declaration: string, type: string) => {
    const slot = Number(binding);
    const sampler = type === "sampler" || type === "sampler_comparison";
    if (sampler ? slot < 16 : slot >= 16) throw new Error(`Unexpected ${sampler ? "sampler" : "texture"} binding ${slot}`);
    return `@group(2) @binding(${sampler ? (slot - 16) * 2 + 1 : slot * 2})${declaration}`;
  }).replace(/[ \t]+$/gmu, "");
}
export async function convertWebShader(input: string, outputStem: string, spirvCross: string, naga: string): Promise<WebShader> {
  await mkdir(dirname(outputStem), {recursive: true});
  const bytes = await readFile(input);
  const reflection = JSON.parse(await run(spirvCross, [input, "--reflect"])) as {
    inputs: WebShader["inputs"]; separate_images?: WebShader["textures"]; separate_samplers?: WebShader["samplers"];
    ubos: {set: number; binding: number; block_size: number}[];
  };
  if (reflection.ubos.length !== 1 || reflection.ubos[0]!.set !== 3 || reflection.ubos[0]!.binding !== 0)
    throw new Error("Web effects require the standard fragment uniform buffer");
  await run(spirvCross, [input, "--es", "--version", "300", "--output", `${outputStem}.glsl`]);
  const temporary = `${outputStem}.web.spv`;
  try {
    await writeFile(temporary, webSpirv(bytes));
    await run(naga, [temporary, `${outputStem}.wgsl`]);
  } finally { await rm(temporary, {force: true}); }
  return {glsl: await readFile(`${outputStem}.glsl`, "utf8"), wgsl: await readFile(`${outputStem}.wgsl`, "utf8"),
    inputs: reflection.inputs, textures: reflection.separate_images ?? [], samplers: reflection.separate_samplers ?? [],
    uniformSize: reflection.ubos[0]!.block_size};
}
