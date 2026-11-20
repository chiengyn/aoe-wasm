#!/usr/bin/env python3
"""Compare two Age of Empires: Rise of Rome installations (Phase 5).

Lists files that differ or exist in only one folder, and compares the two EMPIRESX.EXE files section by
section (standard library PE parsing). Known patch sites of the retail 1.0a build (1998-09-01,
file version 00.04.2.0901) are labelled; anything else is reported as unknown. Writes
reports/version-compare.json. Nothing in either folder is executed or modified.
"""
from pathlib import Path
import argparse, hashlib, json, struct

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x400000

# Patch sites found by disassembling both EXEs (see PLAN.md, Phase 5). The retail code is described first.
KNOWN = {
    0x52b20a: ('_stricmp: result at the first differing character',
               'returns 0 ("equal") for every comparison: a blanket no-CD crack. Side effect: the five '
               'comparisons with "none" in panel loading (Panel_ez.cpp, tpnl_tml.cpp) always match, so 800x600+ '
               'screen backgrounds and optional panel pictures are skipped'),
    0x419d39: ('CD check: compares the CD volume label (CDPath)', 'forces the check to pass: a no-CD crack'),
    0x422dd1: ('TCommunications_Handler::TXMessage: sends only to human players other than me',
               'drops the IsPlayerHuman test: commands also go to computer and dropped player slots'),
    0x4eb35d: ('TRIBE_Command: TRIBE_GAME_COMMAND_ID_CHEAT runs TRIBE_World::cheat',
               'redirects cheat commands to the save-game (sync error) case: cheats never apply in multiplayer'),
    0x47d31b: ('Multiplayer Games screen (Create Dialog)', 'calls the ExWE.dll lobby add-on'),
    0x47d8ea: ('CreateMultiplayerGame: game name', 'calls ExWE.dll GetMainGameString'),
    0x48942e: ('MP Setup Screen constructor', 'calls ExWE.dll InitMPSetupScreen (adds a lobby control)'),
    0x48a879: ("MP Setup Screen: I'm Ready", 'calls ExWE.dll ExitMPSetupScreen'),
    0x48aefc: ('MP Setup Screen: settings changed', 'calls ExWE.dll SaveGameSetings'),
    0x48c682: ('MP Setup Screen: COMM_UPDATE_PARAMS', 'calls ExWE.dll LoadGameSetings'),
    0x7db2ba: ('import of DPLAYX.dll (DirectPlay)', 'imports ExWE.dll instead, which wraps DirectPlay for a lobby platform'),
}
# Which installation carries each patch, when the other has the retail bytes: decided per site by which side
# matches the compiler's code for the recovered source (sbb/movsx for _stricmp, neg/sbb/inc for the CD check).
PATCHED_IN = {0x52b20a: 'a'}


def sections(path):
    d = path.read_bytes()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    count, opt_size = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    stamp = struct.unpack_from('<I', d, pe + 8)[0]
    entry = struct.unpack_from('<I', d, pe + 24 + 16)[0]
    out = {}
    for i in range(count):
        o = pe + 24 + opt_size + i * 40
        name = d[o:o + 8].rstrip(b'\0').decode('latin1')
        vsize, va, raw_size, raw = struct.unpack_from('<IIII', d, o + 8)
        out[name] = {'va': va, 'vsize': vsize, 'data': d[raw:raw + raw_size], 'flags': struct.unpack_from('<I', d, o + 36)[0]}
    return {'timestamp': stamp, 'entry': BASE + entry, 'sections': out, 'imports': imports(d, pe, out)}


def imports(d, pe, secs):
    rva = struct.unpack_from('<I', d, pe + 24 + 104)[0]
    def at(r):
        for s in secs.values():
            if s['va'] <= r < s['va'] + max(s['vsize'], len(s['data'])):
                return s['data'], r - s['va']
        return None, 0
    names = []
    data, o = at(rva)
    while data is not None and o + 20 <= len(data):
        name_rva = struct.unpack_from('<I', data, o + 12)[0]
        if not name_rva:
            break
        nd, no = at(name_rva)
        names.append(nd[no:nd.index(b'\0', no)].decode('latin1') if nd else '?')
        o += 20
    return sorted(set(names))


def diff_ranges(a, b):
    n, i, out = min(len(a), len(b)), 0, []
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j:j + 8] != b[j:j + 8]:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def exe(folder):
    return next(p for p in folder.iterdir() if p.is_file() and p.name.lower() == 'empiresx.exe')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('a', type=Path, help='First installation (e.g. the folder this port was prepared from)')
    parser.add_argument('b', type=Path, help='Second installation')
    args = parser.parse_args()
    tree = lambda root: {p.relative_to(root).as_posix().lower(): p for p in root.rglob('*') if p.is_file()}
    ta, tb = tree(args.a), tree(args.b)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = {'a': str(args.a), 'b': str(args.b),
              'different': sorted(k for k in ta if k in tb and sha(ta[k]) != sha(tb[k])),
              'only_a': sorted(ta.keys() - tb.keys()), 'only_b': sorted(tb.keys() - ta.keys())}
    ea, eb = sections(exe(args.a)), sections(exe(args.b))
    report['exe'] = {
        'a': {'sha256': sha(exe(args.a)), 'timestamp': ea['timestamp'], 'entry': hex(ea['entry']), 'sections': list(ea['sections']), 'imports': ea['imports']},
        'b': {'sha256': sha(exe(args.b)), 'timestamp': eb['timestamp'], 'entry': hex(eb['entry']), 'sections': list(eb['sections']), 'imports': eb['imports']},
        'same_build': ea['timestamp'] == eb['timestamp'],
    }
    patches = []
    for name, sa in ea['sections'].items():
        sb = eb['sections'].get(name)
        if not sb:
            continue
        for i, j in diff_ranges(sa['data'], sb['data']):
            va = BASE + sa['va'] + i
            label = KNOWN.get(va)
            patches.append({'section': name, 'address': hex(va), 'length': j - i,
                            'a': sa['data'][i:j].hex(' '), 'b': sb['data'][i:j].hex(' '),
                            'retail': label[0] if label else None, 'patch': label[1] if label else 'unknown',
                            'patched_in': PATCHED_IN.get(va, 'b') if label else None})
    report['exe']['patches'] = patches
    report['exe']['added_sections_b'] = [n for n in eb['sections'] if n not in ea['sections']]
    out = ROOT / 'reports/version-compare.json'
    out.write_text(json.dumps(report, indent=1, ensure_ascii=False) + '\n')
    print(json.dumps({'different': len(report['different']), 'only_a': len(report['only_a']), 'only_b': len(report['only_b']),
                      'same_build': report['exe']['same_build'], 'patches': len(patches),
                      'unknown': sum(1 for p in patches if p['patch'] == 'unknown'),
                      'added_sections_b': report['exe']['added_sections_b'], 'report': str(out.relative_to(ROOT))}))


if __name__ == '__main__':
    main()
