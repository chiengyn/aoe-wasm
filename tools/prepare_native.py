#!/usr/bin/env python3
"""Mirror the supplied game folder for the real-WinMain build (web/game/native + web/game/native.json).

The engine's filesystem layer lower-cases Windows paths, so files are written to the WASM filesystem in
lower case; the manifest keeps the original names. The folder is only read, never modified.

Intro videos (avi/*.avi: MS Video 1 and Indeo 5, which browsers can't decode) are converted with the local
ffmpeg to WebM (VP9/Opus) in web/game/video; the game still opens the original .avi to check it exists.
The WinHelp file (EMPIRES.HLP) is converted to HTML pages in web/game/help (tools/convert_help.py).
"""
from pathlib import Path
import argparse, hashlib, json, re, shutil, subprocess
from convert_help import convert as convert_help

ROOT = Path(__file__).resolve().parents[1]
# Output folder under web/ (--out): 'game' by default; another name keeps a second edition beside it,
# opened with native.html?data=<name>.
OUT = VIDEO = HELP = MANIFEST = None
PREFIX = 'game'


def set_output(name):
    global OUT, VIDEO, HELP, MANIFEST, PREFIX
    PREFIX = name
    OUT, VIDEO, HELP = ROOT / 'web' / name / 'native', ROOT / 'web' / name / 'video', ROOT / 'web' / name / 'help'
    MANIFEST = ROOT / 'web' / name / 'native.json'
# Installer, archive and documents are not used by the game at runtime.
SKIP_NAMES = {'setup.exe', 'empiresx.rar', 'desktop.ini',
              # Third-party add-ons in some editions (DirectPlay installer, lobby/windowing wrappers): not game files.
              'dplay60a.exe', 'setupreg.exe', 'dplayx.dll', 'd3dhook.dll', 'exwe.dll', 'hook.ini', 'tcf.cfg', 'gameport.dat'}
# Programs other than the game itself (patches, tools) are never run by the game.
KEEP_EXE = {'empires.exe', 'empiresx.exe'}
SKIP_SUFFIXES = {'.doc', '.rtf'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='Game folder (contains EMPIRESX.EXE)')
    parser.add_argument('--ffmpeg', default='ffmpeg', help='ffmpeg used to convert the intro videos')
    parser.add_argument('--out', default='game', help='Output folder under web/ (default: game)')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,40}', args.out):
        parser.error('--out must be a short lower-case folder name')
    set_output(args.out)
    game = args.game.resolve()
    if not any(p.name.lower() == 'empiresx.exe' for p in game.iterdir() if p.is_file()):
        parser.error('EMPIRESX.EXE not found in ' + str(game))
    if OUT.exists():
        shutil.rmtree(OUT)
    files, seen = [], set()
    for source in sorted(p for p in game.rglob('*') if p.is_file()):
        relative = source.relative_to(game).as_posix()
        if source.name.lower() in SKIP_NAMES or source.suffix.lower() in SKIP_SUFFIXES \
                or (source.suffix.lower() == '.exe' and source.name.lower() not in KEEP_EXE and source.parent == game) \
                or not relative.isascii():
            continue
        if relative.lower() in seen:
            raise SystemExit('Case-insensitive name clash: ' + relative)
        seen.add(relative.lower())
        target = OUT / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        data = target.read_bytes()
        files.append({'path': relative, 'url': f'{PREFIX}/native/' + relative,
                      'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)})
    directories = sorted({str(Path(f['path']).parent).lower() for f in files if '/' in f['path']} |
                         {'savegame', 'scenario', 'campaign', 'sound', 'avi'})
    videos = convert_videos(files, args.ffmpeg)
    hlp = next((OUT / f['path'] for f in files if f['path'].lower() == 'empires.hlp'), None)
    help_info = convert_help(hlp, HELP) if hlp else None
    MANIFEST.write_text(json.dumps({'schema': 1, 'directories': directories, 'files': files, 'videos': videos,
                                     'help': f'{PREFIX}/help/' if help_info else None}, indent=1) + '\n')
    print(json.dumps({'files': len(files), 'bytes': sum(f['size'] for f in files), 'videos': sorted(videos), 'help': help_info,
                      'manifest': str(MANIFEST.relative_to(ROOT))}))


def convert_videos(files, ffmpeg):
    """avi/<name>.avi -> game/video/<name>.webm; returns {name: {url, width, height, source_sha256}}."""
    if VIDEO.exists():
        shutil.rmtree(VIDEO)
    VIDEO.mkdir(parents=True)
    videos = {}
    for f in files:
        if not f['path'].lower().endswith('.avi'):
            continue
        source, name = OUT / f['path'], Path(f['path']).stem.lower()
        probe = subprocess.run([ffmpeg.replace('ffmpeg', 'ffprobe'), '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                                'stream=width,height', '-of', 'json', str(source)], capture_output=True, text=True, check=True)
        stream = json.loads(probe.stdout)['streams'][0]
        target = VIDEO / (name + '.webm')
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', str(source), '-c:v', 'libvpx-vp9', '-crf', '20', '-b:v', '0',
                        '-row-mt', '1', '-pix_fmt', 'yuv420p', '-c:a', 'libopus', '-b:a', '96k', str(target)], check=True)
        videos[name] = {'url': f'{PREFIX}/video/' + target.name, 'width': stream['width'], 'height': stream['height'],
                        'source_sha256': f['sha256']}
    return videos


if __name__ == '__main__':
    main()
