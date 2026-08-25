import assert from "node:assert/strict";
import test from "node:test";

import {
  canonicalJson,
  contractHashes,
  migrateDocument,
  parseDocument,
  serializeDocument,
  validateDocument,
  validateUiDocument,
} from "../src/index.js";
import type {JsonValue} from "../src/index.js";

test("contract schemas have deterministic SHA-256 identities", () => {
  assert.ok(Object.keys(contractHashes).length >= 12);
  for (const hash of Object.values(contractHashes)) assert.match(hash, /^[0-9a-f]{64}$/u);
});

test("canonical JSON sorts every object level deterministically", () => {
  const value = {z: 1, a: {z: false, a: [3, {b: 2, a: 1}]}} satisfies JsonValue;
  assert.equal(canonicalJson(value), '{\n  "a": {\n    "a": [\n      3,\n      {\n        "a": 1,\n        "b": 2\n      }\n    ],\n    "z": false\n  },\n  "z": 1\n}\n');
  assert.equal(canonicalJson(value), canonicalJson(value));
});

test("project contract is strict, bounded, and path-safe", () => {
  const project = {
    format: "PrismatiXProject",
    schemaRevision: 1,
    id: "rain-demo",
    name: "Rain Demo",
    resolution: {width: 1280, height: 720},
    defaultLocale: "zh-TW",
    supportedLocales: ["zh-TW", "ja-JP"],
    storyIndex: "Story/story.pxindex",
    gameCatalog: "Content/game.pxgame",
    extensions: ["Content/Extensions/weather.pxextension"],
  } satisfies JsonValue;
  assert.equal(validateDocument("project", project).valid, true);
  assert.equal(validateDocument("project", {...project, editorPanel: {x: 10}}).valid, false);
  assert.equal(validateDocument("project", {...project, storyIndex: "../outside.pxindex"}).valid, false);
  assert.equal(parseDocument("project", `\uFEFF${JSON.stringify(project)}`).valid, false);
  assert.equal(parseDocument("project", "x".repeat(16 * 1024 * 1024 + 1)).valid, false);
  const serialized = serializeDocument("project", project);
  assert.equal(serialized.valid, true);
  assert.equal(parseDocument("project", serialized.value!).valid, true);
});

test("all canonical authored format schemas accept representative documents", () => {
  const uuid = "11111111-1111-4111-8111-111111111111";
  const uuid2 = "22222222-2222-4222-8222-222222222222";
  const documents: ReadonlyArray<readonly [string, JsonValue]> = [
    ["character", {format:"PrismatiXCharacter",schemaRevision:1,id:uuid,displayName:"Yuki",aliases:["yuki"],defaultExpressionId:uuid2,expressions:[{id:uuid2,name:"Smile",aliases:["smile"],assetId:uuid}]}],
    ["storyIndex", {format:"PrismatiXStoryIndex",schemaRevision:1,id:"main",entryScene:"chapter01.scene01",chapters:[{id:"chapter01",title:"Chapter 1",scenes:["chapter01.scene01"]}],scenes:[{id:"chapter01.scene01",sources:{"zh-TW":"Story/zh-TW/ch01/scene01.pxstory","ja-JP":"Story/ja-JP/ch01/scene01.pxstory"},requiredLabels:["start"]}]}],
    ["game", {format:"PrismatiXGame",schemaRevision:1,variables:[{name:"affection",type:"integer",default:0,persistent:true}],progressionFlags:["route.yuki"],gallery:[],unlockables:[]}],
    ["extension", {format:"PrismatiXExtension",schemaRevision:1,language:"javascript",id:"weather",version:"1.0.0",entry:"weather.js",capabilities:["runtime"],safety:{previewSafe:true,deterministic:true,seekSafe:true,rollbackSafe:true},commands:[{id:"weather",parameters:[{name:"type",type:"string",required:true,enum:["rain","snow"],editorHint:"enum"},{name:"strength",type:"number",range:{minimum:0,maximum:1}}]}],actions:[]}],
    ["timeline", {format:"PrismatiXTimeline",schemaRevision:1,id:"intro",duration:5,tracks:[{id:"move",binding:{kind:"stage",target:"yuki",property:"position.x"},keyframes:[{time:0,value:0,easing:"linear"},{time:5,value:100,easing:"easeOut"}]}],markers:[{id:"bell",time:2,name:"bell",payload:{}}],nestedClips:[]}],
    ["animation", {format:"PrismatiXAnimation",schemaRevision:1,version:4,id:uuid,name:"Fade",duration:1,loop:false,tracks:[],markers:[],nested:[]}],
    ["locale", {format:"PrismatiXLocale",schemaRevision:1,locale:"zh-TW",strings:{"ui.settings":"設定"}}],
    ["sourceMap", {format:"PrismatiXSourceMap",schemaRevision:1,documentId:"scene",mappings:[{operationId:"op-1",sourceId:"source-1",sourceUri:"Story/zh-TW/scene.pxstory",startLine:1,startColumn:1,endLine:1,endColumn:5}]}],
  ];
  for (const [contract, document] of documents) {
    const result = validateDocument(contract, document);
    assert.equal(result.valid, true, `${contract}: ${JSON.stringify(result.diagnostics)}`);
  }
});

