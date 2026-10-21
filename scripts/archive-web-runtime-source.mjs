import {spawn} from 'node:child_process';
import {writeFile,rename,rm,mkdir} from 'node:fs/promises';
import {resolve,join} from 'node:path';
import {fileURLToPath} from 'node:url';
// node scripts/archive-web-runtime-source.mjs <destination> [web-player-build-dir]
// writes RuntimeSource.tar.gz (this checkout's runtime source) and
// FriBidiSource.tar.gz (the exact LGPL release source the build compiled).
const root=resolve(fileURLToPath(new URL('..',import.meta.url))),destination=resolve(process.argv[2]);
const build=resolve(process.argv[3]??join(root,'out/build/wasm-player'));
function run(executable,args,options={}){
  return new Promise((resolve,reject)=>{
    const child=spawn(executable,args,{windowsHide:true,cwd:root,...options});let output='';
    child.stdout?.on('data',bytes=>{output+=bytes});child.stderr?.on('data',bytes=>process.stderr.write(bytes));
    child.on('error',reject);child.on('close',code=>code===0?resolve(output):reject(new Error(`${executable} exited ${code}`)));
  });
}
await mkdir(destination,{recursive:true});
const paths=['CMakeLists.txt','CMakePresets.json','cmake','PrismatiXEngine','packages/authoring-sdk','packages/cli','scripts','docs/web-export.md','README.md','LICENSE','THIRD_PARTY_NOTICES.md','package.json','pnpm-lock.yaml','pnpm-workspace.yaml','vcpkg','vcpkg.json','vcpkg-configuration.json'];
const names=await run('git',['ls-files','-z','--cached','--others','--exclude-standard','--',...paths]);
const files=names.split('\0').filter(Boolean);
if(files.some(path=>path.startsWith('/')||path.split('/').includes('..')))throw new Error('Unsafe source archive path');
const list=join(destination,'.runtime-source-files'),pending=join(destination,'RuntimeSource.pending.tar.gz');
try{
  await writeFile(list,files.join('\0')+'\0');
  await run('tar',['-czf',pending,'-C',root,'--null','-T',list]);
  await rename(pending,join(destination,'RuntimeSource.tar.gz'));
}finally{await rm(list,{force:true});await rm(pending,{force:true});}
// Include the LGPL dependency's exact original release source alongside the
// full runtime source, rather than requiring a source offer from the host.
const source=join(build,'wasm-deps/fribidi-build/src/prismatix_fribidi_external');
await run('tar',['-czf',join(destination,'FriBidiSource.tar.gz'),'-C',source,'.']);
