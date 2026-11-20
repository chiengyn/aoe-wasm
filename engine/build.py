#!/usr/bin/env python3
"""Compile the recovered C++ to wasm32 objects, then link the game and/or the headless probe.

--game links the real WinMain build (engine/build/game.{mjs,wasm}; --release: engine/build-release/).
--probe links the headless diagnostic used by the Node tests (engine/build/core-probe.{mjs,wasm}).
Unported Windows calls abort with their name in both builds.
"""
from pathlib import Path
import argparse, subprocess, json, concurrent.futures, hashlib, sys, shutil
ROOT = Path(__file__).resolve().parent
BUILD = ROOT / 'build'
FLAGS = ['-std=c++98', '-O1', '-g', '-fms-extensions', '-fshort-wchar',
         '-fwritable-strings', '-fno-strict-aliasing', '-fwrapv', '-Wno-everything', '-ferror-limit=0']
# --release: optimized, no debug info, in its own folder (the debug build stays for diagnosis).
RELEASE_FLAGS = ['-std=c++98', '-O2', '-fno-strict-return', '-fno-delete-null-pointer-checks', '-fms-extensions', '-fshort-wchar',
                 '-fwritable-strings', '-fno-strict-aliasing', '-fwrapv', '-Wno-everything', '-ferror-limit=0']
RELEASE = False
# Recovered files that misbehave at -O2 and stay at -O1 in the release build. com_hand.cpp (network/lockstep):
# at -O2 a match crashes later in sprite drawing (memory overwritten); found by bisecting objects. The undefined
# behavior inside it is not pinned down yet; the file is not performance-critical.
RELEASE_O1 = {'com_hand.cpp'}
NAMES = False
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', nargs='+', help='Compile selected stems; does not replace the full-build report')
    parser.add_argument('--probe', action='store_true', help='Link the headless diagnostic after a successful full compile')
    parser.add_argument('--web', action='store_true', help='With --game: copy game.mjs/.wasm into web/engine/ for the server')
    parser.add_argument('--relink', action='store_true', help='Reuse a complete build only when all source, object and header hashes still match')
    parser.add_argument('--game', action='store_true', help='Also link the real WinMain build (game.mjs/.wasm) with Asyncify')
    parser.add_argument('--release', action='store_true', help='Optimized game build without debug info, in engine/build-release')
    parser.add_argument('--names', action='store_true', help='With --release: keep function names (--profiling-funcs) for readable stack traces')
    args = parser.parse_args()
    global BUILD, FLAGS, RELEASE, NAMES
    NAMES = args.names
    if args.release:
        if args.probe or args.only:
            parser.error('--release builds the game only (no --probe/--only)')
        BUILD, FLAGS, RELEASE = ROOT / 'build-release', RELEASE_FLAGS, True
    if args.only and (args.probe or args.game):
        parser.error('--probe/--game require a full build')
    if args.web and not args.game:
        parser.error('--web requires --game')
    if args.relink and (not (args.probe or args.game) or args.only):
        parser.error('--relink requires --probe or --game and a complete cached build')
    compiler = args.sdk.resolve() / 'upstream/emscripten/em++'
    if not compiler.is_file():
        parser.error('Emscripten em++ not found in --sdk')
    BUILD.mkdir(exist_ok=True)
    sources = sorted((ROOT / 'upstream/src').glob('*.cpp'))
    if args.only:
        unknown = set(args.only) - {s.stem for s in sources}
        if unknown: parser.error('Unknown source stems: ' + ', '.join(sorted(unknown)))
        sources = [s for s in sources if s.stem in args.only]
    common = [str(compiler), *FLAGS, '-I', str(ROOT / 'platform'), '-include', str(ROOT / 'platform/port_prefix.h')]
    # platform/win32_*.h are private to the real-WinMain platform modules and never reach upstream objects.
    headers = sorted(p for folder in ['platform', 'upstream/include', 'upstream/third_party'] for p in (ROOT / folder).rglob('*.h')
                     if not p.name.startswith('win32_'))
    header_hash = hashlib.sha256(''.join(str(p.relative_to(ROOT)) + digest(p) for p in headers).encode()).hexdigest()
    def compile_one(source):
        target = BUILD / (source.stem + '.o')
        pending = BUILD / (source.stem + '.pending.o')
        target.unlink(missing_ok=True)
        flags = common
        if RELEASE and source.name in RELEASE_O1:
            flags = [('-O1' if f == '-O2' else f) for f in common]
        result = subprocess.run([*flags, '-c', str(source), '-o', str(pending)], capture_output=True, text=True)
        (BUILD / (source.stem + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode == 0: pending.replace(target)
        else: pending.unlink(missing_ok=True)
        return {'source': source.name, 'source_sha256': digest(source), 'success': result.returncode == 0,
                'object_sha256': digest(target) if target.exists() else None,
                'errors': [line for line in result.stderr.splitlines() if 'error:' in line]}
    if args.relink:
        cached = json.loads((BUILD / 'compile.json').read_text())
        assert cached['header_sha256'] == header_hash and cached['flags'] == FLAGS
        assert cached['compiler'] == str(compiler) and cached['passed'] == cached['total'] == len(sources)
        units = cached['units']
        assert {u['source'] for u in units} == {s.name for s in sources}
        for unit in units:
            assert unit['source_sha256'] == digest(ROOT / 'upstream/src' / unit['source']), unit['source']
            assert unit['object_sha256'] == digest((BUILD / unit['source']).with_suffix('.o')), unit['source']
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            units = list(pool.map(compile_one, sources))
    report = {'stage': 'compile_only', 'compiler': str(compiler), 'flags': FLAGS, 'header_sha256': header_hash,
              'passed': sum(unit['success'] for unit in units), 'total': len(units), 'units': units}
    report_path = BUILD / ('compile-subset.json' if args.only else 'compile.json')
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['stage', 'passed', 'total']}), flush=True)
    if report['passed'] != report['total']:
        for unit in units:
            if not unit['success']: print(unit['source'], unit['errors'][:2])
        return 1
    status = 0
    if args.game:
        status = link_game(common, units, args.web) or status
    if args.probe:
        status = link_probe(common, units) or status
    return status

