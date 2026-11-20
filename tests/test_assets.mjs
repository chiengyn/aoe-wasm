import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {AssetCore} from './lib/core.mjs';
const root=new URL('../',import.meta.url), bytes=await readFile(new URL('formats/build/assets.wasm',root));
const module=await WebAssembly.compile(bytes);
assert.equal(WebAssembly.Module.imports(module).length,0,'The WASM core must not depend on external runtimes');
const core=new AssetCore(await WebAssembly.instantiate(module));
const palette=Buffer.from('JASC-PAL\r\n0100\r\n256\r\n'+Array.from({length:256},(_,i)=>`${i} ${255-i} ${(i*17)%256}`).join('\r\n'));
const color=i=>[i,255-i,(i*17)%256,255],transparent=[0,0,0,0],shadow=[0,0,0,128];
function fixture(w,commands,left=0,right=0) {
  const b=Buffer.alloc(72+commands.length);b.write('2.0N');b.writeUInt32LE(1,4);
  b.writeUInt32LE(68,32);b.writeUInt32LE(64,36);b.writeUInt32LE(w,48);b.writeUInt32LE(1,52);
  b.writeUInt16LE(left,64);b.writeUInt16LE(right,66);b.writeUInt32LE(72,68);b.set(commands,72);return b;
}
let fixtureTests=0;
function expectPixels(b,expected,player=1) {
  core.load(b);core.palette(palette);
  const r=core.decode({offset:0,size:b.length},0,player);
  assert.deepEqual([...r.rgba],expected.flat());fixtureTests++;
}
// Known, hand-derived outputs independent of parser implementation.
expectPixels(fixture(4,[8,7,8,5,23,9,15]),[color(7),color(8),transparent,color(9)]);
expectPixels(fixture(7,[0x26,0,9,0x2a,3,0x2b,0x17,200,15]),[color(32),color(41),color(35),color(35),shadow,shadow,color(200)],2);
expectPixels(fixture(3,[0x06,1,9,0x07,1,24,0x0b,1,15]),[color(137),color(24),shadow],8);
expectPixels(fixture(300,[0x12,44,...Array(300).fill(42),15]),Array(300).fill(color(42)));
expectPixels(fixture(300,[0x13,44,15]),Array(300).fill(transparent));
expectPixels(fixture(4,[8,2,3,15],1,1),[transparent,color(2),color(3),transparent]);
expectPixels(fixture(3,[],0x8000,0x8000),Array(3).fill(transparent));
const failures=[
  [fixture(2,[4,2,15]),-7], [fixture(1,[8,2,3,15]),-7],
  [fixture(1,[0x0e,15]),-6], [fixture(1,[0x16,10,15]),-8],
  [fixture(1,[0,15]),-7], [fixture(1,[4,2]),-2],
];
for(const [b,expected] of failures) {
  core.load(b);core.palette(palette);
  assert.equal(core.api.slp_decode(core.input,b.length,0,core.pal,1,core.image,16*1024*1024),expected);fixtureTests++;
}
const valid=fixture(2,[8,2,3,15]);
for(let n=0;n<valid.length;n++) {
  core.load(valid.subarray(0,n));core.palette(palette);
  assert.ok(core.api.slp_decode(core.input,n,0,core.pal,1,core.image,16*1024*1024)<0);fixtureTests++;
}
core.load(valid);core.palette(palette);
assert.equal(core.api.slp_info(core.input,valid.length,1,core.meta),-3);fixtureTests++;
assert.equal(core.api.slp_decode(core.input,valid.length,0,core.pal,0,core.image,16*1024*1024),-3);fixtureTests++;
const guard=new Uint8Array(core.api.memory.buffer,core.image,16);guard.fill(0xab);
assert.equal(core.api.slp_decode(core.input,valid.length,0,core.pal,1,core.image,7),-5);
assert.ok(guard.every(x=>x===0xab));fixtureTests++;
// Malformed pointers and table counts must return errors, never trap the WASM VM.
assert.equal(core.api.slp_info(core.api.memory.buffer.byteLength-1,100,0,core.meta),-2);fixtureTests++;
let fuzzCases=0,seed=0x12345678;
for(let i=0;i<1000;i++) {
  const b=Buffer.from(valid);
  seed=(Math.imul(seed,1664525)+1013904223)>>>0;b[seed%b.length]=(seed>>>16)&255;
  core.load(b);core.palette(palette);
  assert.doesNotThrow(()=>core.api.slp_decode(core.input,b.length,0,core.pal,1,core.image,16*1024*1024));fuzzCases++;
}
const manifest=JSON.parse(await readFile(new URL('web/game/manifest.json',root)));
const pa=manifest.archives.find(x=>x.path==='data/Interfac.drs');
const pb=await readFile(new URL('web/'+pa.url,root));const pe=pa.entries.find(x=>x.id===50500);
const realPalette=pb.subarray(pe.offset,pe.offset+pe.size);
let archives=0,resources=0,sprites=0,frames=0,palettes=0;const errors=[];
const started=performance.now();
for(const archive of manifest.archives) {
  const b=await readFile(new URL('web/'+archive.url,root));core.load(b);core.palette(realPalette);
  const entries=core.entries();
  assert.deepEqual(entries,archive.entries.map(({type,id,offset,size})=>({type,id,offset,size})));archives++;resources+=entries.length;
  for(const entry of entries) {
    if(archive.entries.find(x=>x.type===entry.type&&x.id===entry.id)?.palette) {
      core.palette(b.subarray(entry.offset,entry.offset+entry.size));palettes++;core.palette(realPalette);
    }
    if(entry.type!=='slp')continue;
    sprites++;let info;
    try{info=core.info(entry);}catch(e){errors.push({archive:archive.path,id:entry.id,error:e.message});continue;}
    for(let frame=0;frame<info.frames;frame++) {
      const n=core.api.slp_decode(core.input+entry.offset,entry.size,frame,core.pal,1,core.image,16*1024*1024);
      if(n<0)errors.push({archive:archive.path,id:entry.id,frame,code:n});else frames++;
    }
  }
}
const report={wasm_bytes:bytes.length,wasm_imports:0,fixture_tests:fixtureTests,fuzz_cases:fuzzCases,
              archives,resources,sprites,frames,palettes,errors,corpus_seconds:+((performance.now()-started)/1000).toFixed(3),
              scope:'Asset parsing and RGBA preview only. No gameplay or pixel comparison with Windows execution.'};
await writeFile(new URL('reports/tests.json',root),JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
assert.equal(errors.length,0);assert.equal(frames,manifest.summary.frames);
