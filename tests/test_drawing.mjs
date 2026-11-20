import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import createEngine from '../engine/build/core-probe.mjs';
const sha=b=>createHash('sha256').update(b).digest('hex');
const input=JSON.parse(fs.readFileSync(process.argv[2]));
const wasmBinary=fs.readFileSync(new URL('../engine/build/core-probe.wasm',import.meta.url));
const m=await createEngine({wasmBinary,print:()=>{}});
let checks=0;
for(const c of input.cases){
 const raw=new Uint8Array(96+c.data.length);new Int32Array(raw.buffer,0,24).set(c.v);raw.set(c.data,96);
 const ptr=m.ccall('drawing_probe','number',['array'],[raw]);
 const got=sha(m.HEAPU8.subarray(ptr,ptr+c.v[2]*c.v[3]+16));
 assert.equal(got,c.sha256,'retail renderer case '+checks+' '+JSON.stringify(c.v));checks++;
}
const report={passed:true,checks,wasmSha256:sha(wasmBinary),exeSha256:input.exeSha256,scope:'Byte-for-byte indexed surfaces against emulated functions from the supplied retail EXE; clipping, alignment, span gaps, mirror, player color, shadow, outline and HLine.'};
fs.writeFileSync(new URL('../reports/drawing-tests.json',import.meta.url),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report,null,2));
