import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, mkdir, readFile, writeFile, rm} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import {tmpdir} from 'node:os';
import test from 'node:test';

const cli = process.env.PRISMATIX_TEST_CLI ?? resolve('dist/src/cli.js');
async function fixture() {
  const root = await mkdtemp(join(tmpdir(), 'prismatix-release-'));
  await mkdir(join(root, 'Story'));
  await writeFile(join(root, 'Story/main.pxstory'), 'Original dialogue.\n[end]\n');
  return root;
}
const run = (args: string[]) => spawnSync(process.execPath, [cli, ...args, "--json"], {encoding: 'utf8', timeout: 30000});

test('artifact output never replaces Story or an unrelated directory', async () => {
  const root = await fixture();
  try {
    for (const output of [join(root, 'Story'), join(root, 'personal')]) {
      await mkdir(output, {recursive: true});
      await writeFile(join(output, 'author.txt'), 'Irreplaceable author material');
      const result = run(['build', root, '--artifact-only', '--output', output]);
      assert.notEqual(result.status, 0, result.stdout);
      assert.equal(await readFile(join(output, 'author.txt'), 'utf8'), 'Irreplaceable author material');
      assert.match(await readFile(join(root, 'Story/main.pxstory'), 'utf8'), /Original dialogue/);
    }
  } finally { await rm(root, {recursive:true, force:true}); }
});

test('build --output still requests a distribution', async () => {
  const root = await fixture();
  try {
    const result = run(['build', root, '--output', join(root, 'game'), '--runtime', join(root, 'missing-runtime')]);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /Packager|packager|runtime/i);
  } finally { await rm(root, {recursive:true, force:true}); }
});

async function until(check: () => Promise<boolean>, diagnostic: () => string): Promise<void> {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    if (await check()) return;
    await new Promise((accept) => setTimeout(accept, 50));
  }
  assert.fail(`Timed out: ${diagnostic()}`);
}

test('dev recovers first failure, retains acknowledged revision on rejection, and plays after crash', {timeout: 45000}, async () => {
  const root = await fixture();
  const host = join(root, 'host.mjs');
  const log = join(root, '.prismatix', 'host-log.jsonl');
  await mkdir(join(root, '.prismatix'));
  await mkdir(join(root, 'Story/dist'));
  await rm(join(root, 'Story/main.pxstory'));
  await writeFile(join(root, 'prismatix.json'), '{}');
  const story = join(root, 'Story/dist/main.pxstory');
  await writeFile(story, '[invalid-release-command]\n');
  await writeFile(host, `
    import {createInterface} from 'node:readline';
    import {appendFileSync,readFileSync,existsSync} from 'node:fs';
    import {join} from 'node:path';
    let ir = '';
    const log = ${JSON.stringify(log)};
    const rl=createInterface({input:process.stdin});
    rl.on('line', line=>{
      const q=JSON.parse(line);
      appendFileSync(log, JSON.stringify({...q,pid:process.pid})+'\\n');
      let type={hello:'ready',applyRuntimeIr:'runtimeIrApplied',play:'playAccepted',shutdown:'shutdownAccepted'}[q.type];
      if(q.type==='applyRuntimeIr') {
        ir=readFileSync(join(q.projectRoot,q.irPath),'utf8');
        if(ir.includes('REJECT-ME')) type='runtimeIrRejected';
      }
      process.stdout.write(JSON.stringify({...q,type,appliedRevision:q.revision,runtimeIrSchemaRevision:2,uiSchemaRevision:2})+'\\n');
      if(q.type==='shutdown') {rl.close();process.exit(0);}
      if(q.type==='play' && ir.includes('CRASH-ME') && !existsSync(log+'.crashed')) {
        appendFileSync(log+'.crashed','yes'); setTimeout(()=>process.exit(29),150);
      }
    });
  `);
  const child=spawn(process.execPath,[cli,'dev',root,'--preview-host',host,'--debounce','50','--json'],{stdio:['ignore','pipe','pipe']});
  let stderr='';child.stderr.on('data',c=>stderr+=String(c));child.stdout.resume();
  const events=async ():Promise<Array<Record<string,unknown>>>=>{
    try{return (await readFile(log,'utf8')).trim().split('\n').filter(Boolean).map(s=>JSON.parse(s));}catch{return [];}
  };
  try {
    await until(async()=>stderr.includes('PXCLI1606'),()=>stderr);
    assert.equal(child.exitCode,null);
    await writeFile(story,'Good original.\n[end]\n');
    await until(async()=>(await events()).some(e=>e.type==='play'),()=>stderr);
    const first=(await events()).find(e=>e.type==='applyRuntimeIr')!;
    const original=await readFile(join(String(first.projectRoot),String(first.irPath)),'utf8');
    await writeFile(story,'REJECT-ME\n[end]\n');
    await until(async()=>(await events()).filter(e=>e.type==='applyRuntimeIr').length>=2,()=>stderr);
    await until(async()=>stderr.includes('runtimeIrRejected'),()=>stderr);
    assert.equal(await readFile(join(String(first.projectRoot),String(first.irPath)),'utf8'),original);
    assert.ok(!(await events()).some(e=>e.pid===first.pid && e.type==='shutdown'));
    await writeFile(story,'CRASH-ME\n[end]\n');
    await until(async()=>(await events()).filter(e=>e.type==='play').length>=3,()=>stderr);
    const plays=(await events()).filter(e=>e.type==='play');
    assert.notEqual(plays.at(-1)!.pid,plays.at(-2)!.pid);
  } finally {
    child.kill('SIGTERM');
    await new Promise<void>(accept=>{if(child.exitCode!==null)accept();else child.once('exit',()=>accept());});
    // Windows force termination does not deliver SIGTERM to Node; close any test-only host.
    for(const pid of new Set((await events()).map(e=>Number(e.pid)))) {try{process.kill(pid);}catch{}}
    await rm(root,{recursive:true,force:true,maxRetries:10,retryDelay:100});
  }
});

