import assert from "node:assert/strict";
import test from "node:test";

import {
  canonicalJson,
  contractHashes,
  migrateDocument,
  parseDocument,
  serializeDocument,
  validateDocument,
  validateSemantics,
  validateUiDocument,
} from "../src/index.js";
import type {JsonValue} from "../src/index.js";
import {nativeUiValueType} from "../src/ui-value.js";

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
    schemaRevision: 2,
    id: "rain-demo",
    name: "Rain Demo",
    version: "0.2.0",
    contentVersion: "chapter-1",
    saveVersion: 1,
    resolution: {width: 1280, height: 720},
    entry: {story: "chapter01.scene01", ui: "title"},
    defaultLocale: "zh-TW",
    supportedLocales: ["zh-TW", "ja-JP"],
    storyIndex: "Story/story.pxindex",
    gameCatalog: "Content/game.pxgame",
    extensions: ["Content/Extensions/weather.pxextension"],
    uiEntryPoints: {title: "Content/UI/Title.pxui"},
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

test("character localized display names require valid locale tags and nonempty text", () => {
  const character = {format: "PrismatiXCharacter", schemaRevision: 2,
    id: "11111111-1111-4111-8111-111111111111", displayName: "雪",
    displayNames: {"ja-JP": "ゆき", "zh-TW": "雪"},
    aliases: ["yuki"], expressions: []};
  assert.equal(validateDocument("character", character).valid, true);
  assert.equal(validateDocument("character", {...character,
    displayNames: {"ja-JP": ""}}).valid, false);
  assert.equal(validateDocument("character", {...character,
    displayNames: {"not a locale": "Yuki"}}).valid, false);
});

test("all canonical authored format schemas accept representative documents", () => {
  const uuid = "11111111-1111-4111-8111-111111111111";
  const uuid2 = "22222222-2222-4222-8222-222222222222";
  const documents: ReadonlyArray<readonly [string, JsonValue]> = [
    ["character", {format:"PrismatiXCharacter",schemaRevision:2,id:uuid,displayName:"Yuki",aliases:["yuki"],defaultExpressionId:uuid2,expressions:[{id:uuid2,name:"Smile",aliases:["smile"],assetId:uuid}]}],
    ["storyIndex", {format:"PrismatiXStoryIndex",schemaRevision:2,id:"main",entryScene:"chapter01.scene01",chapters:[{id:"chapter01",title:"Chapter 1",scenes:["chapter01.scene01"]}],scenes:[{id:"chapter01.scene01",sources:{"zh-TW":"Story/zh-TW/ch01/scene01.pxstory","ja-JP":"Story/ja-JP/ch01/scene01.pxstory"},requiredLabels:["start"]}]}],
    ["game", {format:"PrismatiXGame",schemaRevision:2,variables:[{name:"affection",type:"integer",default:0,scope:"profile"}],progressionFlags:["route.yuki"],gallery:[],unlockables:[]}],
    ["extension", {format:"PrismatiXExtension",schemaRevision:2,language:"javascript",id:"weather",version:"1.0.0",requiredEngineVersion:">=0.2.0 <0.3.0",entry:"weather.js",capabilities:["runtime"],safety:{previewSafe:true,deterministic:true,seekSafe:true,rollbackSafe:true},commands:[{id:"weather",parameters:[{name:"type",type:"string",required:true,enum:["rain","snow"],editorHint:"enum"},{name:"strength",type:"number",range:{minimum:0,maximum:1}}]}],actions:[]}],
    ["timeline", {format:"PrismatiXTimeline",schemaRevision:2,id:"intro",duration:5,tracks:[{id:"move",binding:{kind:"stage",target:"yuki",property:"position.x"},keyframes:[{time:0,value:0,easing:"linear"},{time:5,value:100,easing:"easeOut"}]}],markers:[{id:"bell",time:2,name:"bell",payload:{}}],nestedClips:[]}],
    ["animation", {format:"PrismatiXAnimation",schemaRevision:2,version:4,id:uuid,name:"Fade",duration:1,loop:false,tracks:[],markers:[],nested:[]}],
    ["motion", {format:"PrismatiXMotion",schemaRevision:1,id:"fade",canvas:{width:1280,height:720},duration:1,loop:false,layers:[{id:"logo",image:"Assets/logo.png",width:320,height:120}],tracks:[]}],
    ["locale", {format:"PrismatiXLocale",schemaRevision:2,locale:"zh-TW",fontChain:["Content/Fonts/NotoSansTC.ttf"],strings:{"ui.settings":"設定"}}],
    ["sourceMap", {format:"PrismatiXSourceMap",schemaRevision:2,documentId:"scene",mappings:[{operationId:"op-1",sourceId:"source-1",sourceUri:"Story/zh-TW/scene.pxstory",startLine:1,startColumn:1,endLine:1,endColumn:5}]}],
    ["effect", {format:"PrismatiXEffect",schemaRevision:2,id:"dream-tone",targetLayer:"stage",shader:"Effects/dream-tone.frag.hlsl",uniforms:[{name:"amount",type:"number",slot:0,default:0.5,minimum:0,maximum:1}]}],
    ["uiComponent", {format:"PrismatiXUIComponent",schemaRevision:2,id:uuid,revision:1,name:"Panel",width:320,height:180,rootId:uuid,nodes:[{id:uuid,parentId:null,order:0,kind:"group",runtimeType:"Panel",name:"Root",visible:true,locked:false,layout:{mode:"free",x:0,y:0,width:320,height:180,anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0,pivotY:0,margin:0,alignment:"start",sizeRule:"fixed"},accessibilityLabel:"Panel",accessibilityRole:"group",runtimeProperties:{},bindings:{}}],theme:[],componentInterface:{properties:[],signals:[],slots:[]}}],
  ];
  for (const [contract, document] of documents) {
    const result = validateDocument(contract, document);
    assert.equal(result.valid, true, `${contract}: ${JSON.stringify(result.diagnostics)}`);
  }
});

