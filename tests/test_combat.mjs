// A controlled two-human-seat combat scenario, executed in WASM under Node.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import create from '../engine/build/core-probe.mjs';
import {DatCore} from './lib/dat-core.mjs';
import {EngineSession} from './lib/engine-session.mjs';
const root=new URL('../',import.meta.url),read=p=>fs.readFileSync(new URL(p,root)),json=p=>JSON.parse(read(p)),sha=b=>createHash('sha256').update(b).digest('hex');
let checks=0;const eq=(a,b,msg)=>{assert.deepEqual(a,b,msg);checks++;},ok=(x,msg)=>{assert.ok(x,msg);checks++;};
const v=json('web/game/versions.json').find(v=>v.key==='ror'),raw=read('web/'+v.url);eq(sha(raw),v.sha256);
const dat=new DatCore((await WebAssembly.instantiate(read('formats/build/dat.wasm'))).instance).load(raw);
const m=await create({gameStrings:json('web/'+v.strings),print:()=>{}});
for(const d of ['/data','/data2','/c','/savegame'])m.FS.mkdir(d);m.FS.writeFile('/empires.dat',raw);eq(m.ccall('engine_probe_load','number',['string'],['/empires.dat']),1);m._engine_probe_clock(1);
const session=new EngineSession(m,dat);session.start(12345,false,4,2);let state=session.snapshot();
const villagers=state.objects.filter(o=>o.player===1&&o.master===83),enemy=state.objects.find(o=>o.player===2&&o.master===83),town=state.objects.find(o=>o.player===1&&o.master===109);
eq(enemy.hp,25);eq(villagers.length,3);
// The fixture controls the second human seat directly; the web bridge itself
// still rejects orders for enemy units. No statistics or damage are modified.
eq(m._engine_probe_move(enemy.id,town.x-3,town.y+3),1);session.tick(1800);
eq(session.command({kind:'move',ids:villagers.map(o=>o.id),x:35.5,y:21.5}).accepted,3);session.tick(1800);
eq(session.command({kind:'move',ids:villagers.map(o=>o.id),x:36.5,y:20.5}).accepted,3);session.tick(1000);
eq(session.command({kind:'work',ids:villagers.map(o=>o.id),target:enemy.id}).accepted,3);
const hp=[];let removed=false;
for(let i=0;i<800;i++){state=session.tick(1);const target=state.objects.find(o=>o.id===enemy.id);if(!target){removed=true;break;}if(!hp.length||hp.at(-1)!==target.hp)hp.push(target.hp);}
ok(hp.some(n=>n>0&&n<25),'Real combat must lower the target HP before its death');ok(removed,'The defeated object must leave the live object lists');eq(state.objects.filter(o=>o.player===2&&o.group===4).length,2);
eq(state.objects.filter(o=>villagers.some(v=>v.id===o.id)&&o.hp>0).length,3);
const wasmSha256=sha(read('engine/build/core-probe.wasm'));eq(wasmSha256,json('engine/build/link-probe.json').wasm_sha256);
const report={passed:true,checks,seed:12345,simulationMs:state.time,attackers:villagers.map(o=>o.id),target:enemy.id,targetHp:hp,targetRemoved:removed,enemyRemainingVillagers:2,wasmSha256,browserTested:false,scope:'Controlled villager combat through recovered native work/attack commands; no damage/HP patches and no full combat-balance parity claim'};
fs.writeFileSync(new URL('reports/combat-tests.json',root),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report,null,2));
