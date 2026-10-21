import {readFile,writeFile,mkdir,copyFile,readdir} from 'node:fs/promises';
import {join,resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

const root=resolve(fileURLToPath(new URL('..',import.meta.url)));
const build=resolve(process.argv[2]),destination=resolve(process.argv[3]);
const cache=new Map((await readFile(join(build,'CMakeCache.txt'),'utf8')).split(/\r?\n/u)
  .flatMap(line=>{const match=/^([^#/:][^:=]*):[^=]*=(.*)$/u.exec(line);return match?[[match[1],match[2]]]:[]}));
const source=name=>{
  const directory=cache.get(`${name}_SOURCE_DIR`);
  if(!directory)throw new Error(`Missing ${name} dependency source in CMakeCache.txt`);
  return directory;
};
const components=[
  ['SDL',source('SDL3'),['LICENSE.txt']],
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
];
const files=[];
for(const [name,directory,licenses] of components){
  await mkdir(join(destination,'Notices',name),{recursive:true});
  for(const license of licenses){
    const output=`Notices/${name}/${license.replaceAll('/','-')}`;
    await copyFile(join(directory,license),join(destination,output));files.push(output);
  }
}
for(const name of ['LICENSE','THIRD_PARTY_NOTICES.md']){
  const output=`Notices/${name}`;await copyFile(join(root,name),join(destination,output));files.push(output);
}
await writeFile(join(destination,'Notices/WEB-RUNTIME.txt'),
  'This Web runtime is based in part on the work of the Independent JPEG Group.\n'+
  'It uses the source-built browser dependencies recorded in CMakeCache.txt.\n'+
  'Build and relink from the accompanying PrismatiXEngine source and scripts/build-wasm-player.ps1.\n'+
  'FriBidi 1.0.16 source: https://github.com/fribidi/fribidi/releases/tag/v1.0.16\n'+
  'PrismatiXEngine source: https://github.com/PrisX/PrismatiXEngine\n'+
  'Naga and SPIRV-Cross are export-time host tools, not browser runtime modules.\n');
files.push('Notices/WEB-RUNTIME.txt');
// The tool installation has its own notices. Include every Cargo registry
// dependency installed for naga-cli, retaining each original license text.
if(process.argv[4]){
  const registry=resolve(process.argv[4]);
  for(const index of await readdir(registry,{withFileTypes:true}))if(index.isDirectory()){
    for(const crate of await readdir(join(registry,index.name),{withFileTypes:true}))if(crate.isDirectory()){
      const directory=join(registry,index.name,crate.name);
      for(const file of await readdir(directory))if(/^(?:LICENSE|COPYING|NOTICE)(?:[.-].*)?$/iu.test(file)){
        const output=`ToolNotices/${crate.name}/${file}`;
        await mkdir(join(destination,'ToolNotices',crate.name),{recursive:true});
        await copyFile(join(directory,file),join(destination,output));
      }
    }
  }
}
console.log(JSON.stringify(files));
