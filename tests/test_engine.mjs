// Executes the recovered C++ engine in WASM under Node. This is not browser QA
// and does not establish equivalence to the supplied retail executable.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {DatCore} from './lib/dat-core.mjs';
import createEngine from '../engine/build/core-probe.mjs';
const root=new URL('../',import.meta.url),read=p=>fs.readFileSync(new URL(p,root)),json=p=>JSON.parse(read(p));
let checks=0;
const eq=(a,b,message)=>{assert.deepEqual(a,b,message);checks++;};
const ok=(value,message)=>{assert.ok(value,message);checks++;};
const sha=data=>createHash('sha256').update(data).digest('hex');
const versions=json('web/game/versions.json'),parser=await WebAssembly.compile(read('formats/build/dat.wasm'));
async function load(version){
  const bytes=read('web/'+version.url);eq(sha(bytes),version.sha256,'Only verified local DAT fixtures may enter the legacy parser');
  const dat=new DatCore(await WebAssembly.instantiate(parser)).load(bytes);
  const logs=[];
  const engine=await createEngine({gameStrings:json('web/'+version.strings),print:s=>logs.push(s),printErr:s=>logs.push(s)});
  for(const directory of ['/data2','/data','/savegame','/c'])engine.FS.mkdir(directory);
  engine.FS.writeFile('/empires.dat',bytes);
  eq(engine._engine_probe_count(0),-1);
  eq(engine.ccall('engine_probe_load','number',['string'],['/empires.dat']),1);
  return {engine,dat,logs};
}
const metadata=[];
for(const version of versions){
  const {engine:m,dat}=await load(version);
  eq(m._engine_probe_count(0),dat.civs.length);eq(m._engine_probe_count(1),dat.graphics.length);
  for(const [civIndex,civ] of dat.civs.entries()){
    eq(m._engine_probe_units(civIndex),civ.slots);
    const bySlot=new Map(dat.units.filter(u=>u.civ===civIndex).map(u=>[u.slot,u]));
    for(let id=0;id<civ.slots;id++){
      const u=bySlot.get(id);
      if(!u){eq(m._engine_probe_unit(civIndex,id,0),-1);continue;}
      for(const [field,key] of ['id','type','hp','group','los','enabled'].entries())
        eq(m._engine_probe_unit(civIndex,id,field),u[key],`${version.key}/civ ${civIndex}/unit ${id}/${key}`);
    }
  }
  metadata.push({version:version.key,civs:dat.civs.length,graphics:dat.graphics.length,units:dat.units.length,sha256:version.sha256});
}
const ror=versions.find(v=>v.key==='ror');
const unitIds=(m,player)=>Array.from({length:m._engine_probe_objects(player)},(_,i)=>m._engine_probe_object_id(player,i));
const value=(m,id,field)=>m._engine_probe_object_value(id,field);
const resources=m=>[0,1,2,3].map(r=>m._engine_probe_resource(1,r));
function mapSnapshot(m){return Array.from({length:72*72},(_,i)=>m._engine_probe_tile(i%72,Math.floor(i/72),0)|(m._engine_probe_tile(i%72,Math.floor(i/72),1)<<5));}
async function gameplay(seed,actions){
  const {engine:m}=await load(ror);
  eq(m._engine_probe_new_game(seed,0),1);eq(m._engine_probe_objects(1),4);eq(m._engine_probe_objects(2),4);
  const map=mapSnapshot(m);ok(map.every(t=>t>=0&&t<=255));
  const start=unitIds(m,1).map(id=>[id,...[0,1,2,3].map(f=>value(m,id,f))]);
  if(!actions)return {map,start};
  const ids=unitIds(m,1),villager=ids.find(id=>value(m,id,0)===83),town=ids.find(id=>value(m,id,0)===109);
  ok(villager!==undefined&&town!==undefined);
  const before=[value(m,villager,1),value(m,villager,2)];
  eq(m._engine_probe_move(villager,before[0]+3,before[1]+3),1);eq(m._engine_probe_tick(200),10000);
  const after=[value(m,villager,1),value(m,villager,2)];
  ok(Math.hypot(after[0]-before[0],after[1]-before[1])>2,'A command must move the villager in the real engine');
  ok(Math.hypot(after[0]-before[0]-3,after[1]-before[1]-3)<1,'Villager must reach the commanded area');
  eq(resources(m),[200,200,150,0]);
  eq(m._engine_probe_train(town,83),1);eq(m._engine_probe_tick(500),35000);
  eq(m._engine_probe_objects(1),5,'Training must create a fourth villager');eq(resources(m),[150,200,150,0],'Training must spend 50 food');
  const trees=unitIds(m,0).filter(id=>value(m,id,6)===15);
  const distance=id=>(value(m,id,1)-value(m,villager,1))**2+(value(m,id,2)-value(m,villager,2))**2;
  trees.sort((a,b)=>distance(a)-distance(b));ok(trees.length>0);
  eq(m._engine_probe_work(villager,trees[0]),1);eq(m._engine_probe_tick(2400),155000);
  const afterGather=resources(m);ok(afterGather[1]>200,'Wood must be deposited into the player stockpile');
  let location;
  for(let y=12;y<22&&!location;y++)for(let x=52;x<65&&!location;x++)
    if(m._engine_probe_placement(1,70,x+.5,y+.5)===0)location=[x+.5,y+.5];
  ok(location,'An explored legal house location must exist');
  eq(m._engine_probe_build(villager,70,...location),1);eq(m._engine_probe_tick(1800),245000);
  const houses=unitIds(m,1).filter(id=>value(m,id,0)===70);eq(houses.length,1);
  eq(value(m,houses[0],4),2,'House must finish construction');eq(m._engine_probe_objects(1),6);
  eq(resources(m)[1],afterGather[1]-30,'House must spend 30 wood');
  eq(m._engine_probe_tick(-1),-1);eq(m._engine_probe_tile(-1,0,0),-1);
  return {map,start,movement:{before,after},afterGather,resources:resources(m),objects:unitIds(m,1).map(id=>[id,...[0,1,2,3,4].map(f=>value(m,id,f))]),worldTime:245000};
}
const a=await gameplay(12345,true),b=await gameplay(12345,true),c=await gameplay(54321,false);
eq(a,b,'Fresh instances with the same seed and commands must replay identically');
ok(sha(Buffer.from(a.map))!==sha(Buffer.from(c.map)),'Different seeds must generate different maps');
// Independent instruction-by-instruction arithmetic for the actual retail RNG.
const {engine:rng}=await load(ror);
function retailStep(eax){let ecx=(eax+2*eax)>>>0,edx=(eax+4*ecx)>>>0;edx=(edx<<4)>>>0;edx=(edx+eax)>>>0;edx=(edx<<8)>>>0;edx=(edx-eax)>>>0;eax=(eax+4*edx+0x269ec3)>>>0;return [eax,(eax>>16)&0x7fff];}
for(const seed of [0,1,14,12345,0x7fffffff,0x80000000,0xffffffff]){let state=seed;for(let i=1;i<=200;i++){let result;[state,result]=retailStep(state);eq(rng._engine_probe_rand(seed,i),result);}}
const link=json('engine/build/link-probe.json');eq(link.wasm_sha256,sha(read('engine/build/core-probe.wasm')));
const report={passed:true,checks,metadata,seed:12345,simulationTimeMs:a.worldTime,movement:a.movement,
  afterGather:a.afterGather,finalResources:a.resources,finalPlayerObjects:a.objects.length,replayDeterministic:true,
  differentSeedChangesMap:true,rngVectors:1400,wasmSha256:link.wasm_sha256,unsupportedAPIs:link.unsupported_apis,
  browserTested:false,retailGameplayParityVerified:false,scope:'headless C++ engine WASM metadata and simulation; two human-controlled seats, no AI/audio/render/network'};
fs.writeFileSync(new URL('reports/engine-tests.json',root),JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify(report,null,2));
