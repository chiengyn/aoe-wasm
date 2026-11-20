#!/usr/bin/env python3
"""Verify the retail RNG bytes before relating its arithmetic to the WASM port."""
from pathlib import Path
import argparse, hashlib, json, struct
ROOT=Path(__file__).resolve().parents[1]
EXPECTED='a8d8cfb241041f756416a74faa0fa77d894418a8c1bac2d7d06f2aeefa8b4b72'
RAND=bytes.fromhex('a1843656008d0c408d1488c1e20403d0c1e2082bd08d8490c39e2600a384365600c1f81025ff7f0000c3')
SEED=bytes.fromhex('8b442404a384365600c3')
def main():
    p=argparse.ArgumentParser();p.add_argument('exe',type=Path);a=p.parse_args();data=a.exe.read_bytes();digest=hashlib.sha256(data).hexdigest()
    if digest!=EXPECTED:raise SystemExit('Refusing to infer addresses for a different executable hash')
    u16=lambda o:struct.unpack_from('<H',data,o)[0]
    u32=lambda o:struct.unpack_from('<I',data,o)[0]
    pe=u32(60);opt=pe+24;image_base=u32(opt+28);sections=opt+u16(pe+20)
    def at(va,size):
        rva=va-image_base
        for i in range(u16(pe+6)):
            o=sections+40*i;start=u32(o+12);length=u32(o+16);raw=u32(o+20)
            if start<=rva and rva+size<=start+length:return data[raw+rva-start:raw+rva-start+size]
        raise ValueError('Address is outside raw sections')
    assert at(0x528420,len(RAND))==RAND
    assert at(0x528410,len(SEED))==SEED
    report={'passed':True,'exe_sha256':digest,'seed_va':'0x00528410','rand_va':'0x00528420','state_va':'0x00563684',
            'seed_bytes':SEED.hex(),'rand_bytes':RAND.hex(),
            'derivation':'lea/shl/add/sub computes 214013*state + 2531011 modulo 2^32; SAR16 and AND0x7fff returns 15 bits',
            'runtime':'engine/platform/runtime.cpp','scope':'Static verification of these two functions only; not a retail gameplay execution test'}
    (ROOT/'reports/retail-rng.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