test("game input actions declare contexts and default keys without legacy key commands", () => {
  const game = {format: "PrismatiXGame", schemaRevision: 2,
    variables: [], gallery: [], unlockables: [],
    inputActions: [
      {id: "quick-save", context: "game", keys: ["F8"]},
      {id: "screen.open.gallery", context: "game", keys: ["G"], route: "gallery"},
      {id: "camera.photo", context: "game"},
    ]};
  assert.equal(validateDocument("game", game).valid, true);
  assert.equal(validateDocument("game", {...game, inputActions: [
    {id: "quick-save", context: "game", keys: ["F8", "F8"]},
  ]}).valid, false);
  assert.equal(validateDocument("game", {...game, inputBindings: [
    {key: "G", command: "screen.open", argument: "gallery"},
  ]}).diagnostics.some((item) => item.code === "PXSDKSEM1121"), true);
  assert.equal(validateDocument("game", {...game, inputActions: [
    {id: "screen.open.gallery", context: "ui", keys: ["G"], route: "gallery"},
  ]}).diagnostics.some((item) => item.code === "PXSDKSEM1122"), true);
});

test("legacy version-4 animation migration adds schemaRevision explicitly", () => {
  const legacy = {format:"PrismatiXAnimation",version:4,id:"11111111-1111-4111-8111-111111111111",name:"Fade",duration:1,loop:false,tracks:[],markers:[],nested:[]};
  const migrated = migrateDocument("animation", legacy);
  assert.equal(migrated.valid, true);
  assert.equal((migrated.value as {schemaRevision: number}).schemaRevision, 2);
  assert.equal(migrateDocument("animation", {...legacy, schemaRevision: 99}).valid, false);
});