def copy_to_web(output, link, name):
    web = ROOT.parent / 'web/engine'
    web.mkdir(exist_ok=True)
    for extension in ['.mjs', '.wasm']:
        shutil.copy2(output.with_suffix(extension), web / output.with_suffix(extension).name)
    (web / name).write_text(json.dumps(link, indent=2) + '\n')

# Real executable: upstream main.cpp's WinMain runs RGE_Base_Game::run(), whose message loop yields to the
# browser through Asyncify (platform/win32_loop.cpp). Unported Windows calls still abort with their name.
GAME_LOCAL = ['platform/win32_loop.cpp', 'platform/port_glue.cpp', 'platform/runtime.cpp', 'platform/guids.cpp',
              'platform/software_draw.cpp', 'platform/win32_common.cpp', 'platform/win32_kernel.cpp',
              'platform/win32_user.cpp', 'platform/win32_gdi.cpp', 'platform/win32_ddraw.cpp', 'platform/win32_inflate.cpp', 'platform/win32_media.cpp', 'platform/win32_dplay.cpp', 'platform/win32_testhooks.cpp']
def link_game(common, units, web):
    output = BUILD / 'game.mjs'
    for extension in ['.mjs', '.wasm']: output.with_suffix(extension).unlink(missing_ok=True)
    objects = [BUILD / Path(unit['source']).with_suffix('.o') for unit in units]
    # The original compressor (Deflate.c, C) for saved games, scenarios and player profiles.
    deflate = BUILD / 'Deflate.c.o'
    c_opt = ['-O2'] if RELEASE else ['-O1', '-g']
    result = subprocess.run([str(Path(common[0]).with_name('emcc')), *c_opt, '-fno-strict-aliasing', '-fwrapv', '-Wno-everything',
                             '-c', str(ROOT / 'upstream/src/Deflate.c'), '-o', str(deflate)], capture_output=True, text=True)
    if result.returncode:
        print(result.stderr[:4000], file=sys.stderr)
        return result.returncode
    objects.append(deflate)
    command = [*common, '-DAOE_GAME_LOOP=1', *[str(ROOT / p) for p in GAME_LOCAL], *map(str, objects),
               '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sEXPORT_NAME=createGame', '-sALLOW_MEMORY_GROWTH=1',
               '-sINITIAL_MEMORY=67108864', '-sSTACK_SIZE=8388608', '-sASSERTIONS=0' if RELEASE else '-sASSERTIONS=2', '-sERROR_ON_UNDEFINED_SYMBOLS=1',
               '-sASYNCIFY=1', '-sASYNCIFY_STACK_SIZE=262144', '-sEXIT_RUNTIME=0', '-Wl,--error-limit=0',
               '-Wl,--wrap=open', '-Wl,--wrap=unlink', '-Wl,--wrap=chdir',
               *(['--profiling-funcs'] if RELEASE and NAMES else []),
               '-sINCOMING_MODULE_JS_API=["wasmBinary","print","printErr","preRun","onAbort","onExit","arguments","noInitialRun"]',
               '-sEXPORTED_FUNCTIONS=["_main","_malloc","_free"]',
               '-sEXPORTED_RUNTIME_METHODS=["FS","ccall","callMain","HEAPU8","HEAP32"]', '--js-library', str(ROOT / 'diagnostics/unsupported.js'),
               '-o', str(output)]
    result = subprocess.run(command, capture_output=True, text=True)
    (BUILD / 'link-game.log').write_text(result.stdout + result.stderr)
    link = {'stage': 'real_winmain', 'release': RELEASE, 'linked': result.returncode == 0, 'playable': False, 'asyncify': True,
            'unsupported_paths_abort': True, 'release_o1': sorted(RELEASE_O1) if RELEASE else [],
            'inputs': {p: digest(ROOT / p) for p in GAME_LOCAL + ['upstream/src/Deflate.c', 'diagnostics/unsupported.js', 'build.py']}, 'objects': len(objects)}
    if result.returncode == 0:
        link['wasm_sha256'] = digest(output.with_suffix('.wasm'))
        link['wasm_bytes'] = output.with_suffix('.wasm').stat().st_size
    (BUILD / 'link-game.json').write_text(json.dumps(link, indent=2) + '\n')
    print(json.dumps(link), flush=True)
    if result.returncode == 0 and web: copy_to_web(output, link, 'game-build.json')
    if result.returncode: print(result.stderr[:8000], file=sys.stderr)
    return result.returncode

