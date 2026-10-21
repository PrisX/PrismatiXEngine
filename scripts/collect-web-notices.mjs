// Copies the original license texts of the Web Player's browser runtime
// dependencies, found through a Web Player build's CMakeCache.txt.
//
// The renderer-independent dependencies go to <destination>/Notices. SDL
// (which differs between the WebGL2 and WebGPU builds) and, for the WebGPU
// build, Dawn's emdawnwebgpu bindings go to <renderer destination>/Notices.
// Used by scripts/build-wasm-player.mjs.
import {readFile,writeFile,mkdir,copyFile,readdir} from 'node:fs/promises';
import {join,resolve} from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';

const root=resolve(fileURLToPath(new URL('..',import.meta.url)));

async function buildCache(build){
  return new Map((await readFile(join(build,'CMakeCache.txt'),'utf8')).split(/\r?\n/u)
    .flatMap(line=>{const match=/^([^#/:][^:=]*):[^=]*=(.*)$/u.exec(line);return match?[[match[1],match[2]]]:[]}));
}
async function copyNotices(destination,components){
  const files=[];
  for(const [name,directory,licenses] of components){
    await mkdir(join(destination,'Notices',name),{recursive:true});
    for(const license of licenses){
      const output=`Notices/${name}/${license.replaceAll('/','-')}`;
      await copyFile(join(directory,license),join(destination,output));files.push(output);
    }
  }
  return files;
}

/** Notices shared by both renderers; returns paths relative to destination. */
export async function collectSharedNotices(build,destination){
  build=resolve(build);destination=resolve(destination);
  const cache=await buildCache(build);
  const source=name=>{
    const directory=cache.get(`${name}_SOURCE_DIR`);
    if(!directory)throw new Error(`Missing ${name} dependency source in CMakeCache.txt`);
    return directory;
  };
  const files=await copyNotices(destination,[
    ['SDL_image',source('SDL3_image'),['LICENSE.txt']],
    ['SDL_ttf',source('SDL3_ttf'),['LICENSE.txt']],
    ['SDL_mixer',source('SDL3_mixer'),['LICENSE.txt']],
    ['FreeType',source('freetype'),['docs/FTL.TXT']],
    ['HarfBuzz',source('harfbuzz'),['COPYING']],
    ['libpng',source('libpng'),['LICENSE']],
    ['zlib',source('zlib'),['LICENSE']],
    ['IJG-JPEG',join(source('SDL3_image'),'external/jpeg'),['COPYING']],
    ['WebP',source('WebP'),['COPYING','PATENTS']],
    ['Opus',source('Opus'),['COPYING']],
    ['OpusFile',source('OpusFile'),['COPYING']],
    ['Ogg',source('libogg'),['COPYING']],
    ['dr-libs',join(source('SDL3_mixer'),'src/dr_libs'),['LICENSE']],
    ['stb-vorbis',join(source('SDL3_mixer'),'src/stb_vorbis'),['stb_vorbis.h']],
    ['QuickJS-NG',source('quickjs'),['LICENSE']],
    ['nlohmann-json',source('nlohmann_json'),['LICENSE.MIT']],
    ['spdlog',source('spdlog'),['LICENSE']],
    ['utf8proc',source('utf8proc'),['LICENSE.md']],
    ['Mbed-TLS',source('Mbed TLS'),['LICENSE']],
    ['Zstandard',resolve(source('zstd'),'../..'),['LICENSE']],
    ['libunibreak',join(build,'_deps/prismatix_unibreak_source-src'),['LICENCE']],
    ['FriBidi',join(build,'wasm-deps/fribidi-build/src/prismatix_fribidi_external'),['COPYING']],
  ]);
  for(const name of ['LICENSE','THIRD_PARTY_NOTICES.md']){
    const output=`Notices/${name}`;await copyFile(join(root,name),join(destination,output));files.push(output);
  }
  await writeFile(join(destination,'Notices/WEB-RUNTIME.txt'),
    'This Web runtime is based in part on the work of the Independent JPEG Group.\n'+
    'It uses the source-built browser dependencies recorded in CMakeCache.txt.\n'+
    'Build and relink from the accompanying PrismatiXEngine source and scripts/build-wasm-player.mjs.\n'+
    'The WebGL2 renderer uses SDL 3.4.10 (https://github.com/libsdl-org/SDL).\n'+
    'The experimental WebGPU renderer uses the SDL WebGPU backend proposed in\n'+
    'https://github.com/libsdl-org/SDL/pull/16020, built from\n'+
    'https://github.com/TheeStickmahn/SDL_wgpu at a pinned commit, and Dawn\'s\n'+
    'emdawnwebgpu bindings (https://github.com/google/dawn).\n'+
    'FriBidi 1.0.16 source: https://github.com/fribidi/fribidi/releases/tag/v1.0.16\n'+
    'PrismatiXEngine source: https://github.com/PrisX/PrismatiXEngine\n'+
    'Naga and SPIRV-Cross are export-time host tools, not browser runtime modules.\n');
  files.push('Notices/WEB-RUNTIME.txt');
  return files;
}

/**
 * Notices of one renderer's build (SDL, and Dawn for WebGPU); returns paths
 * relative to destination. dawnPackage is the unpacked emdawnwebgpu_pkg.
 */
export async function collectRendererNotices(build,destination,{dawnPackage}={}){
  build=resolve(build);destination=resolve(destination);
  const cache=await buildCache(build);
  const sdl=cache.get('SDL3_SOURCE_DIR');
  if(!sdl)throw new Error('Missing SDL3 dependency source in CMakeCache.txt');
  const components=[['SDL',sdl,['LICENSE.txt']]];
  if(dawnPackage)components.push(['Dawn',join(dawnPackage,'webgpu/src'),['LICENSE']]);
  return copyNotices(destination,components);
}

/**
 * Host tool notices: every Cargo registry crate's license text (the naga-cli
 * installation) and SPIRV-Cross's copyright, into <destination>/ToolNotices.
 */
export async function collectToolNotices(destination,{cargoRegistry,spirvCrossCopyright}){
  destination=resolve(destination);
  if(cargoRegistry){
    const registry=resolve(cargoRegistry);
    for(const index of await readdir(registry,{withFileTypes:true}))if(index.isDirectory()){
      for(const crate of await readdir(join(registry,index.name),{withFileTypes:true}))if(crate.isDirectory()){
        const directory=join(registry,index.name,crate.name);
        for(const file of await readdir(directory))if(/^(?:LICENSE|COPYING|NOTICE)(?:[.-].*)?$/iu.test(file)){
          await mkdir(join(destination,'ToolNotices',crate.name),{recursive:true});
          await copyFile(join(directory,file),join(destination,'ToolNotices',crate.name,file));
        }
      }
    }
  }
  if(spirvCrossCopyright){
    await mkdir(join(destination,'ToolNotices'),{recursive:true});
    await copyFile(spirvCrossCopyright,join(destination,'ToolNotices/spirv-cross.txt'));
  }
}

// node scripts/collect-web-notices.mjs <build-dir> <destination>
if(import.meta.url===pathToFileURL(process.argv[1]??'').href){
  const [build,destination]=process.argv.slice(2);
  if(!build||!destination)throw new Error('Usage: collect-web-notices.mjs <build-dir> <destination>');
  console.log(JSON.stringify([...await collectSharedNotices(build,destination),
    ...await collectRendererNotices(build,destination)]));
}