test('typed options reject misspellings, missing values and conflicting modes', async () => {
  const root=await fixture();
  try {
    for(const args of [['build',root,'--ouptut','x'],['dev',root,'--debounce'],['build',root,'--artifact-only','--distribution']]) {
      const result=run(args);
      assert.equal(result.status,2,result.stderr);
      assert.match(result.stderr,/PXCLI0001/);
    }
    const good=run(['build','--artifact-only',root,'--output',join(root,'result')]);
    assert.equal(good.status,0,good.stderr);
  } finally { await rm(root,{recursive:true,force:true}); }
});

test('Preview rejects incompatible contracts, oversized frames and stale acknowledgements', {timeout:30000}, async()=>{
  const root=await fixture();
  try {
    for(const mode of ['schema','oversize','stale']) {
      const host=join(root,`${mode}.mjs`);
      await writeFile(host,`
        import {createInterface} from 'node:readline';
        const rl=createInterface({input:process.stdin});
        rl.on('line', line=>{
          const q=JSON.parse(line);
          if(q.type==='hello' && '${mode}'==='oversize') {process.stdout.write('x'.repeat(17*1024*1024));return;}
          const type={hello:'ready',applyRuntimeIr:'staleRevision',shutdown:'shutdownAccepted'}[q.type];
          process.stdout.write(JSON.stringify({...q,type,runtimeIrSchemaRevision:${mode==='schema'?1:2},uiSchemaRevision:2,appliedRevision:q.revision})+'\\n');
          if(q.type==='shutdown') {rl.close();process.exit(0);}
        });
      `);
      const result=run(['dev',root,'--once','--preview-host',host]);
      assert.equal(result.status,1,result.stderr);
      assert.match(result.stderr,mode==='schema'?/incompatible IR\/UI/:mode==='oversize'?/exceeds 16 MiB/:/staleRevision/);
    }
  } finally { await rm(root,{recursive:true,force:true,maxRetries:10,retryDelay:100}); }
});

test('pack rejects author directories before invoking a native packager', async()=>{
  const root=await fixture();
  try {
    const tool=join(root,'packager.mjs');
    const sentinel=join(root,'invoked');
    await writeFile(tool,`import {writeFileSync} from 'node:fs';writeFileSync(${JSON.stringify(sentinel)},'unsafe');`);
    const original=await readFile(join(root,'Story/main.pxstory'),'utf8');
    const result=run(['pack',root,'--output',join(root,'Story'),'--packager',tool,'--player',tool]);
    assert.equal(result.status,1,result.stderr);
    assert.match(result.stderr,/overlaps author input/);
    await assert.rejects(readFile(sentinel));
    assert.equal(await readFile(join(root,'Story/main.pxstory'),'utf8'),original);
  } finally { await rm(root,{recursive:true,force:true}); }
});
