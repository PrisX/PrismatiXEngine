import {convertWebShader} from "../packages/cli/dist/src/web-shader.js";
import {writeFile} from "node:fs/promises";
const [input, output, cross, naga] = process.argv.slice(2);
if (![input, output, cross, naga].every(Boolean)) throw new Error("Usage: convert-web-shader.mjs <input.spv> <output-stem> <spirv-cross> <naga>");
const shader = await convertWebShader(input, output, cross, naga);
await writeFile(`${output}.json`, JSON.stringify(shader));
