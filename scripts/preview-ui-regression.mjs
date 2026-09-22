#!/usr/bin/env node
// Real native PreviewHost only. No mocked protocol or synthetic UI actions.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, cp, readFile, writeFile, mkdir} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve, join, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createInterface} from 'node:readline';
import {setTimeout as delay} from 'node:timers/promises';
const repo = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const host = resolve(process.argv[2] ?? join(repo, 'out/install/release-candidate/bin/PrismatiXPreviewHost.exe'));
const root = await mkdtemp(join(tmpdir(), 'prismatix-preview-ui-'));
await cp(join(repo, 'PrismatiXEngine/Tests/Fixtures/PreviewHostScript'), root, {recursive:true});
const manifest = JSON.parse(await readFile(join(root,'project.pxproject'),'utf8'));
manifest.uiEntryPoints = {}; manifest.extensions = [];
await writeFile(join(root,'project.pxproject'), JSON.stringify(manifest));
await mkdir(join(root,'Content/Localization'),{recursive:true});
await mkdir(join(root,'Content/Fonts'),{recursive:true});
await cp(join(repo,'packages/cli/templates/minimal/Content/Fonts/NotoSansTC-Bold.ttf'),join(root,'Content/Fonts/NotoSansTC-Bold.ttf'));
await writeFile(join(root,'Content/Localization/en.json'),JSON.stringify({format:'PrismatiXLocale',schemaRevision:2,locale:'en',strings:{},fontChain:['Content/Fonts/NotoSansTC-Bold.ttf']}));
let transcript = '', errors = '', sequence = 0;
function session() {
  let revision = 1;
  const child = spawn(host, [], {cwd:root, windowsHide:true, stdio:['pipe','pipe','pipe']});
  const pending = new Map();
  child.stderr.on('data', data => { errors += data; });
  createInterface({input:child.stdout}).on('line',line => {
    transcript += line+'\n';
    let value; try {value=JSON.parse(line);} catch{return;}
    const waiter = pending.get(value.requestId);
    if (waiter && !value.type?.endsWith('Event')) {pending.delete(value.requestId);clearTimeout(waiter.timer);waiter.resolve(value);}
  });
  child.on('exit', code => {for(const waiter of pending.values()) waiter.reject(new Error(`Host exited ${code}`));});
  async function request(type, fields={}) {
    const requestId=String(++sequence);
    const response = await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>{pending.delete(requestId);reject(new Error(`Timeout ${type}`));},15000);
      pending.set(requestId,{resolve,reject,timer});
      child.stdin.write(JSON.stringify({protocol:'PrismatiXPreviewProtocol',schemaRevision:2,protocolVersion:2,sessionId:'native-ui-test',requestId,documentId:'preview-controls-document',revision,type,...fields})+'\n');
    });
    assert.notEqual(response.type,'error',JSON.stringify(response));
    if (type === 'applyRuntimeIr') revision = fields.committedRevision;
    return response;
  }
  async function route(expected) {
    for(let i=0;i<40;i++) {await delay(30);const state=await request('capture');if(state.uiRoute===expected)return state;}
    throw new Error(`UI did not reach ${expected}`);
  }
  async function activate(state, text) {
    assert.ok(Array.isArray(state.uiControls), 'Host must include bounded uiControls diagnostics; rebuild/install current candidate');
    assert.ok(state.uiControls.length <= 64);
    assert.ok(state.uiControls.every(c => Buffer.byteLength(c.text ?? '', 'utf8') <= 512));
    const control=state.uiControls.find(c=>c.visible && c.text?.startsWith(text));
    assert.ok(control,`Missing visible ${text}: ${JSON.stringify(state.uiControls)}`);
    assert.ok(control.enabled,`${text} disabled`);
    await request('activateUiControl',{nodeId:control.nodeId});
  }
  return {child,request,route,activate};
}
async function boot(s) {
  assert.equal((await s.request('hello',{documentId:'',revision:0})).type,'ready');
  await s.request('applyRuntimeIr',{projectRoot:root,committedRevision:1,irPath:'preview-controls.pxir'});
  await s.request('play',{showTitle:true});
  return s.route('title');
}
let active;
try {
  active=session(); const title=await boot(active);
  await active.activate(title,'開始遊戲');
  const initial=await active.route('hud');
  await active.activate(initial,'SAVE');
  const save=await active.route('save');
  await delay(180); assert.equal((await active.request('capture')).uiRoute,'save','save modal survives frames');
  await active.activate(save,'Preview checkpoint 1'); await active.route('hud');
  await active.request('advance'); await active.request('advance');
  await delay(80);
  let changed=await active.request('capture');
  if(changed.choiceCount>0) {await active.request('selectChoice',{index:0});await delay(50);changed=await active.request('capture');}
  assert.notEqual(changed.sourceId,initial.sourceId,'story progressed after save');
  await active.activate(changed,'LOAD');
  const load=await active.route('load');
  await delay(180);assert.equal((await active.request('capture')).uiRoute,'load','load modal survives frames');
  await active.activate(load,'Preview checkpoint 1');
  const restored=await active.route('hud');
  assert.equal(restored.sourceId,initial.sourceId,'load restores saved story source');
  const rebuilt = JSON.parse(await readFile(join(root,'preview-controls.pxir'),'utf8'));
  rebuilt.committedRevision = 2;
  await writeFile(join(root,'rebuilt.pxir'),JSON.stringify(rebuilt));
  await active.request('applyRuntimeIr',{projectRoot:root,revision:2,committedRevision:2,irPath:'rebuilt.pxir'});
  await active.request('play');
  await active.activate(await active.route('hud'),'LOAD');
  const rebuiltLoad = await active.route('load');
  const rebuiltSlots = rebuiltLoad.uiControls.filter(c=>c.text?.startsWith('Preview checkpoint'));
  assert.equal(rebuiltSlots.length,4);
  assert.ok(rebuiltSlots.every(c=>!c.enabled),'structural rebuild clears Preview slots');
  await active.request('shutdown',{documentId:'',revision:0});active.child.stdin.end();
  active=session();await boot(active);
  const title2=await active.request('capture');await active.activate(title2,'開始遊戲');
  await active.activate(await active.route('hud'),'LOAD');
  const empty=await active.route('load');
  const slots=empty.uiControls.filter(c=>c.text?.startsWith('Preview checkpoint'));
  assert.equal(slots.length,4);assert.ok(slots.every(c=>!c.enabled),'new Host has no persistent Preview slots');
  await active.request('shutdown',{documentId:'',revision:0});active.child.stdin.end();
  console.log(`PASS native Title / Start / HUD / Save / Load / session-only checkpoints. Evidence: ${root}`);
} finally {
  active?.child.kill();
  await writeFile(join(root,'protocol.ndjson'),transcript);await writeFile(join(root,'host-stderr.log'),errors);
  console.log(`Native Preview UI evidence: ${root}`);
}


