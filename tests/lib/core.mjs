// Minimal JS host. DRS parsing, SLP decoding and palette parsing run in WASM.
export class AssetCore {
  constructor(instance) {
    this.api=instance.exports;
    this.base=this.api.__heap_base.value;
    this.meta=this.base;
    this.pal=this.base+64;
    this.input=this.base+2048;
    this.image=this.input;
    this.length=0;
  }
  ensure(bytes) {
    const mem=this.api.memory;
    if (mem.buffer.byteLength<bytes) mem.grow(Math.ceil((bytes-mem.buffer.byteLength)/65536));
  }
  load(bytes) {
    if (bytes.byteLength>64*1024*1024) throw new Error('File exceeds the 64 MB limit.');
    this.length=bytes.byteLength;
    this.image=(this.input+this.length+15)&~15;
    this.ensure(this.image+4*4096*1024);
    new Uint8Array(this.api.memory.buffer,this.input,this.length).set(bytes);
  }
  check(code) {
    const reasons={
      '-1':'Unsupported format','-2':'Truncated data or offset past end of file','-3':'Index out of range',
      '-4':'Invalid image size','-5':'Image buffer too small','-6':'Unsupported SLP command',
      '-7':'Corrupt image row','-8':'Invalid palette',
    };
    if(code<0) throw new Error(`${reasons[code]||'Decode error'} (${code})`);
    return code;
  }
  entries() {
    const n=this.check(this.api.drs_count(this.input,this.length)),result=[];
    for(let i=0;i<n;i++) {
      this.check(this.api.drs_entry(this.input,this.length,i,this.meta));
      const [type,id,offset,size]=new Uint32Array(this.api.memory.buffer,this.meta,4);
      const name=String.fromCharCode(type>>>24,(type>>>16)&255,(type>>>8)&255,type&255).trim();
      result.push({type:name,id,offset,size});
    }
    return result;
  }
  palette(bytes) {
    // Separate scratch: preserves the loaded archive.
    const ptr=this.image+4*4096*1024;
    this.ensure(ptr+bytes.length);
    new Uint8Array(this.api.memory.buffer,ptr,bytes.length).set(bytes);
    this.check(this.api.palette_decode(ptr,bytes.length,this.pal,1024));
  }
  info(entry,frame=0) {
    this.check(this.api.slp_info(this.input+entry.offset,entry.size,frame,this.meta));
    const [width,height,x,y,frames,properties]=new Int32Array(this.api.memory.buffer,this.meta,6);
    return {width,height,x,y,frames,properties};
  }
  decode(entry,frame=0,player=1) {
    const info=this.info(entry,frame);
    const size=this.check(this.api.slp_decode(this.input+entry.offset,entry.size,frame,this.pal,player,this.image,4*4096*1024));
    const rgba=new Uint8ClampedArray(this.api.memory.buffer,this.image,size).slice();
    return {...info,rgba};
  }
}
