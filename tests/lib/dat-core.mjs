// The binary DAT reader and direction mapping run in C/WASM.
// This module copies the exported records into browser-friendly objects.
const fields={
  graphic:'present id offset nameOffset slp layer color frames angles speed:f duration:f pause:f flags mirror deltaStart deltaCount loaded colorFlag',
  unit:'slot id civ type offset nameOffset nameLength language group hp los:f speed:f enabled stand death undead walk run attack construction taskStart taskCount trainAt trainTime:f icon',
  civ:'nameOffset unitStart unitCount slots culture',
  task:'type id action group object terrain move work work2 carry',
  delta:'graphic x y facet',
};
export class DatCore {
  constructor(instance){this.api=instance.exports;this.base=this.api.__heap_base.value;}
  load(bytes){
    if(bytes.length>8*1024*1024)throw new Error('DAT exceeds the 8 MB limit.');
    const memory=this.api.memory;
    if(memory.buffer.byteLength<this.base+bytes.length)memory.grow(Math.ceil((this.base+bytes.length-memory.buffer.byteLength)/65536));
    new Uint8Array(memory.buffer,this.base,bytes.length).set(bytes);
    const result=this.api.dat_parse(this.base,bytes.length);
    if(result<0)throw new Error(`Invalid DAT (${result}, section ${this.api.dat_stage()}, byte ${this.api.dat_error_offset()}).`);
    this.bytes=bytes;this.counts=Array.from({length:8},(_,i)=>this.api.dat_count(i));
    this.graphics=Array.from({length:this.counts[0]},(_,i)=>this.record('graphic',i));
    this.units=Array.from({length:this.counts[1]},(_,i)=>this.record('unit',i));
    this.civs=Array.from({length:this.counts[2]},(_,i)=>this.record('civ',i));
    this.tasks=Array.from({length:this.counts[3]},(_,i)=>this.record('task',i));
    this.deltas=Array.from({length:this.counts[4]},(_,i)=>this.record('delta',i));
    for(const g of this.graphics)if(g.present)g.name=this.string(g.nameOffset,21);
    for(const u of this.units)u.name=this.string(u.nameOffset,u.nameLength);
    for(const c of this.civs)c.name=this.string(c.nameOffset,20);
    return this;
  }
  record(kind,index){
    const ptr=this.api[`dat_${kind}`](index);if(!ptr)throw new Error('Invalid DAT index.');
    const view=new DataView(this.api.memory.buffer),out={index};
    fields[kind].split(' ').forEach((f,i)=>{const [key,type]=f.split(':');out[key]=type==='f'?view.getFloat32(ptr+i*4,true):view.getInt32(ptr+i*4,true);});
    return out;
  }
  string(offset,size){return new TextDecoder('windows-1252').decode(this.bytes.subarray(offset,offset+size)).split('\0')[0];}
  frame(graphic,direction,frame){
    const packed=this.api.dat_frame(graphic,direction,frame);if(packed<0)throw new Error('Invalid direction or frame.');
    return {index:packed&0x3fffffff,mirror:!!(packed&0x40000000)};
  }
  states(unit){
    const result=[];
    const add=(key,label,id)=>{if(id>=0&&this.graphics[id]?.present)result.push({key,label,id});};
    for(const [key,label] of [['stand','Stand'],['walk','Walk'],['run','Run'],['attack','Attack'],['death','Death'],['undead','Corpse'],['construction','Under construction']])add(key,label,unit[key]);
    for(const t of this.tasks.slice(unit.taskStart,unit.taskStart+unit.taskCount)){
      for(const [key,label] of [['move','Move'],['work','Work'],['work2','Work (secondary)'],['carry','Carry resources']]){
        const id=t[key];if(result.some(s=>s.id===id))continue;
        add(`task-${t.id}-${key}`,`${label} · task ${t.id}`,id);
      }
    }
    return result;
  }
  layers(graphic,direction){
    const g=this.graphics[graphic];if(!g?.present)return [];
    if(!g.deltaCount)return [{graphic,x:0,y:0,direction}];
    // draw_list replaces the root image; -1 means the root itself.
    // The original draw() calls child do_draw(), not recursive child draw().
    return this.deltas.slice(g.deltaStart,g.deltaStart+g.deltaCount)
      .filter(d=>d.facet<0||d.facet===direction)
      .map(d=>{const id=d.graphic===-1?graphic:d.graphic,child=this.graphics[id];
        if(!child?.present)throw new Error(`Missing graphic layer ${id}.`);
        return {graphic:id,x:d.x,y:d.y,direction:Math.floor(child.angles*direction/g.angles)};
      });
  }
}
export function animationTiming(graphic,unitSpeed=0){
  const interval=graphic.duration*(graphic.speed&&unitSpeed?graphic.speed/unitSpeed:1);
  return {seconds:Math.max(.001,interval),pause:Math.max(0,graphic.pause),
    animated:!!(graphic.flags&1)&&graphic.frames>1&&(!graphic.speed||unitSpeed>0),once:!!(graphic.flags&8)};
}
export function animationFrame(graphic,timing,elapsed){
  if(!timing.animated)return {frame:0,done:true};
  const active=graphic.frames*timing.seconds;
  if(timing.once&&elapsed>=active)return {frame:graphic.frames-1,done:true};
  const phase=elapsed%(active+timing.pause);
  return {frame:Math.min(graphic.frames-1,Math.floor(phase/timing.seconds)),done:false};
}
