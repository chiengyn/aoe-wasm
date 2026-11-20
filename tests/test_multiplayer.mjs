import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import createEngine from '../engine/build/core-probe.mjs';
import {DatCore} from './lib/dat-core.mjs';
import {EngineSession} from './lib/engine-session.mjs';
const root=new URL('../',import.meta.url),read=p=>fs.readFileSync(new URL(p,root)),json=p=>JSON.parse(read(p)),sha=b=>createHash('sha256').update(b).digest('hex');
const v=json('web/game/versions.json').find(v=>v.key==='ror'),raw=read('web/'+v.url),strings=json('web/'+v.strings),wasmBinary=read('engine/build/core-probe.wasm');
const dat=new DatCore((await WebAssembly.instantiate(read('formats/build/dat.wasm'))).instance).load(raw);
const room={seed:12345,settings:[0,4,1,1,0,0,50,0,0,0,0,0,2,0],players:[{slot:1,name:'Host',civ:1,color:1,team:1},{slot:2,name:'Guest',civ:2,color:2,team:1}]};
const sessions=[];
for(const me of [1,2]){
 const m=await createEngine({wasmBinary,gameStrings:strings,print:()=>{},printErr:console.error});
 for(const dir of ['/data','/data2','/savegame','/c'])m.FS.mkdir(dir);
 m.FS.writeFile('/empires.dat',raw);assert.equal(m.ccall('engine_probe_load','number',['string'],['/empires.dat']),1);
 const s=new EngineSession(m,dat);s.startMultiplayer(room,me);sessions.push(s);
}
let checks=0;
function shared(s){const state=s.snapshot();return sha(JSON.stringify({time:state.time,objects:state.objects,players:state.players,resources:room.players.map((_,i)=>Array.from({length:33},(_,r)=>s.m._engine_probe_resource(i+1,r)))}));}
function same(){assert.equal(shared(sessions[0]),shared(sessions[1]),'Independent engines must agree');checks++;}
same();const state=sessions[0].snapshot(),towns=[1,2].map(p=>state.objects.find(o=>o.player===p&&o.master===109)),villagers=[1,2].map(p=>state.objects.find(o=>o.player===p&&o.master===83));
assert.notEqual(sha(sessions[0].snapshot().visibility),sha(sessions[1].snapshot().visibility),'Each player gets their own fog');checks++;
for(const s of sessions){assert.equal(s.command({kind:'move',ids:[villagers[1].id],x:20,y:20},1).accepted,0);checks++;}
for(let actor=1;actor<=2;actor++)for(const s of sessions){assert.equal(s.command({kind:'train',ids:[towns[actor-1].id],unit:83},actor).accepted,1);checks++;}
for(let turn=0;turn<250;turn++){for(const s of sessions)s.tick(2);same();}
for(const s of sessions)for(const p of [1,2]){assert.equal(s.snapshot().objects.filter(o=>o.player===p&&o.master===83).length,state.objects.filter(o=>o.player===p&&o.master===83).length+1);checks++;}
for(let actor=1;actor<=2;actor++)for(const s of sessions)s.command({kind:'move',ids:[villagers[actor-1].id],x:towns[actor-1].x+4,y:towns[actor-1].y+4},actor);
for(let turn=0;turn<100;turn++){for(const s of sessions)s.tick(2);same();}
const report={passed:true,checks,wasmSha256:sha(wasmBinary),scope:'Two independent WASM instances: same world, separate fog and ownership, training and movement from both players over 350 lockstep turns. No claim of retail network compatibility.'};
fs.writeFileSync(new URL('reports/multiplayer-tests.json',root),JSON.stringify(report,null,2)+'\n');console.log(report);