def link_probe(common, units):
    output = BUILD / 'core-probe.mjs'
    for extension in ['.mjs', '.wasm']: output.with_suffix(extension).unlink(missing_ok=True)
    local = ['diagnostics/core_probe.cpp', 'diagnostics/menu_probe.cpp', 'diagnostics/game_ui.cpp', 'diagnostics/host.cpp', 'platform/win32_loop.cpp', 'platform/port_glue.cpp', 'platform/win32_common.cpp', 'platform/runtime.cpp', 'platform/guids.cpp', 'platform/menu_host.cpp', 'platform/software_draw.cpp']
    objects = [BUILD / Path(unit['source']).with_suffix('.o') for unit in units]
    command = [*common, *[str(ROOT / p) for p in local], *map(str, objects), '--no-entry',
               '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=67108864',
               '-sSTACK_SIZE=8388608', '-sASSERTIONS=2', '-sERROR_ON_UNDEFINED_SYMBOLS=1',
               '-sINCOMING_MODULE_JS_API=["wasmBinary","print","printErr"]',
               '-sEXPORTED_RUNTIME_METHODS=["FS","ccall","HEAPU8","HEAP32"]', '--js-library', str(ROOT / 'diagnostics/unsupported.js'),
               '-o', str(output)]
    result = subprocess.run(command, capture_output=True, text=True)
    (BUILD / 'link-probe.log').write_text(result.stdout + result.stderr)
    unsupported = json.loads((ROOT / 'diagnostics/unsupported.json').read_text())['symbols']
    link = {'stage': 'headless_diagnostic', 'linked': result.returncode == 0, 'playable': False,
            'unsupported_apis': len(unsupported), 'unsupported_paths_abort': True,
            'inputs': {p: digest(ROOT / p) for p in local + ['../formats/assets.c', 'diagnostics/unsupported.js', 'diagnostics/unsupported.json', 'build.py']}, 'objects': len(objects)}
    if result.returncode == 0:
        link['wasm_sha256'] = digest(output.with_suffix('.wasm'))
        link['wasm_bytes'] = output.with_suffix('.wasm').stat().st_size
    (BUILD / 'link-probe.json').write_text(json.dumps(link, indent=2) + '\n')
    print(json.dumps(link), flush=True)
    if result.returncode: print(result.stderr[:8000], file=sys.stderr)
    return result.returncode
if __name__ == '__main__':
    raise SystemExit(main())
