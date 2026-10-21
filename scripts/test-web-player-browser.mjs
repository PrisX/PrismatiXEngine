import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {mkdtemp, mkdir, rm, writeFile, access, readFile} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join, resolve} from "node:path";
import {createHash} from "node:crypto";
import {webExportServer} from "./serve-web-export.mjs";

assert.ok(process.argv[2], "Usage: node scripts/test-web-player-browser.mjs <export-directory> [browser]");
const root = resolve(process.argv[2]);
const qaRoot = `${root}-qa`;
await mkdir(qaRoot, {recursive: true});
const overrides=new Map();
const server = webExportServer(root,overrides);
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const url = `http://127.0.0.1:${server.address().port}/`;
const candidates = [process.argv[3], process.env.PRISMATIX_BROWSER,
  "C:/Program Files/Google/Chrome/Application/chrome.exe",
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe", "/usr/bin/chromium", "/usr/bin/google-chrome"];
let executable;
for (const candidate of candidates) if (candidate) {
  try { await access(candidate); executable = candidate; break; } catch {}
}
assert.ok(executable, "A Chrome/Chromium browser is required");
const exported = JSON.parse(await readFile(join(root, "web-export.json"), "utf8"));
const renderer = exported.renderer ?? "webgl2";
console.log(`Web export: ${exported.title} (renderer ${renderer}, engine ${exported.engineVersion ?? "unknown"})`);
const profile = await mkdtemp(join(tmpdir(), "px-web-player-"));
const hardware = process.env.PRISMATIX_WEB_HARDWARE === "1";
// Without a GPU, a WebGPU export runs on Dawn's SwiftShader fallback adapter,
// which Chrome offers only with GPU acceleration off and without
// --enable-unsafe-swiftshader (the WebGL route returns no adapter).
const softwareWebGpu = !hardware && renderer === "webgpu";
const browser = spawn(executable, ["--headless=new", "--no-sandbox", "--disable-dev-shm-usage",
  "--no-first-run", "--no-default-browser-check", "--enable-unsafe-webgpu",
  ...(hardware ? ["--enable-webgl", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"] :
    softwareWebGpu ? ["--disable-gpu"] :
    ["--enable-webgl", "--enable-unsafe-swiftshader", "--use-gl=angle", "--use-angle=swiftshader",
      "--use-webgpu-adapter=swiftshader", "--enable-features=Vulkan", "--use-vulkan=swiftshader"]),
  "--remote-debugging-port=0", `--user-data-dir=${profile}`, "about:blank"],
  {windowsHide: true, stdio: ["ignore", "pipe", "pipe"]});
let log = "", socket, closeBrowser;
browser.stderr.on("data", data => { log += data; });
browser.stdout.on("data", data => { log += data; });
const messages = [], exceptions = [];
async function until(condition, label, timeout = 90000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    const value = await condition(); if (value) return value;
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out: ${label}\n${messages.slice(-20).join("\n")}\n${log.slice(-2048)}`);
}
try {
  const endpoint = await until(async () => {
    const printed = /DevTools listening on (ws:\/\/[^\s]+)/u.exec(log)?.[1];
    if(printed)return printed;
    const lines=(await readFile(join(profile,'DevToolsActivePort'),'utf8').catch(()=>'' )).trim().split(/\r?\n/u);
    return /^\d+$/u.test(lines[0]) && lines[1]?.startsWith('/devtools/browser/') ? `ws://127.0.0.1:${lines[0]}${lines[1]}` : null;
  }, "DevTools");
  const host = new URL(endpoint).host;
  const target = await (await fetch(`http://${host}/json/new?about:blank`, {method: "PUT"})).json();
  socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener("open", resolve, {once: true}); socket.addEventListener("error", reject, {once: true});
  });
  const pending = new Map(); let next = 1, pageLoads = 0;
  socket.addEventListener("message", event => {
    const value = JSON.parse(event.data);
    if(value.method==="Page.loadEventFired")pageLoads++;
    if (value.method === "Runtime.consoleAPICalled") messages.push(value.params.args.map(arg => arg.value ?? arg.description).join(" "));
    if (value.method === "Runtime.exceptionThrown") {
      const detail = JSON.stringify(value.params.exceptionDetails);
      exceptions.push(detail); messages.push(detail);
    }
    if (value.id && pending.has(value.id)) {
      const {resolve, reject, timer} = pending.get(value.id); pending.delete(value.id); clearTimeout(timer);
      value.error ? reject(new Error(JSON.stringify(value.error))) : resolve(value.result);
    }
  });
  const command = (method, params = {}, timeout = 180000) => new Promise((resolve, reject) => {
    const id = next++;
    const timer = setTimeout(() => {pending.delete(id); reject(new Error(`CDP command timed out: ${method}\n${messages.slice(-20).join('\n')}`));}, timeout);
    pending.set(id, {resolve, reject, timer}); socket.send(JSON.stringify({id, method, params}));
  });
  closeBrowser=()=>command('Browser.close',{},5000).catch(()=>{});
  const evaluate = async expression => {
    const result = await command("Runtime.evaluate", {expression, returnByValue: true, awaitPromise: true});
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  const reloadPage = async () => {
    const before=pageLoads;await command('Page.reload');
    await until(()=>pageLoads>before,'new page load');
    await until(()=>evaluate("document.getElementById('start')?.disabled===false"),'new Player initialization');
  };
  await command("Runtime.enable"); await command("Page.enable");
  const mobile = process.env.PRISMATIX_WEB_MOBILE === "1";
  await command("Emulation.setDeviceMetricsOverride", {width: mobile ? 844 : 1280, height: mobile ? 390 : 720, deviceScaleFactor: 1, mobile});
  if (mobile) {
    await command("Emulation.setTouchEmulationEnabled", {enabled:true,maxTouchPoints:5});
    const version = (await evaluate("navigator.userAgent")).match(/(?:HeadlessChrome|Chrome)\/([\d.]+)/u)?.[1] ?? "130.0.0.0";
    await command("Emulation.setUserAgentOverride", {userAgent:`Mozilla/5.0 (Linux; Android 14; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${version} Mobile Safari/537.36`,platform:"Linux armv8l"});
  }
  await command("Page.navigate", {url});
  await until(async () => {
    const status = await evaluate("document.getElementById('status')?.textContent");
    if (status) throw new Error(`${status}\n${messages.slice(-12).join("\n")}`);
    return evaluate("document.getElementById('start')?.disabled === false");
  }, "shipping Player initialization");
  const button = await evaluate("(()=>{const r=document.getElementById('start').getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()");
  for (const type of ["mousePressed", "mouseReleased"]) await command("Input.dispatchMouseEvent", {type, ...button, button: "left", clickCount: 1});
  await until(async () => {
    const status = await evaluate("document.getElementById('status').textContent");
    if (status) throw new Error(`${status}\n${messages.slice(-15).join("\n")}`);
    return messages.some(text => /Player presentation ready/u.test(text));
  }, "native Player boot");
  await new Promise(resolve => setTimeout(resolve, 600));
  assert.deepEqual(exceptions, [], "Player raised an uncaught browser exception");
  const screenshot = await command("Page.captureScreenshot", {format: "png"});
  await writeFile(join(qaRoot, "browser-smoke.png"), Buffer.from(screenshot.data, "base64"));
  await writeFile(join(qaRoot, "browser-smoke.log"), messages.join("\n"));
  assert.equal(await evaluate("document.getElementById('welcome').hidden"), true);
  assert.equal(await evaluate("document.title"), exported.title, "page title is not the game title");
  const device = messages.find(text => /Graphics device created/u.test(text)) ?? "no graphics device line";
  console.log(`Web: ${device}`);
  // The WebGPU renderer must actually run on SDL's GPU renderer (WebGPU).
  if (renderer === "webgpu")
    assert.match(device, /driver=(?:gpu|webgpu)\b/u, "WebGPU export did not create an SDL_GPU renderer");
  if (process.env.PRISMATIX_WEB_TEST_PLATFORM === "1") {
    const layout = await evaluate("(()=>{const r=document.getElementById('canvas').getBoundingClientRect();return{width:r.width,height:r.height,viewportWidth:innerWidth,viewportHeight:innerHeight}})()");
    assert.ok(layout.width > 0 && layout.height > 0 && layout.width <= layout.viewportWidth + 1 && layout.height <= layout.viewportHeight + 1, "canvas does not fit the viewport");
    await evaluate("document.getElementById('tools').open=true");
    const position=await evaluate("(()=>{const r=document.getElementById('fullscreen').getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()");
    for(const type of ['mousePressed','mouseReleased'])await command('Input.dispatchMouseEvent',{type,...position,button:'left',clickCount:1});
    await until(()=>evaluate("!!document.fullscreenElement"),'browser fullscreen');
    await evaluate("document.exitFullscreen()");
    await until(()=>evaluate("!document.fullscreenElement"),'browser fullscreen exit');
    await evaluate("document.getElementById('tools').open=false");
    console.log(`Web: ${mobile?'mobile emulation':'desktop'} layout and browser fullscreen passed`);
  }
  if (process.env.PRISMATIX_WEB_TEST_STORY === "1") {
    // Title screen, start, first Story line, quick save, quick load: through
    // the semantic controls the canvas publishes. Labels default to the
    // Starter sample's; PRISMATIX_WEB_TEST_FIRST_LINE checks the first line.
    const label = (name, fallback) => process.env[`PRISMATIX_WEB_TEST_${name}`] ?? fallback;
    const visible = text => `[...document.querySelectorAll('#semantics [aria-label]')].find(e=>e.getAttribute('aria-label')===${JSON.stringify(text)}&&!e.closest('[hidden]'))`;
    const activate = async text => {
      await until(() => evaluate(`!!(${visible(text)})`), `control ${text}`);
      assert.equal(await evaluate(`(()=>{const e=${visible(text)};if(e.getAttribute('aria-disabled')==='true')return false;e.click();return true})()`), true, `${text} is disabled`);
    };
    const capture = async name => {
      const shot = await command("Page.captureScreenshot", {format: "png"});
      await writeFile(join(qaRoot, `${name}.png`), Buffer.from(shot.data, "base64"));
    };
    const start = label("START_LABEL", "開始遊戲");
    await until(() => evaluate(`!!(${visible(start)})`), "title screen", 120000);
    await new Promise(resolve => setTimeout(resolve, 800));
    await capture("story-title");
    // A Story line is new text (not on the title screen) in a text or log
    // element, such as the HUD's Dialogue; it settles once it is fully shown.
    const titleTexts = await evaluate("[...document.querySelectorAll('#semantics [data-text]')].map(e=>e.dataset.text)");
    await activate(start);
    const firstLine = process.env.PRISMATIX_WEB_TEST_FIRST_LINE ?? "";
    const readLine = () => evaluate(`(()=>{const title=new Set(${JSON.stringify(titleTexts)});
      const e=[...document.querySelectorAll('#semantics [role=note],#semantics [role=log]')]
        .find(e=>!e.closest('[hidden]')&&e.dataset.text.length>0&&!title.has(e.dataset.text)&&e.dataset.text.includes(${JSON.stringify(firstLine)}));
      return e?.dataset.text??null})()`);
    let line = await until(readLine, "first Story line", 120000);
    for (let settled = 0; settled < 20; settled++) {
      await new Promise(resolve => setTimeout(resolve, 600));
      const next = await readLine();
      if (next === line) break;
      line = next ?? line;
    }
    console.log(`Web: first Story line "${line}"`);
    await new Promise(resolve => setTimeout(resolve, 1500));
    await capture("story-first-line");
    const before = messages.length;
    await activate(label("QUICK_SAVE", "Q.SAVE"));
    await until(() => messages.slice(before).some(text => /Saved slot \d+/u.test(text)), "quick save");
    await activate(label("QUICK_LOAD", "Q.LOAD"));
    await until(() => messages.slice(before).some(text => /Loaded slot \d+/u.test(text)), "quick load");
    await until(() => evaluate(`[...document.querySelectorAll('#semantics [data-text]')].some(e=>!e.closest('[hidden]')&&e.dataset.text===${JSON.stringify(line)})`),
      "restored Story line after load");
    await new Promise(resolve => setTimeout(resolve, 800));
    await capture("story-loaded");
    assert.ok(!messages.some(text => /PXPLAYER600[12]|PXPERSIST600[167]/u.test(text)), "save/load persistence failure");
    console.log("Web: title, start, first line, quick save and quick load passed");
  }
  if (process.env.PRISMATIX_WEB_TEST_VIDEO === "1") {
    await evaluate(`(async()=>{
      const {WebVideo}=await import('./video.mjs');const open=WebVideo.prototype.open;
      WebVideo.prototype.open=function(...args){globalThis.testVideo=this;return open.apply(this,args)};
      const start=[...document.querySelectorAll('#semantics button')].find(e=>e.getAttribute('aria-label')==='開始遊戲');
      if(!start)throw new Error('Video fixture Start is missing');${mobile ? '' : 'start.click();'}
    })()`);
    if(mobile){
      const position=await evaluate("(()=>{const r=document.getElementById('canvas').getBoundingClientRect();return{x:r.x+r.width*226/1280,y:r.y+r.height*325/720}})()");
      await command('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{...position,id:1,radiusX:8,radiusY:8,force:1}]});
      await new Promise(resolve=>setTimeout(resolve,100));
      await command('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
    }
    await until(()=>evaluate("globalThis.testVideo?.items.size>0"),"engine video open");
    await until(()=>evaluate("[...testVideo.items.values()].some(i=>i.video.readyState>=2&&i.video.currentTime>.2)"),"engine video decoding");
    await new Promise(resolve=>setTimeout(resolve,400));
    if(process.env.PRISMATIX_WEB_TEST_PLATFORM==='1'){
      const other=await command('Target.createTarget',{url:'about:blank'});
      try{
        await command('Target.activateTarget',{targetId:other.targetId});
        await until(()=>evaluate("document.hidden"),'background page visibility');
        await until(()=>evaluate("[...testVideo.items.values()].every(i=>i.video.paused)"),'native lifecycle movie suspension');
        await command('Target.activateTarget',{targetId:target.id});
        await until(()=>evaluate("!document.hidden"),'foreground page visibility');
        await until(()=>evaluate("[...testVideo.items.values()].some(i=>!i.video.paused)"),'native lifecycle movie resume');
      }finally{await command('Target.closeTarget',{targetId:other.targetId});}
      console.log('Web: native background/foreground movie suspension passed');
    }
    const shot=await command("Page.captureScreenshot",{format:"png"});
    await writeFile(join(qaRoot,"engine-video.png"),Buffer.from(shot.data,"base64"));
    // Keep the engine movie alive for the independent decoder/seek checks.
    await evaluate("testVideo.control([...testVideo.items.keys()][0],'pause')");
  }
  if (process.env.PRISMATIX_WEB_TEST_PORTED_GAME === "1") {
    const visible = label => `Array.from(document.querySelectorAll('#semantics [aria-label]')).find(e=>e.getAttribute('aria-label')===${JSON.stringify(label)}&&!e.closest('[hidden]'))`;
    const activate = async label => {
      await until(() => evaluate(`!!(${visible(label)})`), `Ported game control: ${label}`);
      assert.equal(await evaluate(`(()=>{const e=${visible(label)};if(e.getAttribute('aria-disabled')==='true')return false;e.click();return true})()`), true);
      await new Promise(resolve=>setTimeout(resolve,250));
    };
    const capture = async name => {
      const shot=await command("Page.captureScreenshot",{format:"png"});
      await writeFile(join(qaRoot,`${name}.png`),Buffer.from(shot.data,"base64"));
    };
    await activate("設定");
    await until(()=>evaluate(`!!(${visible("Return")})`),"Ported game settings");
    await activate("設定 3");
    await until(()=>evaluate(`!!(${visible("マスター音量")})`),"Ported game volume slider");
    await capture("settings");
    await evaluate(`(()=>{const e=${visible("マスター音量")};e.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowLeft',bubbles:true}));})()`);
    await activate("Return");
    await activate("はじめから");
    console.log("Ported game: playing the authored opening before save/load checks");
    await until(()=>evaluate(`(()=>{const e=${visible("本文")};return e&&e.dataset.text.length>10})()`),"Ported game opening dialogue",180000);
    await capture("story");
    await evaluate("document.getElementById('canvas').focus()");
    for(const type of ["keyDown","keyUp"]){
      await command("Input.dispatchKeyEvent",{type,key:"F6",code:"F6",windowsVirtualKeyCode:117,nativeVirtualKeyCode:117});
      await new Promise(resolve=>setTimeout(resolve,100));
    }
    await activate("セーブデータ");
    await activate("はい");
    await until(()=>messages.some(text=>/Saved slot 1\b/u.test(text)),"Ported game encrypted slot write");
    await capture("save");
    await activate("Title");await activate("はい");
    await activate("ロード");await activate("セーブデータ");await activate("はい");
    await until(()=>messages.some(text=>/Loaded slot 1\b/u.test(text)),"Ported game encrypted slot load");
    await until(()=>evaluate(`!!(${visible("本文")})`),"Ported game restored dialogue");
    await capture("load");
    assert.ok(!messages.some(text=>/PXPLAYER600[12]|PXPERSIST600[167]/u.test(text)),"Ported game persistence failure");
    console.log("Ported game: settings, opening, save and load passed");
  }
  if (process.env.PRISMATIX_WEB_TEST_EFFECTS === "1") {
    const result = await evaluate(`(async()=>{
      const {WebEffects}=await import('./effects.mjs');
      const {WebFiles}=await import('./files.mjs');
      const manifest=await(await fetch('./web-export.json')).json();
      const canvas=document.createElement('canvas');canvas.width=32;canvas.height=16;
      const gl=canvas.getContext('webgl2');
      const effects=new WebEffects(manifest,new WebFiles(manifest,new URL('./',location.href)),new URL('./',location.href));await effects.initialize(gl,{webgpu:true});
      const original=effects.backend;
      const texture=bytes=>{const t=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,t);
        gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,32,16,0,gl.RGBA,gl.UNSIGNED_BYTE,bytes);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);return t;};
      const data=new Uint8Array(32*16*4);
      for(let y=0;y<16;y++)for(let x=0;x<32;x++)data.set([80+x*5,40+y*10,10+x*3+y,255],(y*32+x)*4);
      const source=texture(data),output=texture(null),uniforms=new Float32Array([1/32,1/16,0,0,0,0,1,0]);
      const cases=[];
      const custom=[];
      for(const [id,mapping] of Object.entries(manifest.effects??{}))custom.push([id,await effects.load(id,mapping.fingerprint)]);
      for(const backend of ['webgl2',...(effects.device?['webgpu']:[])]){
        effects.backend=backend;
        await effects.pass(1,source,null,output,32,16,new Uint8Array(uniforms.buffer));
        if(effects.backend!==backend)throw new Error(backend+' shader did not execute');
        const pixels=effects.withState(()=>effects.readTexture(output,32,16));
        for(const [x,y] of [[0,0],[16,8],[31,15]]){
          const offset=(y*32+x)*4,luma=Math.round(data[offset]*.299+data[offset+1]*.587+data[offset+2]*.114);
          if([0,1,2].some(i=>Math.abs(pixels[offset+i]-luma)>1)||pixels[offset+3]!==255)
            throw new Error(backend+' effect/color/orientation mismatch '+x+','+y+': '+pixels.slice(offset,offset+4)+' expected '+luma);
        }
        cases.push(backend);
        for(const [id,handle] of custom){
          const bytes=new Float32Array(40);bytes[0]=1/32;bytes[1]=1/16;bytes[2]=1;bytes[4]=1;bytes[38]=32;bytes[39]=16;
          if(id==='dream-transition')await effects.pass(handle,source,source,output,32,16,new Uint8Array(bytes.buffer));
          else if(id==='dream-node'){
            const framebuffer=gl.createFramebuffer();gl.bindFramebuffer(gl.FRAMEBUFFER,framebuffer);
            gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,output,0);
            gl.clearColor(0,0,0,0);gl.clear(gl.COLOR_BUFFER_BIT);
            await effects.geometry(handle,source,effects.quad(32,16),32,16,true,0,[4,2,24,12],new Uint8Array(bytes.buffer),32,16);
            gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.deleteFramebuffer(framebuffer);
          }else await effects.pass(handle,source,null,output,32,16,new Uint8Array(bytes.buffer));
          if(effects.backend!==backend)throw new Error(backend+' custom effect fell back: '+id);
          const pixels=effects.withState(()=>effects.readTexture(output,32,16)),offset=(8*32+16)*4;
          const expected=[data[offset+2],data[offset],data[offset+1],255];
          if(expected.some((value,i)=>Math.abs(pixels[offset+i]-value)>1))throw new Error(backend+' '+id+' pixels '+pixels.slice(offset,offset+4)+' expected '+expected);
          if(id==='dream-node'&&pixels.slice(0,4).some(value=>value!==0))throw new Error(backend+' node scissor was ignored');
        }
      }
      effects.destroy();
      const standard=new WebEffects(manifest,new WebFiles(manifest,new URL('./',location.href)),new URL('./',location.href));
      await standard.initialize(gl);const preferred=standard.backend;standard.destroy();
      return{preferred,optIn:original,passed:cases,custom:custom.map(([id])=>id)};
    })()`);
    await writeFile(join(qaRoot, "effects-result.json"), JSON.stringify(result, null, 2));
    assert.ok(result.passed.includes("webgl2"));
    assert.equal(result.preferred, "webgl2", "effects must prefer WebGL2 unless WebGPU effects are requested");
    if (process.env.PRISMATIX_WEB_REQUIRE_WEBGPU === "1") assert.ok(result.passed.includes("webgpu"), "WebGPU adapter unavailable");
  }
  if (process.env.PRISMATIX_WEB_TEST_VIDEO === "1") {
    await evaluate("for(const id of testVideo.items.keys())testVideo.control(id,'play')");
    await until(()=>evaluate("testVideo.items.size===0"),"engine movie completion",30000);
    await until(async()=>{
      const status=await evaluate("document.getElementById('status').textContent");if(status)throw new Error(status+'\n'+messages.slice(-15).join('\n'));
      return evaluate("[...document.querySelectorAll('#semantics [data-text]')].some(e=>e.dataset.text.includes('你好'))");
    },"story after engine movie");
    assert.ok(!messages.some(text=>/WebGPU fallback|Web effect artifact mismatch/u.test(text)),"native custom stage effect failed");
    const screenshot=await command('Page.captureScreenshot',{format:'png'});
    await writeFile(join(qaRoot,'custom-stage-story.png'),Buffer.from(screenshot.data,'base64'));
  }
  if (process.env.PRISMATIX_WEB_PROFILE) {
    const profile=await readFile(resolve(process.env.PRISMATIX_WEB_PROFILE),'utf8'),before=pageLoads;
    await evaluate(`(()=>{const input=document.getElementById('import-saves'),data=new DataTransfer();
      data.items.add(new File([${JSON.stringify(profile)}],'fixture.pxwebsave',{type:'application/json'}));
      input.files=data.files;input.dispatchEvent(new Event('change',{bubbles:true}));})()`);
    await until(()=>pageLoads>before,'save import reload');
    await until(()=>evaluate("document.getElementById('start')?.disabled===false"),'imported Player initialization');
    const position=await evaluate("(()=>{const r=document.getElementById('start').getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()");
    const messageStart=messages.length;
    for(const type of ['mousePressed','mouseReleased'])await command('Input.dispatchMouseEvent',{type,...position,button:'left',clickCount:1});
    await until(()=>messages.slice(messageStart).some(text=>/Player presentation ready/u.test(text)),'imported profile boot');
    assert.ok(!messages.slice(messageStart).some(text=>/PXPERSIST600[167]/u.test(text)),'encrypted profile could not be imported');
    const visible=label=>`[...document.querySelectorAll('#semantics [aria-label]')].find(e=>e.getAttribute('aria-label')===${JSON.stringify(label)}&&!e.closest('[hidden]'))`;
    const activate=async label=>{
      await until(()=>evaluate(`!!(${visible(label)})`),`imported ported game control ${label}`);
      assert.equal(await evaluate(`(()=>{const e=${visible(label)};if(e.getAttribute('aria-disabled')==='true')return false;e.click();return true})()`),true);
      await new Promise(resolve=>setTimeout(resolve,400));
    };
    await activate('鑑賞');await activate('CG鑑賞');
    await until(()=>messages.slice(messageStart).some(text=>/gallery presented total=1139 unlocked=1139/u.test(text)),'unlocked CG gallery');
    await activate('CGを表示');
    await new Promise(resolve=>setTimeout(resolve,1000));
    const cg=await command('Page.captureScreenshot',{format:'png'});await writeFile(join(qaRoot,'gallery-cg.png'),Buffer.from(cg.data,'base64'));
    for(const type of ['mousePressed','mouseReleased'])await command('Input.dispatchMouseEvent',{type,x:640,y:360,button:'right',clickCount:1});
    await new Promise(resolve=>setTimeout(resolve,400));
    await until(()=>evaluate(`!!(${visible('鑑賞')})||!!(${visible('BGM鑑賞')})`),'Ported game extras return');
    if(!await evaluate(`!!(${visible('BGM鑑賞')})`))await activate('鑑賞');
    await activate('BGM鑑賞');await activate('bgm001');
    await until(()=>messages.slice(messageStart).some(text=>/music gallery selected path=.*bgm001/u.test(text)),'music gallery playback');
    await activate('stop');await activate('Scene');await activate('scene01');
    await until(()=>messages.slice(messageStart).some(text=>/Player started story entry label=scene01/u.test(text)),'scene replay');
    console.log('Ported game: encrypted profile import, CG gallery, music and replay passed');
  }
  if (process.env.PRISMATIX_WEB_TEST_SAVES === "1") {
    await evaluate(`(async()=>{const create=URL.createObjectURL;URL.createObjectURL=function(blob){globalThis.testSaveBlob=blob;return create.call(this,blob)};
      await document.getElementById('export-saves').onclick();URL.createObjectURL=create;})()`);
    const data=await evaluate("testSaveBlob.text()");
    const saved=JSON.parse(data);assert.equal(saved.format,'PrismatiXWebSaves');assert.ok(saved.files.length>0);
    assert.ok(saved.files.some(file=>file.path.endsWith('/profile.dat')));
    await writeFile(join(qaRoot,'roundtrip.pxwebsave'),data);
    const before=pageLoads;
    await evaluate(`(()=>{const input=document.getElementById('import-saves'),files=new DataTransfer();files.items.add(new File([${JSON.stringify(data)}],'roundtrip.pxwebsave'));input.files=files.files;input.dispatchEvent(new Event('change'));})()`);
    await until(()=>pageLoads>before,'roundtrip import reload');
    await until(()=>evaluate("document.getElementById('start')?.disabled===false"),'roundtrip import initialization');
    const position=await evaluate("(()=>{const r=document.getElementById('start').getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()");
    const messageStart=messages.length;
    for(const type of ['mousePressed','mouseReleased'])await command('Input.dispatchMouseEvent',{type,...position,button:'left',clickCount:1});
    await until(()=>messages.slice(messageStart).some(text=>/Player presentation ready/u.test(text)),'roundtrip engine boot');
    assert.ok(!messages.slice(messageStart).some(text=>/PXPERSIST600[167]/u.test(text)),'roundtrip persistence failed');
    console.log('Web: encrypted save export/import roundtrip passed');
  }
  if (process.env.PRISMATIX_WEB_TEST_UPDATE === "1") {
    const original=JSON.parse(await readFile(join(root,'web-export.json'),'utf8'));
    const exportId=createHash('sha256').update(original.exportId+'browser-update-test').digest('hex');
    overrides.set('/web-export.json',Buffer.from(JSON.stringify({...original,exportId})));
    overrides.set('/sw.mjs',Buffer.from((await readFile(join(root,'sw.mjs'),'utf8')).replaceAll(original.exportId,exportId)));
    await evaluate("(async()=>{const registration=await navigator.serviceWorker.getRegistration();await registration.update()})()");
    await until(()=>evaluate("!document.getElementById('update').hidden"),'waiting Web update');
    assert.equal(await evaluate("(async()=>{return(await(await fetch('./web-export.json')).json()).exportId})()"),original.exportId,'running game changed before update approval');
    const before=pageLoads;await evaluate("document.getElementById('update').click()");
    await until(()=>pageLoads>before,'approved update reload');
    await until(()=>evaluate("document.getElementById('start')?.disabled===false"),'updated Player initialization');
    assert.equal(await evaluate("(async()=>{return(await(await fetch('./web-export.json')).json()).exportId})()"),exportId);
    console.log('Web: active game retained its version and accepted update passed');
  }
  if (process.env.PRISMATIX_WEB_TEST_OFFLINE === "1") {
    console.log("Web: downloading the complete export for offline checks");
    await evaluate("document.getElementById('download').click()");
    await until(async()=>{
      const status=await evaluate("document.getElementById('download-status').textContent");
      if(status.includes('尚未完成'))throw new Error(status);
      return status.includes('可離線遊玩');
    },"complete offline download",900000);
    await command("Network.enable");
    await command("Network.emulateNetworkConditions",{offline:true,latency:0,downloadThroughput:0,uploadThroughput:0});
    const offline=await evaluate(`(async()=>{
      const manifest=await(await fetch('./web-export.json')).json();
      const videos=[];
      for(const path of Object.values(manifest.videos)){
        const video=document.createElement('video');video.muted=true;video.playsInline=true;video.src=new URL(path,location.href);
        await new Promise((resolve,reject)=>{video.onloadedmetadata=resolve;video.onerror=()=>reject(new Error(video.error?.message));});
        const duration=video.duration;video.currentTime=Math.max(.1,duration*.8);
        await new Promise((resolve,reject)=>{video.onseeked=resolve;video.onerror=()=>reject(new Error(video.error?.message));});
        if(video.readyState<2)await new Promise((resolve,reject)=>{video.onloadeddata=resolve;video.onerror=()=>reject(new Error(video.error?.message));});
        const canvas=document.createElement('canvas');canvas.width=video.videoWidth;canvas.height=video.videoHeight;
        const context=canvas.getContext('2d');context.drawImage(video,0,0);
        const bytes=context.getImageData(0,0,canvas.width,canvas.height).data;
        if(!bytes.some((value,index)=>index%4!==3&&value>0))throw new Error('Offline video decoded no color');
        videos.push({path,duration,width:canvas.width,height:canvas.height,seek:video.currentTime});video.removeAttribute('src');video.load();
      }return{videos};
    })()`);
    await writeFile(join(qaRoot,"offline-result.json"),JSON.stringify(offline,null,2));
    const before=messages.length;await reloadPage();
    const position=await evaluate("(()=>{const r=document.getElementById('start').getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()");
    for(const type of ['mousePressed','mouseReleased'])await command('Input.dispatchMouseEvent',{type,...position,button:'left',clickCount:1});
    await until(()=>messages.slice(before).some(text=>/Player presentation ready/u.test(text)),"offline engine boot");
    console.log("Web: complete offline download, video seeking and Player reload passed");
  }
  if (process.env.PRISMATIX_WEB_TEST_STARTER === "1") {
    // The standard minimal starter has Exit at these authored coordinates.
    for (const type of ["mousePressed", "mouseReleased"])
      await command("Input.dispatchMouseEvent", {type, x: 260, y: 485, button: "left", clickCount: 1});
    await until(() => evaluate("document.getElementById('welcome').hidden === false"), "Player shutdown");
    const persisted = await evaluate(`(async()=>{
      const database=await new Promise((resolve,reject)=>{
        const request=indexedDB.open('/persist');request.onsuccess=()=>resolve(request.result);request.onerror=()=>reject(request.error);
      });
      const keys=await new Promise((resolve,reject)=>{
        const request=database.transaction('FILE_DATA').objectStore('FILE_DATA').getAllKeys();
        request.onsuccess=()=>resolve(request.result);request.onerror=()=>reject(request.error);
      });database.close();return keys;
    })()`);
    assert.ok(persisted.some(path => path.endsWith("/Save/config.dat")), "settings were not persisted to IndexedDB");
    assert.ok(persisted.some(path => path.endsWith("/Save/profile.dat")), "profile was not persisted to IndexedDB");
    await reloadPage();
    await until(() => evaluate("document.getElementById('start')?.disabled === false"), "persistent Player reload");
    const next = await evaluate("(()=>{const r=document.getElementById('start').getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()");
    const before = messages.length;
    for (const type of ["mousePressed", "mouseReleased"])
      await command("Input.dispatchMouseEvent", {type, ...next, button: "left", clickCount: 1});
    await until(() => messages.slice(before).some(text => /Player presentation ready/u.test(text)), "Player with persisted profile");
    assert.ok(!messages.slice(before).some(text => /PXPERSIST600[167]/u.test(text)), "persisted data could not be read");
  }
  assert.deepEqual(exceptions, [], "Player raised an uncaught browser exception");
  console.log(`Web Player browser boot passed: ${root}`);
} finally {
  await writeFile(join(qaRoot, "browser-last.log"), messages.join("\n"));
  await closeBrowser?.();
  socket?.close(); browser.kill();
  await new Promise(resolve => server.close(resolve));
  await rm(profile, {recursive: true, force: true, maxRetries: 10, retryDelay: 300}).catch(error=>console.error(`QA profile cleanup failed: ${error.message}`));
}
