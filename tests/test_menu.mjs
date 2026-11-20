// Native constructor/render/transition regression. Font metrics are a fixture;
// these tests deliberately make no claim about Windows text pixel equality.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import createEngine from '../engine/build/core-probe.mjs';
import {AssetCore} from './lib/core.mjs';
const root=new URL('../',import.meta.url),read=p=>fs.readFileSync(new URL(p,root)),json=p=>JSON.parse(read(p));
const sha=b=>createHash('sha256').update(b).digest('hex');
const manifest=json('web/game/manifest.json'),archives=manifest.archives.filter(a=>['data/Interfac.drs','data2/Interfac.drs'].includes(a.path));
const retail=json('reports/retail-ui.json');assert.equal(retail.passed,true);
let checks=0;const eq=(a,b,message)=>{assert.deepEqual(a,b,message);checks++;};
const images=[];
for(const width of [640,800,1024]){
  const height=width*3/4;let engine,frame,commands=[];
  const host={
    cursor(id){assert.ok([0,32512,32514].includes(id));},
    measure(font,text,length,field){const size=Math.abs(engine.HEAP32[font/4]);return field===0?Math.round(length*size*.55):field===1?size+4:size;},
    text(font,text,length,x,y,color,clip){commands.push({x,y,color,text:new TextDecoder('windows-1252').decode(engine.HEAPU8.subarray(text,text+length))});},
    begin(){commands=[];},
    present(bits,w,h,pitch,palette){frame=new Uint8Array(w*h*4);for(let y=0;y<h;y++)for(let x=0;x<w;x++){const index=engine.HEAPU8[bits+y*pitch+x],src=palette+index*4,dst=4*(y*w+x);frame.set(engine.HEAPU8.subarray(src,src+3),dst);frame[dst+3]=255;}},
  };
  engine=await createEngine({gameStrings:json('web/game/ror.strings.json'),menuHost:host,print:()=>{},printErr:console.error});
  for(const dir of ['/data','/data2'])engine.FS.mkdir(dir);
  for(const a of archives){const data=read('web/'+a.url);eq(sha(data),a.sha256);engine.FS.writeFile('/'+a.path,data);}
  eq(engine._menu_init(width,height),1);eq(engine._menu_draw(),5);
  const name=()=>engine.ccall('menu_name','string',[],[]);
  const labels=count=>Array.from({length:count},(_,i)=>engine.ccall('menu_button_text','string',['number'],[i]));
  eq(name(),'Main Menu');eq(labels(5),['Single Player','Multiplayer','Help','Scenario Builder','Exit']);
  for(let i=0;i<5;i++)eq(Array.from({length:4},(_,f)=>engine._menu_button_value(i,f)),retail.menus[0].buttons[i].rect.map((v,f)=>Math.trunc(v*(f%2?height/480:width/640))));
  // Independent decoder comparison: untouched top section must reproduce
  // original RoR pixels, including the title/logo, at all three resolutions.
  const archive=archives.find(a=>a.path==='data2/Interfac.drs'),core=new AssetCore((await WebAssembly.instantiate(read('formats/build/assets.wasm'))).instance);
  const data=read('web/'+archive.url);core.load(data);
  const palette=archive.entries.find(e=>e.id===50501);core.palette(data.subarray(palette.offset,palette.offset+palette.size));
  const entry=archive.entries.find(e=>e.id==={640:50100,800:50103,1024:50105}[width]),background=core.decode(entry);
  const crop=(rgba,end=150,start=6)=>{const out=new Uint8Array((width-12)*(end-start)*4);for(let y=start;y<end;y++)out.set(rgba.subarray((y*width+6)*4,(y*width+width-6)*4),(y-start)*(width-12)*4);return out;};
  eq(sha(crop(frame)),sha(crop(background.rgba)),'Native framebuffer must match original background pixels');
  images.push({width,height,verifiedRegion:[6,6,width-12,144],background_sha256:sha(crop(frame))});
  eq(engine._menu_activate(1),2);eq(name(),'Main Menu');
  eq(engine._menu_activate(0),1);eq(engine._menu_draw(),7);eq(name(),'Single Player Menu');
  const singleArchive=archives.find(a=>a.path==='data/Interfac.drs'),singleData=read('web/'+singleArchive.url);
  core.load(singleData);const singlePalette=singleArchive.entries.find(e=>e.id===50502);
  core.palette(singleData.subarray(singlePalette.offset,singlePalette.offset+singlePalette.size));
  const single=core.decode(singleArchive.entries.find(e=>e.id==={640:50107,800:50109,1024:50111}[width]));
  eq(sha(crop(frame,100,30)),sha(crop(single.rgba,100,30)),'Single-player background must use its own palette and the base-game archive fallback');
  eq(labels(6),['Random Map','Campaign','Death Match','Scenario','Saved Game','Cancel']);
  for(let i=0;i<6;i++)eq(Array.from({length:4},(_,f)=>engine._menu_button_value(i,f)),retail.menus[1].buttons[i].rect.map((v,f)=>Math.trunc(v*(f%2?height/480:width/640))));
  eq(engine._menu_button_value(5,5),27);
  eq(engine._menu_activate(5),1);eq(engine._menu_draw(),5);eq(name(),'Main Menu');
  eq(engine._menu_activate(0),1);eq(engine._menu_draw(),7);
  eq(engine._menu_is_closed(),0);eq(engine._menu_activate(6),1);eq(engine._menu_is_closed(),1);
}
const report={passed:true,checks,images,wasmSha256:sha(read('engine/build/core-probe.wasm')),scope:'Native C++ main and single-player menu constructors, DRS loading, indexed background pixels, control layout and navigation. Test font metrics are synthetic; Windows font and full retail behavior parity are not established.'};
fs.writeFileSync(new URL('reports/menu-tests.json',root),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report,null,2));
