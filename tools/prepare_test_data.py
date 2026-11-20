#!/usr/bin/env python3
"""Prepare the test data used by the probe and format-reader tests (not needed to play).

Reads an existing installation without running it: inventories every file (reports/), copies the DRS
archives into web/game/ with a manifest, then runs prepare_dat, prepare_engine and export_renderer.
"""
from pathlib import Path
import argparse, collections, hashlib, json, shutil, struct, zlib, datetime
ROOT=Path(__file__).resolve().parents[1]
U32=lambda b,o: struct.unpack_from('<I',b,o)[0]
def sha(b): return hashlib.sha256(b).hexdigest()
def drs_entries(b):
    if len(b)<64 or b[40:44]!=b'1.00': raise ValueError('Unsupported DRS header')
    for t in range(U32(b,56)):
        ext,offset,count=struct.unpack_from('<4sII',b,64+12*t)
        kind=ext[::-1].decode('ascii').strip()
        for i in range(count):
            rid,at,size=struct.unpack_from('<III',b,offset+12*i)
            if at+size>len(b): raise ValueError('Resource out of bounds')
            yield {'type':kind,'id':rid,'offset':at,'size':size}
def pe_report(b):
    pe=U32(b,60);coff=pe+4;opt=coff+20
    machine,nsec,timestamp=struct.unpack_from('<HHI',b,coff)
    optlen=struct.unpack_from('<H',b,coff+16)[0]
    sections=[]
    for n in range(nsec):
        at=opt+optlen+n*40
        vs,va,rs,rp=struct.unpack_from('<IIII',b,at+8)
        sections.append({'name':b[at:at+8].split(b'\0')[0].decode(),'virtual_size':vs,'rva':va,'raw_size':rs,'raw_offset':rp})
    def offset(rva):
        for s in sections:
            if s['rva']<=rva<s['rva']+s['raw_size']:return s['raw_offset']+rva-s['rva']
        raise ValueError('RVA outside file')
    def cstring(rva):
        at=offset(rva);end=b.index(0,at);return b[at:end].decode('ascii')
    imports=[];di=offset(U32(b,opt+104))
    while any(b[di:di+20]):
        lookup,_,__,name,first=struct.unpack_from('<IIIII',b,di);thunk=offset(lookup or first);functions=[]
        while U32(b,thunk):
            v=U32(b,thunk)
            functions.append('#'+str(v&65535) if v&0x80000000 else cstring(v+2));thunk+=4
        imports.append({'dll':cstring(name),'functions':functions});di+=20
    sig=b.find(bytes.fromhex('bd04effe'));version=None
    if sig>=0:
        ms,ls=struct.unpack_from('<II',b,sig+8);version=f'{ms>>16}.{ms&65535}.{ls>>16}.{ls&65535}'
    return {'machine':hex(machine),'architecture':'x86-32','format':'PE32','file_version':version,
            'linker_timestamp_utc':datetime.datetime.fromtimestamp(timestamp,datetime.timezone.utc).isoformat(),
            'entry_point':hex(U32(b,opt+28)+U32(b,opt+16)),
            'sections':sections,'imports':imports,'import_count':sum(len(x['functions']) for x in imports)}
def main():
    p=argparse.ArgumentParser();p.add_argument('game_directory',type=Path);a=p.parse_args();src=a.game_directory.resolve()
    if not src.is_dir():raise SystemExit('Game directory not found')
    reportdir=ROOT/'reports';reportdir.mkdir(exist_ok=True)
    files=sorted(x for x in src.rglob('*') if x.is_file());inventory=[];archives=[];palettes=[];binaries=[];dat=[]
    for f in files:
        b=f.read_bytes();rel=f.relative_to(src).as_posix();inventory.append({'path':rel,'size':len(b),'sha256':sha(b)})
        if f.suffix.lower()=='.exe':
            binaries.append({'path':rel,'sha256':sha(b),**pe_report(b)})
        if f.suffix.lower()=='.dat':
            try:
                d=zlib.decompress(b,-15)
                dat.append({'path':rel,'compressed_size':len(b),'decoded_size':len(d),'version':d[:8].decode('ascii').rstrip('\0'),'decoded_sha256':sha(d)})
            except zlib.error:pass
        if f.suffix.lower()!='.drs':continue
        entries=list(drs_entries(b))
        for e in entries:
            d=b[e['offset']:e['offset']+e['size']]
            if e['type']=='slp':
                e['version']=d[:4].decode();e['frames']=U32(d,4)
                e['width'],e['height'],e['hotspot_x'],e['hotspot_y']=struct.unpack_from('<iiii',d,48)
                # Union of frame bounds around the source hotspot stabilizes animation.
                bounds=[struct.unpack_from('<iiii',d,48+32*f) for f in range(e['frames'])]
                e['bounds']=[min(-x for w,h,x,y in bounds),min(-y for w,h,x,y in bounds),max(w-x for w,h,x,y in bounds),max(h-y for w,h,x,y in bounds)]
            elif e['type']=='bina' and d.startswith(b'JASC-PAL'):
                e['palette']=True;palettes.append({'archive':rel,'id':e['id']})
        target=ROOT/'web/game'/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,target)
        archives.append({'path':rel,'url':'game/'+rel,'size':len(b),'sha256':sha(b),'entries':entries})
    slps=[e for ar in archives for e in ar['entries'] if e['type']=='slp']
    summary={'file_count':len(files),'total_bytes':sum(x['size'] for x in inventory),'archive_count':len(archives),
             'resources':sum(len(x['entries']) for x in archives),'sprites':len(slps),'frames':sum(x['frames'] for x in slps),
             'palettes':len(palettes),'source_files_found':[x['path'] for x in inventory if Path(x['path']).suffix.lower() in ('.c','.cpp','.h','.wasm','.pdb')]}
    manifest={'schema':1,'summary':summary,'archives':archives,'palettes':palettes}
    (ROOT/'web/game/manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2))
    (reportdir/'inventory.json').write_text(json.dumps({'summary':summary,'files':inventory},indent=2))
    (reportdir/'binaries.json').write_text(json.dumps(binaries,indent=2))
    (reportdir/'dat.json').write_text(json.dumps(dat,indent=2))
    (reportdir/'source.json').write_text(json.dumps({'directory':str(src),'reference_commit':'6d6d8b14023aa74e6ac30a6c88229d5b818fb805','reference_exe_sha256':'88a0392120280c1b91ee90fe6f7cf50c72e55149a16c9b1ca79505572b8b9c90','reference_matches_supplied_exe':False},indent=2))
    from prepare_dat import prepare
    prepare(src)
    from prepare_engine import prepare as prepare_engine
    prepare_engine(src)
    from export_renderer import prepare as prepare_renderer
    prepare_renderer(src)
    print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
