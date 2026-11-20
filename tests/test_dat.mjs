import fs from 'node:fs';
import assert from 'node:assert/strict';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {DatCore,animationTiming,animationFrame} from './lib/dat-core.mjs';
import {resourceIndex,planLayers} from './lib/unit-assets.mjs';
const root=fileURLToPath(new URL('../',import.meta.url)),read=p=>fs.readFileSync(root+p),json=p=>JSON.parse(read(p));
const module=await WebAssembly.compile(read('formats/build/dat.wasm'));assert.deepEqual(WebAssembly.Module.imports(module),[]);
const core=new DatCore(await WebAssembly.instantiate(module));
let checks=0;const eq=(a,b)=>{assert.deepEqual(a,b);checks++;},ok=x=>{assert.ok(x);checks++;};
// Small wire fixture: independent explicit byte offsets from the 77-byte graphic
// record, plus empty sections and the serialized 36,304-byte map structure.
function fixture(){
 const chunks=[];const emit=(n,values=[])=>{const b=Buffer.alloc(n);for(const [at,type,value] of values)b[type](value,at);chunks.push(b);return b;};
 chunks.push(Buffer.from('VER 3.7\0'));emit(4);emit(2);emit(2);emit(2,[[0,'writeInt16LE',3]]);emit(12,[[0,'writeInt32LE',1],[4,'writeInt32LE',1],[8,'writeInt32LE',1]]);
 for(const [id,angles,mirror] of [[0,8,6],[1,2,1],[2,4,0]]){
  const g=emit(77,[[34,'writeInt32LE',202+id],[40,'writeUInt8',20],[41,'writeInt16LE',-1],[54,'writeInt16LE',-1],[57,'writeInt16LE',2],[59,'writeInt16LE',angles],[61,'writeFloatLE',1],[65,'writeFloatLE',.125],[69,'writeFloatLE',.5],[73,'writeUInt8',1],[74,'writeInt16LE',id],[76,'writeUInt8',mirror]]);
  g.write('fixture',0);
 }
 emit(36304);emit(8);emit(4);emit(2);emit(2);return Buffer.concat(chunks);
}
const sample=fixture();core.load(sample);eq(core.counts,[3,0,0,0,0,0,0,0]);eq(core.graphics[0].name,'fixture');
const indices=[4,2,0,2,4,6,8,6],mirrors=[true,true,false,false,false,false,false,true];
for(let d=0;d<8;d++)eq(core.frame(0,d,0),{index:indices[d],mirror:mirrors[d]});
eq(core.frame(1,0,1),{index:1,mirror:false});eq(core.frame(1,1,1),{index:1,mirror:true});eq(core.frame(2,3,1),{index:7,mirror:false});
for(const args of [[-1,0,0],[3,0,0],[0,-1,0],[0,8,0],[0,0,2]])eq(core.api.dat_frame(...args),-1);
const time=animationTiming(core.graphics[0],2);eq(time,{seconds:.0625,pause:.5,animated:true,once:false});
for(const [elapsed,expected] of [[0,0],[.063,1],[.2,1],[.624,1],[.625,0]])eq(animationFrame(core.graphics[0],time,elapsed).frame,expected);
eq(animationFrame({...core.graphics[0],flags:9},{...time,once:true},1),{frame:1,done:true});
eq(animationTiming(core.graphics[0],0).animated,false);
function rejected(bytes){assert.throws(()=>core.load(bytes));checks++;eq(core.api.dat_unit(0),0);eq(core.api.dat_graphic(0),0);}
for(const n of [0,7,8,9,12,16,18,30,40,100,260,sample.length-1])rejected(sample.subarray(0,n));
let bad=Buffer.from(sample);bad[0]=0;rejected(bad);bad=Buffer.concat([sample,Buffer.from([0])]);rejected(bad);
bad=Buffer.from(sample);bad.writeInt16LE(-1,16);rejected(bad);
const manifest=json('web/game/manifest.json'),versions=json('web/game/versions.json');const reports=[];
for(const version of versions){
 const raw=read('web/'+version.url);eq(createHash('sha256').update(raw).digest('hex'),version.sha256);core.load(raw);
 eq(core.api.dat_error_offset(),raw.length);eq(core.api.dat_position(9),raw.length);
 eq(core.counts,version.key==='aoe'?[847,2399,13,2823,448,140,118,5]:[941,3580,17,4490,515,218,127,10]);
 const language=json('web/'+version.strings),resources=resourceIndex(manifest,version.key),types={};
 const cavalry=core.units.find(u=>u.civ===1&&u.id===37),villager=core.units.find(u=>u.civ===1&&u.id===83);
 eq([cavalry.name,language[cavalry.language],cavalry.hp,cavalry.speed,cavalry.stand,cavalry.walk,cavalry.attack,cavalry.death],['Soldier-Cavalry1','Cavalry',150,2,289,669,21,191]);
 eq([villager.name,language[villager.language],villager.hp,villager.trainAt,villager.trainTime],['Man','Villager',25,109,20]);
 eq(core.graphics[669].slp,663);ok(Math.abs(animationTiming(core.graphics[669],cavalry.speed).seconds-.05)<1e-7);
 eq(core.states(villager).map(s=>s.key),['stand','walk','attack','death']);
 const issues=[],referenced=new Set();let states=0,plans=0,valid=0;
 for(const u of core.units){
  types[u.type]=(types[u.type]||0)+1;ok(u.civ>=0&&u.civ<core.civs.length);ok(u.name.length>0);ok(Number.isFinite(u.speed)&&u.speed>=0);ok(Number.isFinite(u.los));
  eq(raw.readInt16LE(u.offset+3),u.id);eq(raw.readUInt16LE(u.offset+5),u.language);
  for(const key of ['stand','walk','run','attack','death','undead','construction'])if(u[key]>=0)ok(core.graphics[u[key]]?.present);
  for(const s of core.states(u)){states++;referenced.add(s.id);}
 }
 for(const id of referenced){
  const g=core.graphics[id];ok(g.frames>0&&g.angles>0);
  for(let direction=0;direction<g.angles;direction++)for(let frame=0;frame<g.frames;frame++){
   const plan=planLayers(core,resources,id,direction,frame);plans++;
   if(plan.issues.length){if(frame===0||frame===g.frames-1)issues.push({graphic:id,name:g.name,direction,frame,issues:plan.issues});}
   else valid++;
   for(const layer of plan.layers)ok(layer.mapped.index>=0&&layer.mapped.index<layer.entry.frames);
  }
 }
 const checkpoints=Array.from({length:10},(_,i)=>core.api.dat_position(i));
 // Cut at every major section, and corrupt unit type/name size independently.
 for(const at of checkpoints.filter(x=>x>0&&x<raw.length))rejected(raw.subarray(0,at));
 bad=Buffer.from(raw);bad[cavalry.offset]=255;rejected(bad);
 bad=Buffer.from(raw);bad.writeInt16LE(-1,cavalry.offset+1);rejected(bad);
 core.load(raw);const offsets=Array.from({length:10},(_,i)=>core.api.dat_position(i));
 reports.push({version:version.key,bytes:raw.length,counts:core.counts,checkpoints:offsets,unit_types:types,
  named_units:core.units.filter(u=>language[u.language]).length,states,referenced_graphics:referenced.size,frame_plans:plans,complete_frame_plans:valid,incomplete_frame_plans:plans-valid,issues});
}
// Deterministic mutations: rejection OR a bounded successful parse, never trap.
let seed=0x17202026;const random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed;};
const raw=read('web/game/ror.dat.raw');let accepted=0,rejectedMutations=0;
for(let i=0;i<1000;i++){
 const mutation=Buffer.from(raw);for(let j=0;j<1+random()%4;j++)mutation[random()%mutation.length]=random()&255;
 new Uint8Array(core.api.memory.buffer,core.base,mutation.length).set(mutation);
 const result=core.api.dat_parse(core.base,mutation.length);
 if(result===0){accepted++;eq(core.api.dat_error_offset(),mutation.length);}else{rejectedMutations++;eq(core.api.dat_graphic(0),0);}
}
core.load(raw);eq(core.counts[1],3580);
const report={milestone:2,wasm_bytes:read('formats/build/dat.wasm').length,runtime_imports:0,checks,mutations:{total:1000,accepted,rejected:rejectedMutations,traps:0},versions:reports};
fs.writeFileSync(root+'reports/dat-tests.json',JSON.stringify(report,null,2));
console.log(JSON.stringify({...report,versions:reports.map(({issues,...r})=>({...r,issue_examples:issues.slice(0,3)}))},null,2));