test("UI contract models simultaneous Visual State Groups through shared property metadata", () => {
  const root = "11111111-1111-4111-8111-111111111111";
  const button = "22222222-2222-4222-8222-222222222222";
  const ui = {
    format:"PrismatiXUIScene",schemaRevision:2,id:"33333333-3333-4333-8333-333333333333",revision:1,name:"Menu",width:1280,height:720,rootId:root,
    nodes:[
      {id:root,parentId:null,order:0,kind:"control",runtimeType:"Control",name:"Root",visible:true,locked:false,layout:{mode:"free",x:0,y:0,width:1280,height:720,anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0,pivotY:0,margin:0,alignment:"start",sizeRule:"fixed"},runtimeProperties:{},bindings:{}},
      {id:button,parentId:root,order:0,kind:"button",runtimeType:"Button",name:"Start",visible:true,locked:false,layout:{mode:"free",x:100,y:100,width:240,height:80,anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0.5,pivotY:0.5,margin:0,alignment:"center",sizeRule:"fixed"},runtimeProperties:{texture:"Assets/UI/start-normal.png",opacity:1,outlineColor:{type:"color",value:"#2D171DFF"}},bindings:{text:{path:"locale.ui.start"}}},
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
      {id:"texture",writable:true,bindable:false,animatable:true,valueType:"string"},
      {id:"opacity",writable:true,bindable:true,animatable:true,valueType:"number"},
      {id:"outlineColor",writable:true,bindable:false,animatable:false,valueType:"color"},
      {id:"text",writable:true,bindable:true,animatable:false},
    ]},
  ]};
  assert.equal(validateUiDocument(ui, registry).valid, true);
  const invalid = structuredClone(ui) as typeof ui;
  (invalid.visualStateGroups[0]!.states[0]!.overrides[0] as {property: string}).property = "removedProperty";
  assert.equal(validateUiDocument(invalid, registry).valid, false);
  const wrongColor = structuredClone(ui) as typeof ui;
  (wrongColor.nodes[1]!.runtimeProperties as {outlineColor: unknown}).outlineColor = "#2D171DFF";
  const rejected = validateUiDocument(wrongColor, registry);
  assert.equal(rejected.valid, false);
  assert.ok(rejected.diagnostics.some((item) => item.code === "PXSDKSEM1324" && item.message.includes("expects color")));
  const malformedColor = structuredClone(ui) as typeof ui;
  (malformedColor.nodes[1]!.runtimeProperties as {outlineColor: unknown}).outlineColor = {type:"color",value:"pink"};
  assert.ok(validateUiDocument(malformedColor, registry).diagnostics.some((item) =>
    item.code === "PXSDKSEM1325" && item.details === "nodes[1].runtimeProperties.outlineColor"));
});

test("UI revision-1 migration normalizes legacy presentation fields", () => {
  const root = "11111111-1111-4111-8111-111111111111";
  const sourceNode = "22222222-2222-4222-8222-222222222222";
  const legacy = {
    format:"PrismatiXUIScene",schemaRevision:1,id:"33333333-3333-4333-8333-333333333333",revision:1,name:"Legacy",width:1280,height:720,rootId:root,
    nodes:[{id:root,parentId:null,order:0,kind:"control",name:"Root",visible:true,locked:false,layout:{mode:"free",x:0,y:0,width:1280,height:720,anchorX:0,anchorY:0,pivotX:0,pivotY:0,margin:0,alignment:"start",sizeRule:"fixed"},content:{text:"",assetId:null},appearance:{backgroundColor:"#000000",textColor:"#ffffff",opacity:1,styleToken:null,hoverBackgroundColor:"#111111",focusColor:"#222222",disabledOpacity:0.4},interaction:{onClick:null},accessibility:{label:"Root",role:"presentation",description:"Legacy root",focusOrder:7},componentInstance:{componentId:sourceNode,instanceRootId:root,sourceNodeId:sourceNode,sourcePath:[sourceNode,sourceNode],overrides:[],publicProperties:{caption:"Title"},publicSignals:{}},componentSlot:{instanceRootId:root,slotId:"content"}}],
    theme:[],behaviorGraph:{nodes:[],links:[],groups:[]},behaviorTriggers:[],
  };
  const migrated = migrateDocument("ui", legacy);
  assert.equal(migrated.valid, true, JSON.stringify(migrated.diagnostics));
  const value = migrated.value as {schemaRevision: number; nodes: Array<Record<string, unknown>>};
  assert.equal(value.schemaRevision, 2);
  assert.deepEqual(value.nodes[0]?.componentInstance, legacy.nodes[0]?.componentInstance);
  assert.deepEqual(value.nodes[0]?.componentSlot, legacy.nodes[0]?.componentSlot);
  assert.deepEqual(value.nodes[0]?.appearance, legacy.nodes[0]?.appearance);
  assert.equal(value.nodes[0]?.accessibilityDescription, "Legacy root");
  assert.equal(value.nodes[0]?.accessibilityFocusOrder, 7);
});