test("legacy version-4 animation migration adds schemaRevision explicitly", () => {
  const legacy = {format:"PrismatiXAnimation",version:4,id:"11111111-1111-4111-8111-111111111111",name:"Fade",duration:1,loop:false,tracks:[],markers:[],nested:[]};
  const migrated = migrateDocument("animation", legacy);
  assert.equal(migrated.valid, true);
  assert.equal((migrated.value as {schemaRevision: number}).schemaRevision, 1);
  assert.equal(migrateDocument("animation", {...legacy, schemaRevision: 99}).valid, false);
});

test("UI contract models simultaneous Visual State Groups through shared property metadata", () => {
  const root = "11111111-1111-4111-8111-111111111111";
  const button = "22222222-2222-4222-8222-222222222222";
  const ui = {
    format:"PrismatiXUIScene",schemaRevision:2,id:"33333333-3333-4333-8333-333333333333",revision:1,name:"Menu",width:1280,height:720,rootId:root,
    nodes:[
      {id:root,parentId:null,order:0,kind:"control",runtimeType:"Control",name:"Root",visible:true,locked:false,layout:{mode:"free",x:0,y:0,width:1280,height:720,anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0,pivotY:0,margin:0,alignment:"start",sizeRule:"fixed"},runtimeProperties:{},bindings:{}},
      {id:button,parentId:root,order:0,kind:"button",runtimeType:"Button",name:"Start",visible:true,locked:false,layout:{mode:"free",x:100,y:100,width:240,height:80,anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0.5,pivotY:0.5,margin:0,alignment:"center",sizeRule:"fixed"},runtimeProperties:{texture:"Assets/UI/start-normal.png",opacity:1},bindings:{text:{path:"locale.ui.start"}}},
    ],
    theme:[],behaviorGraph:{nodes:[],links:[],groups:[]},behaviorTriggers:[],
    visualStateGroups:[
      {id:"interaction",defaultState:"normal",states:[{id:"normal",overrides:[{nodeId:button,property:"texture",value:"Assets/UI/start-normal.png"}]},{id:"hover",overrides:[{nodeId:button,property:"texture",value:"Assets/UI/start-hover.png"}]}],transitions:[{from:"normal",to:"hover",duration:0.12,easing:"easeOut"}]},
      {id:"focus",defaultState:"unfocused",states:[{id:"unfocused",overrides:[]},{id:"focused",overrides:[{nodeId:button,property:"opacity",value:0.9}]}],transitions:[]},
    ],
  } satisfies JsonValue;
  const registry = {controls:[
    {runtimeType:"Control",properties:[]},
    {runtimeType:"Button",properties:[
      {id:"texture",writable:true,bindable:false,animatable:true},
      {id:"opacity",writable:true,bindable:true,animatable:true},
      {id:"text",writable:true,bindable:true,animatable:false},
    ]},
  ]};
  assert.equal(validateUiDocument(ui, registry).valid, true);
  const invalid = structuredClone(ui) as typeof ui;
  (invalid.visualStateGroups[0]!.states[0]!.overrides[0] as {property: string}).property = "removedProperty";
  assert.equal(validateUiDocument(invalid, registry).valid, false);
});

test("UI revision-1 migration normalizes legacy presentation fields", () => {
  const root = "11111111-1111-4111-8111-111111111111";
  const legacy = {
    format:"PrismatiXUIScene",schemaRevision:1,id:"33333333-3333-4333-8333-333333333333",revision:1,name:"Legacy",width:1280,height:720,rootId:root,
    nodes:[{id:root,parentId:null,order:0,kind:"control",name:"Root",visible:true,locked:false,layout:{mode:"free",x:0,y:0,width:1280,height:720,anchorX:0,anchorY:0,pivotX:0,pivotY:0,margin:0,alignment:"start",sizeRule:"fixed"},content:{text:"",assetId:null},appearance:{backgroundColor:"#000000",textColor:"#ffffff",opacity:1,styleToken:null},interaction:{onClick:null},accessibility:{label:"Root",role:"presentation"}}],
    theme:[],behaviorGraph:{nodes:[],links:[],groups:[]},behaviorTriggers:[],
  };
  const migrated = migrateDocument("ui", legacy);
  assert.equal(migrated.valid, true, JSON.stringify(migrated.diagnostics));
  assert.equal((migrated.value as {schemaRevision: number}).schemaRevision, 2);
});

test("Timeline semantic validation rejects unordered or out-of-range samples", () => {
  const invalid = {format:"PrismatiXTimeline",schemaRevision:1,id:"bad",duration:1,tracks:[{id:"track",binding:{kind:"stage",target:"yuki",property:"opacity"},keyframes:[{time:0.8,value:1,easing:"linear"},{time:0.4,value:0,easing:"linear"}]}],markers:[],nestedClips:[]} satisfies JsonValue;
  const result = validateDocument("timeline", invalid);
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((item) => item.code === "PXSDKSEM1201"));
});
