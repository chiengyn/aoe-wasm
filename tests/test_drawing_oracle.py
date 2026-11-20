#!/usr/bin/env python3
"""Generate indexed drawing fixtures by executing the user's retail x86 code.
Requires pefile and unicorn in an isolated environment. Never writes the EXE.
JSON contains synthetic inputs and hashes only, not executable/game bytes.
"""
import argparse,hashlib,json,random,struct
from pathlib import Path
import pefile
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--mask-table',type=Path);p.add_argument('--exe',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
exe=a.exe.read_bytes();sha=hashlib.sha256(exe).hexdigest()
assert sha=='a8d8cfb241041f756416a74faa0fa77d894418a8c1bac2d7d06f2aeefa8b4b72','Unknown executable: locate and verify function addresses first'
pe=pefile.PE(data=exe);base=pe.OPTIONAL_HEADER.ImageBase
u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(base,(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095);u.mem_write(base,pe.get_memory_mapped_image())
b=0x1000000;u.mem_map(b,0x100000);stop=b+0x60000
pack=lambda *v:struct.pack('<'+'I'*len(v),*(n&0xffffffff for n in v))
def call(addr,*args):
 sp=b+0x5f000;u.mem_write(sp,pack(stop,*args));u.reg_write(UC_X86_REG_ESP,sp);
 try:u.emu_start(addr,stop,count=2000000);assert u.reg_read(UC_X86_REG_EIP)==stop
 except Exception:
  print("Oracle failure",len(cases),hex(u.reg_read(UC_X86_REG_EIP)),hex(addr),args);raise
def frame(rows):
 result=bytearray(len(rows)*8);cursor=len(result)
 for row,cmd in enumerate(rows):
  struct.pack_into('<I',result,row*4,cursor);struct.pack_into('<HH',result,len(rows)*4+row*4,0,0);result.extend(cmd);cursor=len(result)
 return bytes(result)
# literal, skip, fill; player copy/fill and palette shadows; long literal/skip;
# empty/outlined rows and extended outline commands.
rng=random.Random(357)
frames=[
 (20,frame([[32,*range(16+r,24+r),17,0x87,83+r,15] for r in range(4)])),
 (24,frame([[0x66,0,1,2,3,4,5,0x6a,7,0x6b,0x67,105,15] for _ in range(4)])),
 (36,frame([[2,16,*[rng.randrange(256) for _ in range(16)],3,4,7,16,90,15] for _ in range(4)])),
 (20,frame([[0x1e,0x2e,3,0x87,93,0x87,70,15] for _ in range(4)]))]
# Add nonzero row outlines and an entirely transparent row.
f=bytearray(frames[0][1]);struct.pack_into('<HH',f,16,3,2);struct.pack_into('<HH',f,20,0x8000,0x8000);frames.append((25,bytes(f)))
cases=[]
def run(v,data=b''):
 h=v[2];pitch=v[3];alignment=v[4];size=h*pitch+16;surface=bytes([205])*size
 u.mem_write(b+0x20000,surface);u.mem_write(b+0x10000,pack(*(b+0x20000+y*pitch+8+alignment for y in range(h))))
 for y in range(h):
  n=b+0x11000+y*32;two=v[21]<=v[22]
  u.mem_write(n,pack(n+16 if two else 0,0,v[19],v[20],0,n,v[21],v[22]))
 u.mem_write(b+0x12000,pack(*(b+0x11000+y*32 for y in range(h))))
 u.mem_write(b+0x13000,pack(*(b+0x11000+y*32+(16 if v[21]<=v[22] else 0) for y in range(h))))
 u.mem_write(b+0x30000,bytes((i*37+11)&255 for i in range(256)))
 call(0x545720,b+0x10000,b+0x12000,b+0x13000,*v[10:14])
 call(0x545760,v[14]);call(0x545680,*v[15:19]);call(0x5457a0,b+0x30000)
 if v[0]==0:
  u.mem_write(b,data);call(0x5459a0,b,v[5],v[6],v[7],v[8],0,v[8]*4,v[9])
 else:call(0x5457e0,v[5],v[7],v[6],v[14])
 output=bytes(u.mem_read(b+0x20000,size));


 cases.append({'v':v,'data':list(data),'sha256':hashlib.sha256(output).hexdigest()});return output
if a.mask_table:
 tables=[]
 for kind in [0,6,7,11]:
  mirrored=[]
  for flip in range(2):
   lengths=[]
   for n in range(12):
    alignments=[]
    for alignment in range(4):
     if not n:alignments.append([]);continue
     command=([n*4]+[255]*n) if kind==0 else ([kind,n]+([255]*n if kind==6 else [255] if kind==7 else []))
     data=frame([command+[15]])
     v=[0,512,1,544,alignment,0,0,n,1,1+2*flip,0,0,511,0,0,0x88442211,0,0x88442211,0,0,511,1,0,len(data)]
     output=run(v,data)[8+alignment:8+alignment+n]
     lookup={0x11:0,0x22:1,0x44:2,0x88:3} if kind!=11 else {0:0,0x20:1,4:2,0x88:3}
     alignments.append([lookup[v] for v in (reversed(output) if flip else output)])
    lengths.append(alignments)
   mirrored.append(lengths)
  tables.append(mirrored)
 # Verify the repeated DWORD body independently at every remaining SLP run length.
 for ki,kind in enumerate([0,6,7,11]):
  for flip in range(2):
   for alignment in range(4):
    for n in range(12,256):
     command=(([2,n]+[255]*n) if n>63 else ([n*4]+[255]*n)) if kind==0 else ([kind,n]+([255]*n if kind==6 else [255] if kind==7 else []))
     data=frame([command+[15]]);v=[0,512,1,544,alignment,0,0,n,1,1+2*flip,0,0,511,0,0,0x88442211,0,0x88442211,0,0,511,1,0,len(data)]
     output=run(v,data)[8+alignment:8+alignment+n];lookup={0x11:0,0x22:1,0x44:2,0x88:3} if kind!=11 else {0:0,0x20:1,4:2,0x88:3};actual=[lookup[v] for v in (reversed(output) if flip else output)]
     start_alignment=(alignment+n-1)&3 if flip else alignment
     # The short-run table is indexed by destination's first written address.
     start_offset=(start_alignment-alignment)&3
     small=tables[ki][flip][8+n%4][(alignment+start_offset-((8+n%4-1) if flip else 0))&3]
     prefix=(start_alignment+1)%4 if flip else (-start_alignment)%4
     body=([3,2,1,0] if flip else [0,1,2,3])*((n-(8+n%4))//4)
     predicted=small[:prefix]+body+small[prefix:]
     assert predicted==actual,(kind,flip,alignment,n,predicted[:20],actual[:20])
 def initializer(v):return '{'+','.join(initializer(x) if isinstance(x,list) else str(x) for x in v)+'}'
 # Reindex mirror rows by the initial destination pointer, not the left edge.
 for ki in range(4):
  for n in range(1,12):
   old=tables[ki][1][n];tables[ki][1][n]=[old[(a-n+1)&3] for a in range(4)]
 a.mask_table.write_text('// Generated by test_drawing_oracle.py from retail EXE '+sha+'\n// Byte lanes of original short-run codelets; long runs repeat aligned DWORDs.\nstatic const unsigned char mask_lanes[4][2][12][4][12]='+initializer(tables)+';\n')
 cases.clear()
for fi,(width,data) in enumerate(frames):
 for flags in range(4):
  for alignment in range(4):
   for x in [-19,-2,6,23,49,65]:
    for y in [-3,0,7,14,18]:
     split=(x+alignment)%2
     v=[0,64,16,96,alignment,x,y,width,4,flags,0,0,63,15,48,0x00ff00ff,0,0xff00ff00,0,5 if split else 0,25 if split else 63,35,58 if split else 34,len(data)]
     run(v,data)
for i in range(240):
 width,data=frames[i%3];mask=[(0x00ffffff,0,0xffff00ff,0),(0xff00ff00,0x00280028,0x00ff00ff,0x28002800)][i%2]
 v=[0,64,16,96,i%4,[-9,4,42][i%3],[-3,1,7,15][i%4],width,4,1+(i%2)*2,0,2,63,15,32,*mask,5,25,35,58,len(data)];run(v,data)
for i in range(300):
 v=[1,64,16,96,i%4,rng.randrange(-20,85),rng.randrange(-5,22),rng.randrange(-20,85),0,0,3,2,60,14,rng.randrange(256),0,0,0,0,5,25,35,58,0];run(v)
# Spans wider than the min/max window (RGE_View narrows the window to one scroll strip while a tile's spans
# cover two): sprite rows clip to the spans only, the window only rejects whole sprites; HLine clamps to it.
for fi,(width,data) in enumerate(frames):
 for flags in range(4):
  for x in [-30,-2,9,23,38,41,49]:
   for split in range(2):
    v=[0,64,16,96,(fi+flags)%4,x,(fi*3)%5-1,width,4,flags,20,0,40,15,48,0x00ff00ff,0,0xff00ff00,0,0 if not split else 2,63 if not split else 30,35,34 if not split else 61,len(data)]
    run(v,data)
for i in range(100):
 v=[1,64,16,96,i%4,rng.randrange(-20,85),rng.randrange(0,16),rng.randrange(-20,85),0,0,20,0,40,15,rng.randrange(256),0,0,0,0,0,30,35,63,0];run(v)
a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps({'exeSha256':sha,'functions':{'surface':'0x545720','sprite':'0x5459a0','hline':'0x5457e0'},'cases':cases}))
print(json.dumps({'cases':len(cases),'exeSha256':sha,'out':str(a.out)}))