test("UI component contract rejects scene-only fields and migrates canonical property paths", () => {
  const root = "11111111-1111-4111-8111-111111111111";
  const legacy = {
    format:"PrismatiXUIComponent",schemaRevision:1,id:root,revision:1,name:"Panel",width:320,height:180,rootId:root,
    nodes:[{id:root,parentId:null,order:0,kind:"group",name:"Root",visible:true,locked:false,layout:{mode:"free",x:0,y:0,width:320,height:180,anchorX:0,anchorY:0,pivotX:0,pivotY:0,margin:0,alignment:"start",sizeRule:"fixed"},content:{text:"Panel",assetId:null},appearance:{backgroundColor:"#000000",textColor:"#ffffff",opacity:1,styleToken:null},interaction:{onClick:null},accessibility:{label:"Panel",role:"group"}}],theme:[],
    componentInterface:{properties:[{id:"caption",displayName:"Caption",nodeId:root,property:"content.text",valueType:"string",defaultValue:"Panel"}],signals:[],slots:[]},
  };
  const migrated = migrateDocument("uiComponent", legacy);
  assert.equal(migrated.valid, true, JSON.stringify(migrated.diagnostics));
  const value = migrated.value as {componentInterface: {properties: {property: string}[]}};
  assert.equal(value.componentInterface.properties[0]!.property, "text");
  assert.equal(validateDocument("uiComponent", {...migrated.value as object, behaviorGraph:{nodes:[],links:[],groups:[]}}).valid, false);
});

test("UI component defaults and metadata use native JSON capacity limits", () => {
  const id = "11111111-1111-4111-8111-111111111111";
  const layout = {mode:"free",x:0,y:0,width:100,height:100,
    anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0,pivotY:0,
    margin:0,alignment:"start",sizeRule:"fixed"};
  const component = {format:"PrismatiXUIComponent",schemaRevision:2,id,revision:1,
    name:"Panel",width:100,height:100,rootId:id,
    nodes:[{id,parentId:null,order:0,kind:"group",runtimeType:"Control",
      name:"Root",visible:true,locked:false,layout,
      runtimeProperties:{payload:{small:1}},bindings:{}}],theme:[],
    componentInterface:{properties:[{id:"payload",displayName:"Payload",nodeId:id,
      property:"runtimeProperties.payload",valueType:"object",defaultValue:{small:1}}],
    signals:[],slots:[]}};
  assert.equal(validateDocument("uiComponent", component).valid, true);
  const largeDefault = structuredClone(component) as typeof component;
  (largeDefault.componentInterface.properties[0] as {defaultValue: unknown}).defaultValue =
    {value:"x".repeat(1024 * 1024)};
  const defaultResult = validateDocument("uiComponent", largeDefault);
  assert.equal(defaultResult.valid, false);
  assert.ok(defaultResult.diagnostics.some((item) => item.code === "PXSDKUICOMP1105" &&
    item.details === "payload"));
  const longKey = "界".repeat(86);
  for (const location of ["componentInterface", "properties", "signals", "slots"] as const) {
    const invalid = structuredClone(component) as typeof component;
    const interface_ = invalid.componentInterface as Record<string, unknown>;
    if (location === "componentInterface") interface_.metadata = {[longKey]:1};
    else if (location === "properties") (interface_.properties as Record<string, unknown>[])[0]!.metadata = {[longKey]:1};
    else if (location === "signals") interface_.signals = [{id:"pressed",displayName:"Pressed",
      nodeId:id,signal:"clicked",arguments:[],metadata:{[longKey]:1}}];
    else interface_.slots = [{id:"content",displayName:"Content",nodeId:id,
      metadata:{[longKey]:1}}];
    const result = validateDocument("uiComponent", invalid);
    assert.equal(result.valid, false, location);
    assert.ok(result.diagnostics.some((item) => item.code === "PXSDKUICOMP1108" &&
      item.details?.includes(location === "componentInterface" ? "componentInterface.metadata" :
        `componentInterface.${location}[0].metadata`)), location);
  }
});

