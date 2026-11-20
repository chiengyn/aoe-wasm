#!/usr/bin/env python3
"""Build the standalone DRS/SLP and DAT readers into formats/build/ (freestanding WASM, or --native .so).

Needs clang and wasm-ld only; no Emscripten, Wine or x86 emulator.
"""
from pathlib import Path
import subprocess, shutil, argparse
ROOT=Path(__file__).resolve().parent
p=argparse.ArgumentParser();p.add_argument('--native',action='store_true');a=p.parse_args()
clang=shutil.which('clang')
if not clang: raise SystemExit('Install clang and lld (wasm-ld) first.')
modules={
    'assets':['drs_count','drs_entry','slp_info','palette_decode','slp_decode'],
    'dat':['dat_parse','dat_count','dat_position','dat_error_offset','dat_stage','dat_graphic','dat_unit','dat_civ','dat_task','dat_delta','dat_frame'],
}
(ROOT/'build').mkdir(exist_ok=True)
for module,exports in modules.items():
    args=[clang,'-std=c11','-O2','-Wall','-Wextra','-Werror','-fno-builtin',str(ROOT/f'{module}.c')]
    if a.native:
        args+=['-shared','-fPIC','-o',str(ROOT/f'build/lib{module}.so')]
    else:
        initial=131072 if module=='assets' else 8388608
        args+=['--target=wasm32','-nostdlib','-Wl,--no-entry','-Wl,--export-memory','-Wl,--export=__heap_base',
               f'-Wl,--initial-memory={initial}','-Wl,--max-memory=134217728','-Wl,-z,stack-size=65536']
        args+=['-Wl,--export='+name for name in exports]
        args+=['-o',str(ROOT/f'build/{module}.wasm')]
    subprocess.run(args,check=True)
    print('Built',args[-1])
