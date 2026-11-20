#!/usr/bin/env python3
"""Inflate original DATs and read PE string-table resources without running DLLs.
The DAT structure is interpreted at runtime by dat.wasm, not by this script.
"""
from pathlib import Path
import argparse, hashlib, json, struct, zlib
ROOT=Path(__file__).resolve().parents[1]

def pe_strings(data):
    def u16(at): return struct.unpack_from('<H',data,at)[0]
    def u32(at): return struct.unpack_from('<I',data,at)[0]
    if data[:2]!=b'MZ': raise ValueError('Expected PE DLL')
    pe=u32(60)
    if data[pe:pe+4]!=b'PE\0\0': raise ValueError('Expected PE signature')
    optional=pe+24
    if u16(optional)!=0x10b: raise ValueError('Expected PE32')
    sections=optional+u16(pe+20)
    def rva(address,size=1):
        for i in range(u16(pe+6)):
            at=sections+i*40;va=u32(at+12);length=u32(at+16);offset=u32(at+20)
            if va<=address and address+size<=va+length and offset+address-va+size<=len(data):
                return offset+address-va
        raise ValueError('Resource address outside PE sections')
    base=rva(u32(optional+112));resource_size=u32(optional+116)
    def rel(at,size):
        if at<0 or at+size>resource_size: raise ValueError('Resource directory outside section')
        return base+at
    def directory(at):
        p=rel(at,16);n=u16(p+12)+u16(p+14);rel(at+16,n*8)
        return [(u32(p+16+i*8),u32(p+20+i*8)) for i in range(n)]
    strings={};languages=set()
    for type_id,type_ptr in directory(0):
        if type_id!=6: continue # RT_STRING
        if not type_ptr&0x80000000: raise ValueError('Missing resource type directory')
        for block,block_ptr in directory(type_ptr&0x7fffffff):
            if block&0x80000000 or not block_ptr&0x80000000: continue
            variants=directory(block_ptr&0x7fffffff)
            # English if available; deterministic fallback by language number.
            lang,entry=min(variants,key=lambda item:(item[0]!=1033,item[0]))
            p=rel(entry,16);size=u32(p+4);p=rva(u32(p),size);end=p+size;languages.add(lang)
            for index in range(16):
                if p+2>end: raise ValueError('Truncated string table')
                length=u16(p);p+=2
                if p+2*length>end: raise ValueError('Truncated UTF-16 string')
                value=data[p:p+2*length].decode('utf-16le');p+=2*length
                if value: strings[str((block-1)*16+index)]=value
    return strings,sorted(languages)

def prepare(src):
    specs=[('aoe','Age of Empires','data/Empires.dat',['language.dll']),
           ('ror','Rise of Rome','data2/empires.dat',['language.dll','languagex.dll'])]
    versions=[]
    for key,label,path,dlls in specs:
        compressed=(src/path).read_bytes();decoder=zlib.decompressobj(-15)
        raw=decoder.decompress(compressed,8*1024*1024)
        if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail: raise ValueError('Invalid/oversized DAT stream')
        if raw[:8]!=b'VER 3.7\0': raise ValueError('Unsupported DAT version')
        target=ROOT/f'web/game/{key}.dat.raw';target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
        strings={};sources=[]
        for dll in dlls:
            data=(src/dll).read_bytes();values,languages=pe_strings(data);strings.update(values)
            sources.append({'path':dll,'sha256':hashlib.sha256(data).hexdigest(),'languages':languages,'strings':len(values)})
        language_file=ROOT/f'web/game/{key}.strings.json';language_file.write_text(json.dumps(strings,ensure_ascii=False))
        versions.append({'key':key,'label':label,'source':path,'url':f'game/{key}.dat.raw',
                         'strings':f'game/{key}.strings.json','sha256':hashlib.sha256(raw).hexdigest(),
                         'compressed_sha256':hashlib.sha256(compressed).hexdigest(),'size':len(raw),'language_sources':sources})
    (ROOT/'web/game/versions.json').write_text(json.dumps(versions,indent=2))
    print('Prepared DAT and language resources:',[(v['key'],v['size']) for v in versions])
    return versions

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('game_directory',type=Path);a=p.parse_args();prepare(a.game_directory.resolve())