test("UI semantic validation matches native built-in Action argument contracts", () => {
  const root = "11111111-1111-4111-8111-111111111111";
  const button = "22222222-2222-4222-8222-222222222222";
  const layout = {mode:"free",x:0,y:0,width:200,height:60,
    anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0,pivotY:0,
    margin:0,alignment:"start",sizeRule:"fixed"};
  const scene = {format:"PrismatiXUIScene",schemaRevision:2,id:root,revision:1,
    name:"Save",width:1280,height:720,rootId:root,
    nodes:[
      {id:root,parentId:null,order:0,kind:"group",runtimeType:"Control",name:"Root",
        visible:true,locked:false,layout,runtimeProperties:{},bindings:{}},
      {id:button,parentId:root,order:0,kind:"button",runtimeType:"TextureButton",
        name:"Slot0Button",visible:true,locked:false,layout,runtimeProperties:{},
        bindings:{},onClick:{id:"save.slot",arguments:{slot:0}}},
    ],theme:[],behaviorGraph:{nodes:[],links:[],groups:[]},
    behaviorTriggers:[],visualStateGroups:[],
  } satisfies JsonValue;
  assert.equal(validateDocument("ui", scene).valid, true);
  const invalid = structuredClone(scene) as {
    nodes: Array<{onClick?: {id: string; arguments: Record<string, JsonValue>}}>;
  };
  invalid.nodes[1]!.onClick = {id:"save.slot",arguments:{argument:"0"}};
  const result = validateDocument("ui", invalid);
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((item) => item.code === "PXSDKUI1025"));
});

test("UI binding and runtime-property limits match the native loader", () => {
  const id = "11111111-1111-4111-8111-111111111111";
  const layout = {mode:"free",x:0,y:0,width:100,height:100,
    anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0,pivotY:0,
    margin:0,alignment:"start",sizeRule:"fixed"};
  const node = {id,parentId:null,order:0,kind:"control",runtimeType:"Control",
    name:"Root",visible:true,locked:false,layout,runtimeProperties:{},
    bindings:{visible:{path:"saveLoad.slot0.enabled"}}};
  const scene = {format:"PrismatiXUIScene",schemaRevision:2,id,revision:1,
    name:"Bindings",width:100,height:100,rootId:id,nodes:[node],theme:[],
    behaviorGraph:{nodes:[],links:[],groups:[]},behaviorTriggers:[],
    visualStateGroups:[]};
  const withMaps = (runtimeProperties: Record<string, JsonValue>,
    bindings: Record<string, JsonValue>) => ({...scene,
      nodes:[{...node,runtimeProperties,bindings}]});
  assert.equal(validateDocument("ui", scene).valid, true);
  assert.equal(validateDocument("ui", withMaps({},
    {visible:{path:"saveLoad.slots[0].enabled"}})).valid, true);
  assert.equal(validateDocument("ui", withMaps({},
    {visible:{path:"saveLoad.slots[00].enabled"}})).valid, false);
  assert.equal(validateDocument("ui", withMaps({},
    {visible:{path:"saveLoad.slots[0].enabled["}})).valid, false);
  assert.equal(validateDocument("ui", withMaps({},
    {"bad key":{path:"saveLoad.slot0.enabled"}})).valid, false);
  assert.equal(validateDocument("ui", withMaps({},
    {visible:{path:"saveLoad.slot0.enabled",formatter:"bad formatter"}})).valid, false);
  assert.equal(validateDocument("ui", withMaps({}, Object.fromEntries(
    Array.from({length:257}, (_, index) => [`binding${index}`,
      {path:"saveLoad.slot0.enabled"}])))).valid, false);
  assert.equal(validateDocument("ui", withMaps(Object.fromEntries(
    Array.from({length:257}, (_, index) => [`property${index}`, index])), {})).valid, false);
  assert.equal(validateDocument("ui", withMaps({"":1}, {})).valid, false);
});

