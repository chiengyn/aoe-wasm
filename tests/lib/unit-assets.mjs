import {AssetCore} from './core.mjs';
// Original archives first, expansion overrides second.
export function resourceIndex(manifest,version){
  const result=new Map();
  const archives=manifest.archives.filter(a=>a.path.startsWith('data/')||version==='ror'&&a.path.startsWith('data2/'))
    .sort((a,b)=>Number(a.path.startsWith('data2/'))-Number(b.path.startsWith('data2/')));
  for(const archive of archives)for(const e of archive.entries)if(e.type==='slp')result.set(e.id,{...e,archive:archive.path});
  return result;
}
export function planLayers(dat,resources,id,direction,frame){
  const layers=[],issues=[];
  for(const layer of dat.layers(id,direction)){
    const g=dat.graphics[layer.graphic],entry=resources.get(g.slp);
    if(!entry){issues.push(`Missing image ${g.slp} (${g.name})`);continue;}
    let mapped;
    try{mapped=dat.frame(g.index,layer.direction,frame>=g.frames?0:frame);}catch{issues.push(`Invalid direction: ${g.name}`);continue;}
    if(mapped.index>=entry.frames){issues.push(`Image ${g.slp} has no frame ${mapped.index}`);continue;}
    layers.push({...layer,entry,mapped,level:g.layer,color:g.color});
  }
  layers.sort((a,b)=>a.level-b.level);
  return {layers,issues};
}
export class UnitAssets {
  constructor(manifest,module,cacheLimit=256){this.manifest=manifest;this.module=module;this.cacheLimit=cacheLimit;this.cores=new Map();this.bytes=new Map();this.cache=new Map();}
  async fetchArchive(path){
    if(!this.bytes.has(path))this.bytes.set(path,(async()=>{const ar=this.manifest.archives.find(a=>a.path===path);if(!ar)throw new Error('Missing archive '+path);
      const response=await fetch(ar.url);if(!response.ok)throw new Error('Cannot read '+path);return new Uint8Array(await response.arrayBuffer());})());
    return this.bytes.get(path);
  }
  async core(path){
    if(!this.cores.has(path))this.cores.set(path,(async()=>{
      const core=new AssetCore(await WebAssembly.instantiate(this.module));core.load(await this.fetchArchive(path));
      const ar=this.manifest.archives.find(a=>a.path==='data/Interfac.drs'),e=ar.entries.find(e=>e.type==='bina'&&e.id===50500);
      const data=await this.fetchArchive(ar.path);core.palette(data.subarray(e.offset,e.offset+e.size));return core;
    })());
    return this.cores.get(path);
  }
  async prepare(dat,resources,id){
    const paths=new Set(),g=dat.graphics[id];
    for(let direction=0;direction<g.angles;direction++)for(const layer of dat.layers(id,direction)){
      const e=resources.get(dat.graphics[layer.graphic].slp);if(e)paths.add(e.archive);
    }
    await Promise.all([...paths].map(path=>this.core(path)));
    this.ready=new Map(await Promise.all([...this.cores].map(async([k,v])=>[k,await v])));
  }
  image(layer,player){
    const {entry,mapped}=layer,key=`${entry.archive}:${entry.id}:${mapped.index}:${player}`;
    if(this.cache.has(key))return this.cache.get(key);
    const core=this.ready.get(entry.archive);if(!core)throw new Error('Image archive not loaded yet.');
    const data=core.decode(entry,mapped.index,player),canvas=document.createElement('canvas');
    canvas.width=data.width;canvas.height=data.height;
    canvas.getContext('2d').putImageData(new ImageData(data.rgba,data.width,data.height),0,0);
    const result={...data,rgba:undefined,canvas};if(this.cache.size>=this.cacheLimit)this.cache.delete(this.cache.keys().next().value);this.cache.set(key,result);return result;
  }
}
