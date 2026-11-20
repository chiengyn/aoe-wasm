#!/usr/bin/env python3
"""Read layout operands from the supplied retail PE; do not execute its code."""
from pathlib import Path
import argparse, hashlib, json, struct
ROOT=Path(__file__).resolve().parents[1]
EXPECTED='a8d8cfb241041f756416a74faa0fa77d894418a8c1bac2d7d06f2aeefa8b4b72'
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('exe',type=Path);args=parser.parse_args()
    data=args.exe.read_bytes();digest=hashlib.sha256(data).hexdigest()
    if digest!=EXPECTED:raise SystemExit('This address map belongs to a different EXE; refusing to infer results')
    u16=lambda o:struct.unpack_from('<H',data,o)[0]
    u32=lambda o:struct.unpack_from('<I',data,o)[0]
    pe=u32(60);opt=pe+24;base=u32(opt+28);sections=opt+u16(pe+20)
    def at(va,size):
        rva=va-base
        for i in range(u16(pe+6)):
            pos=sections+40*i;start=u32(pos+12);length=u32(pos+16);raw=u32(pos+20)
            if start<=rva and rva+size<=start+length:return data[raw+rva-start:raw+rva-start+size]
        raise ValueError('Outside raw PE sections')
    evidence=[]
    def immediate(va,opcode,size):
        prefix=bytes.fromhex(opcode);assert at(va,len(prefix))==prefix,hex(va)
        value=int.from_bytes(at(va+len(prefix),size),'little')
        evidence.append({'va':hex(va),'bytes':at(va,len(prefix)+size).hex(),'value':value})
        return value
    def call(va,target):
        assert at(va,1)==b'\xe8'
        actual=va+5+struct.unpack('<i',at(va+1,4))[0];assert actual==target
        evidence.append({'va':hex(va),'call':hex(actual)})
    menus=[]
    for spec in [
        dict(name='Main Menu',constructor=0x4879b0,end=0x487ebc,config=0x487a3b,ideal=[0x487a68,0x487a63],y=(0x487a74,'be'),x=0x487a89,w=0x487a83,h=0x487a81,step=(0x487aa6,'83c6'),end_y=(0x487aac,'81fe'),create=0x487a99,labels=[0x487aba,0x487acc,0x487ade,0x487af0,0x487b02]),
        dict(name='Single Player Menu',constructor=0x49f5f0,end=0x49f82d,config=0x49f64e,ideal=[0x49f69c,0x49f697],y=(0x49f6dd,'bf'),x=0x49f6f2,w=0x49f6ec,h=0x49f6ea,step=(0x49f70f,'83c7'),end_y=(0x49f715,'81ff'),create=0x49f702,labels=[0x49f775,0x49f787,0x49f799,0x49f7ab,0x49f7bd,0x49f7cf]),
    ]:
        config=immediate(spec['config'],'68',4)
        ideal=[immediate(va,'68',4) for va in spec['ideal']]
        y=immediate(*spec['y'],4);x=immediate(spec['x'],'68',4)
        width=immediate(spec['w'],'68',4);height=immediate(spec['h'],'6a',1)
        step=immediate(*spec['step'],1);end_y=immediate(*spec['end_y'],4)
        call(spec['create'],0x456ce0)
        labels=[immediate(va,'68',4) for va in spec['labels']]
        assert len(range(y,end_y,step))==len(labels)
        menus.append({'name':spec['name'],'constructor_va':hex(spec['constructor']),'config_id':config,'ideal_size':ideal,
          'buttons':[{'rect':[x,row,width,height],'string_id':label} for row,label in zip(range(y,end_y,step),labels)]})
    assert menus[0]['config_id']==50051 and menus[1]['config_id']==50052
    report={'passed':True,'exe_sha256':digest,'menus':menus,'instructions':evidence,
      'scope':'Static retail constructor layout operands and create_button call target only. No claim of whole-function, font raster, event timing, gameplay or beta-to-retail equivalence.'}
    (ROOT/'reports/retail-ui.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':True,'exe_sha256':digest,'menus':menus},indent=2))
if __name__=='__main__':main()
