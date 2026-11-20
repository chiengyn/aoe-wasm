// Offline integration of the probe build through tests/lib/engine-session.mjs. No browser is started.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import createEngine from '../engine/build/core-probe.mjs';
import {DatCore} from './lib/dat-core.mjs';
import {AssetCore} from './lib/core.mjs';
import {EngineSession,project,unproject,visibleObject} from './lib/engine-session.mjs';
import {resourceIndex,planLayers} from './lib/unit-assets.mjs';
const root=new URL('../',import.meta.url),read=p=>fs.readFileSync(new URL(p,root)),json=p=>JSON.parse(read(p)),sha=b=>createHash('sha256').update(b).digest('hex');
let checks=0;const eq=(a,b,msg)=>{assert.deepEqual(a,b,msg);checks++;},ok=(x,msg)=>{assert.ok(x,msg);checks++;};
const versions=json('web/game/versions.json'),version=versions.find(v=>v.key==='ror'),raw=read('web/'+version.url),strings=json('web/'+version.strings);
eq(sha(raw),version.sha256);
const dat=new DatCore((await WebAssembly.instantiate(read('formats/build/dat.wasm'))).instance).load(raw);
// Match the browser worker: instantiate from the bytes whose hash was verified.
const wasmBinary=read('engine/build/core-probe.wasm');
eq(sha(wasmBinary),json('engine/build/link-probe.json').wasm_sha256);
const engine=await createEngine({wasmBinary,gameStrings:strings,print:()=>{},printErr:()=>{}});
for(const path of ['/data','/data2','/savegame','/c'])engine.FS.mkdir(path);
const files=json('web/game/engine-files.json');
for(const file of files){const data=read('web/'+file.url);eq(sha(data),file.sha256);engine.FS.writeFile(file.virtual,data);}
engine.FS.writeFile('/empires.dat',raw);eq(engine.ccall('engine_probe_load','number',['string'],['/empires.dat']),1);
// Regression: the upper 16 ID bits are ignored by Windows string tables.
for(let i=0;i<125;i++)eq(engine.ccall('engine_probe_string','string',['number'],[0x26930+i]),strings[0x6930+i]);
eq(engine.ccall('engine_probe_string','string',['number'],[65535]),'');
eq(engine._engine_probe_clock(1),1);
const session=new EngineSession(engine,dat),map=session.start(12345,true,4,2);
eq([map.width,map.height,map.halfWidth,map.halfHeight],[72,72,32,16]);
const aiFile=engine.ccall('engine_probe_ai_name','string',[],[]);
ok(files.some(f=>f.virtual==='/'+aiFile.toLowerCase().replaceAll('\\','/')),'Selected AI script must be an original local file');
ok(engine._engine_probe_ai(2)>0,'AI must load a nonempty build list');
const initial=session.snapshot();eq(initial.objects.filter(o=>o.player===1).length,4);eq(initial.objects.filter(o=>o.player===2).length,4);
eq(initial.resources.slice(0,4),[200,200,150,0]);eq(initial.players,[0,0]);
const town=initial.objects.find(o=>o.player===1&&o.master===109),villager=initial.objects.find(o=>o.player===1&&o.master===83),enemy=initial.objects.find(o=>o.player===2&&o.master===83);
eq(session.command({kind:'move',ids:[enemy.id],x:20,y:20}).accepted,0,'The UI cannot command another player');
eq(session.command({kind:'move',ids:[villager.id],x:NaN,y:20}).accepted,0);
eq(session.command({kind:'work',ids:[villager.id],target:enemy.id}).accepted,0,'Unseen targets cannot be attacked through the bridge');
eq(session.command({kind:'train',ids:[villager.id],unit:83}).accepted,0,'Training requires the correct building');
eq(session.command({kind:'train',ids:[town.id],unit:83}).accepted,1);
let state=session.tick(500);eq(state.objects.filter(o=>o.player===1).length,5);eq(state.resources[0],150);
eq(session.command({kind:'stop',ids:[villager.id]}).accepted,1);
const nearest=state.objects.filter(o=>o.player===0&&o.group===15).sort((a,b)=>Math.hypot(a.x-villager.x,a.y-villager.y)-Math.hypot(b.x-villager.x,b.y-villager.y))[0];
ok(nearest);eq(session.command({kind:'move',ids:[villager.id],x:nearest.x+1,y:nearest.y+1}).accepted,1);state=session.tick(600);
const trees=state.objects.filter(o=>o.player===0&&o.group===15&&visibleObject(o,state.visibility,map.width,map.height)).sort((a,b)=>Math.hypot(a.x-villager.x,a.y-villager.y)-Math.hypot(b.x-villager.x,b.y-villager.y));ok(trees.length>0,'Exploring must reveal nearby trees');
eq(session.command({kind:'work',ids:[villager.id],target:trees[0].id}).accepted,1);
state=session.tick(2400);ok(state.resources[1]>200,'A browser-bridge command must deposit harvested wood');const gatheredWood=state.resources[1];
let place;
for(let y=town.y-6;y<town.y+6&&!place;y++)for(let x=town.x-6;x<town.x+6&&!place;x++)if(engine._engine_probe_placement(1,70,x,y)===0)place={x,y};
ok(place);eq(session.command({kind:'build',ids:[villager.id],unit:70,...place}).accepted,1);state=session.tick(1800);
const house=state.objects.find(o=>o.player===1&&o.master===70&&o.state===2);ok(house,'House must be completed');eq(state.resources[1],gatheredWood-30);
ok(state.objects.filter(o=>o.player===2).length>4,'AI must grow without scripted host commands');
state=session.tick(10000);
ok(engine._engine_probe_resource(2,0)>200&&engine._engine_probe_resource(2,1)>200,'AI must independently gather and deposit food and wood');
const aiSummary={clock:'recovered deterministic debug clock advanced with simulation time',script:aiFile,buildListItems:engine._engine_probe_ai(2),objects:state.objects.filter(o=>o.player===2).length,resources:[0,1,2,3].map(r=>engine._engine_probe_resource(2,r))};
// Verify every terrain reference and current unit draw layer using the real decoders.
const manifest=json('web/game/manifest.json'),resources=resourceIndex(manifest,'ror'),assetModule=await WebAssembly.compile(read('formats/build/assets.wasm')),cores=new Map(),seen=new Set();
const paletteArchive=manifest.archives.find(a=>a.path==='data/Interfac.drs'),paletteEntry=paletteArchive.entries.find(e=>e.id===50500&&e.palette),palette=read('web/'+paletteArchive.url).subarray(paletteEntry.offset,paletteEntry.offset+paletteEntry.size);
async function decode(entry,frame,player){
  const key=`${entry.archive}/${entry.id}/${frame}/${player}`;if(seen.has(key))return;seen.add(key);
  let core=cores.get(entry.archive);if(!core){core=new AssetCore(await WebAssembly.instantiate(assetModule));core.load(read('web/'+manifest.archives.find(a=>a.path===entry.archive).url));core.palette(palette);cores.set(entry.archive,core);}
  const image=core.decode(entry,frame,Math.max(1,player));eq(image.rgba.length,image.width*image.height*4);
}
for(const tile of map.tiles){const entry=resources.get(tile.slp);ok(entry&&tile.frame>=0&&tile.frame<entry.frames,`Terrain (${tile.x},${tile.y}) must reference a real SLP frame`);await decode(entry,tile.frame,0);}
const issues=[];
for(const o of state.objects){if(o.inside>=0||o.state>=7)continue;const g=dat.graphics[o.graphic];ok(g?.present);const plan=planLayers(dat,resources,o.graphic,o.facet%g.angles,Math.min(o.frame,g.frames-1));issues.push(...plan.issues);for(const layer of plan.layers)await decode(layer.entry,layer.mapped.index,o.player);}
for(let x=0;x<72;x+=7)for(let y=0;y<72;y+=7){const p=project(x+.5,y+.5);eq(unproject(p.x,p.y),{x:x+.5,y:y+.5});}
const link=json('engine/build/link-probe.json');eq(sha(read('engine/build/core-probe.wasm')),link.wasm_sha256);
const report={passed:true,checks,simulationMs:state.time,bridgeCommands:['train','stop','gather','build'],completedHouse:house.id,gatheredWood,ai:aiSummary,terrainTiles:map.tiles.length,decodedFrames:seen.size,renderReferenceIssues:[...new Set(issues)],wasmSha256:link.wasm_sha256,browserTested:false,scope:'Node executes the same EngineSession as the browser worker; no layout, pointer events, audio or full-game parity verification'};
fs.writeFileSync(new URL('reports/session-tests.json',root),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report,null,2));