test("all authored UI value surfaces reject values the native parser cannot load", () => {
  const id = "11111111-1111-4111-8111-111111111111";
  const layout = {mode:"free",x:0,y:0,width:100,height:100,
    anchorX:0,anchorY:0,anchorRight:0,anchorBottom:0,pivotX:0,pivotY:0,
    margin:0,alignment:"start",sizeRule:"fixed"};
  const node = {id,parentId:null,order:0,kind:"control",runtimeType:"Control",
    name:"Root",visible:true,locked:false,layout,bindings:{},runtimeProperties:{}};
  const scene = {format:"PrismatiXUIScene",schemaRevision:2,id,revision:1,
    name:"Values",width:100,height:100,rootId:id,nodes:[node],theme:[],
    behaviorGraph:{nodes:[],links:[],groups:[]},behaviorTriggers:[],
    visualStateGroups:[]};
  assert.equal(validateDocument("ui", scene).valid, true);
  const invalidVec = {type:"vec2",x:1,y:"not a number"};
  const invalidNode = validateDocument("ui", {...scene,nodes:[
    {...node,runtimeProperties:{translation:invalidVec}}]});
  assert.equal(invalidNode.valid, false);
  assert.ok(invalidNode.diagnostics.some((item) => item.code === "PXSDKSEM1325" &&
    item.details === "nodes[0].runtimeProperties.translation"));
  const invalidAction = validateDocument("ui", {...scene,nodes:[
    {...node,onClick:{id:"host.custom",arguments:{color:{type:"color",value:"pink"}}}}]});
  assert.equal(invalidAction.valid, false);
  assert.ok(invalidAction.diagnostics.some((item) => item.code === "PXSDKSEM1325" &&
    item.details === "nodes[0].onClick.arguments.color"));
  assert.equal(nativeUiValueType({type:"resource",value:"Assets/UI/button.png"}), "invalid");
  assert.equal(nativeUiValueType({type:"resource",value:"Assets/UI/button.png"}, true), "resource");
  assert.equal(nativeUiValueType(Array.from({length:1025}, () => 1)), "invalid");
  assert.equal(nativeUiValueType({["界".repeat(86)]:1}), "invalid");
  assert.equal(nativeUiValueType({value:"x".repeat(1024 * 1024)}), "invalid");
  const surfaces = {...scene,
    nodes:[{...node,componentInstance:{publicSignals:{pressed:{arguments:{bad:invalidVec}}}}}],
    behaviorGraph:{nodes:[{properties:{bad:invalidVec},arguments:{bad:invalidVec}}],
      links:[],groups:[]},
    visualStateGroups:[{states:[{overrides:[{value:invalidVec}]}]}],
    animations:{clips:[{tracks:[{keys:[{value:invalidVec}]}]}],
      stateMachine:{parameters:[{defaultValue:invalidVec}],
        transitions:[{conditions:[{value:invalidVec}]}]}}};
  const locations = new Set(validateSemantics("ui", surfaces)
    .filter((item) => item.code === "PXSDKSEM1325").map((item) => item.details));
  for (const location of [
    "nodes[0].componentInstance.publicSignals.pressed.arguments.bad",
    "behaviorGraph.nodes[0].properties.bad",
    "behaviorGraph.nodes[0].arguments.bad",
    "visualStateGroups[0].states[0].overrides[0].value",
    "animations.clips[0].tracks[0].keys[0].value",
    "animations.stateMachine.parameters[0].defaultValue",
    "animations.stateMachine.transitions[0].conditions[0].value",
  ]) assert.ok(locations.has(location), `missing native UiValue validation at ${location}`);
});

test("Timeline semantic validation rejects unordered or out-of-range samples", () => {
  const invalid = {format:"PrismatiXTimeline",schemaRevision:2,id:"bad",duration:1,tracks:[{id:"track",binding:{kind:"stage",target:"yuki",property:"opacity"},keyframes:[{time:0.8,value:1,easing:"linear"},{time:0.4,value:0,easing:"linear"}]}],markers:[],nestedClips:[]} satisfies JsonValue;
  const result = validateDocument("timeline", invalid);
  assert.equal(result.valid, false);
  assert.ok(result.diagnostics.some((item) => item.code === "PXSDKSEM1201"));
});

test("compiled Runtime IR and source maps use the same bounded UTF-8 byte budget as native", () => {
  const documents = {
    runtimeIr: {format:"PrismatiXRuntimeIR",schemaRevision:2,documentId:"large",committedRevision:1,operations:[]},
    sourceMap: {format:"PrismatiXSourceMap",schemaRevision:2,documentId:"large",mappings:[]},
  };
  for (const [kind, value] of Object.entries(documents)) {
    const json = JSON.stringify(value);
    const boundary = json + " ".repeat(64 * 1024 * 1024 - Buffer.byteLength(json));
    assert.equal(parseDocument(kind, boundary).valid, true);
    const over = parseDocument(kind, boundary + " ");
    assert.equal(over.valid, false);
    assert.equal(over.diagnostics[0]?.code, "PXSDKJSON1001");
    assert.equal(parseDocument(kind, boundary.slice(0, -1) + "雪").valid, false);
  }
});
