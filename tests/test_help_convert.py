#!/usr/bin/env python3
"""Phase 3: tools/convert_help.py on the supplied EMPIRES.HLP. Checks the context hash against the hashes
stored in the file, that every topic, keyword and picture converts, and that links resolve to pages.
Writes reports/help-convert-tests.json."""
from pathlib import Path
import json, re, sys, tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import convert_help as ch

hlp = ROOT / 'web/game/native/EMPIRES.HLP'
checks = 0
def ok(value, message):
    global checks
    assert value, message
    checks += 1

hf = ch.HelpFile(hlp)
contexts = dict(hf.tree('|CONTEXT', lambda r: (r.s32(), r.s32())))
# Context names used by picture hotspots carry both the name and the stored hash.
named = [(name, h) for n in hf.files if n.startswith('|bm')
         for kind, *_, h, name in (hf.picture(n) or (None, []))[1] if kind in (0xe2, 0xe3, 0xe6, 0xe7)]
ok(len(named) > 20 and all(ch.context_hash(name) == h for name, h in named), 'Context hash matches the file for every hotspot target')
ok(all(h in contexts for _, h in named), 'Every hotspot target is a known context')

with tempfile.TemporaryDirectory() as out:
    info = ch.convert(hlp, out)
    out = Path(out)
    ok(info['topics'] >= 200 and info['pictures'] == sum(1 for n in hf.files if n.startswith('|bm')), f'All topics and pictures convert {info}')
    index = json.loads((out / 'help.json').read_text())
    ok(len(index['keywords']) >= 200 and all((out / page).is_file() for page in index['keywords'].values()), 'Keywords map to topic pages')
    ok((out / index['contents']).is_file(), 'Contents topic exists')
    links = [href for page in out.glob('t*.html') for href in re.findall(r'href="(t\d+\.html)"', page.read_text())]
    ok(len(links) > 500 and all((out / href).is_file() for href in links), f'{len(links)} topic links all resolve')
    pages_with_text = sum(1 for page in out.glob('t*.html') if len(re.sub(r'<[^>]+>', '', page.read_text())) > 400)
    ok(pages_with_text > 150, f'Most topics have text ({pages_with_text})')
    maps = sum(page.read_text().count('<area ') for page in out.glob('t*.html'))
    ok(maps > 100, f'Picture hotspots become image-map areas ({maps})')

report = {'passed': True, 'checks': checks, **info, 'hotspotNames': len(named)}
(ROOT / 'reports/help-convert-tests.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
