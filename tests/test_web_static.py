#!/usr/bin/env python3
"""Static checks of the browser page without starting a browser.

Checks local links in web/*.html, that every element ID the pages' scripts look up (getElementById() in
native.html, $('...') in lobby.mjs for index.html) names an element on the page, that every web module parses (node --check) and that relative imports resolve. With --built, also checks that
the game build has been copied to web/engine/ (engine/build.py --game --web). Writes reports/web-static.json.
"""
from pathlib import Path
from html.parser import HTMLParser
import argparse, json, re, subprocess
ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'web'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--node', default='node')
p.add_argument('--built', action='store_true', help='also require web/engine/game.{mjs,wasm}')
args = p.parse_args()

class Page(HTMLParser):
    def __init__(self):
        super().__init__(); self.ids = []; self.references = []
    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        if 'id' in d: self.ids.append(d['id'])
        for name in ['src', 'href']:
            if name in d: self.references.append(d[name])

pages = {}
for path in sorted(WEB.glob('*.html')):
    page = Page(); text = path.read_text(); page.feed(text)
    assert len(page.ids) == len(set(page.ids)), f'duplicate id in {path.name}'
    pages[path.name] = page
    for target in page.references:
        if not target.startswith(('http:', 'https:', '#', 'data:')):
            assert (WEB / target / 'index.html' if target.endswith('/') else WEB / target).is_file(), target
native = (WEB / 'native.html').read_text()
for identifier in re.findall(r"getElementById\('([^']+)'\)", native):
    assert identifier in pages['native.html'].ids, identifier
for identifier in re.findall(r"\$\('([^']+)'\)", (WEB / 'lobby.mjs').read_text()):
    assert identifier in pages['index.html'].ids, 'lobby.mjs: ' + identifier
modules = sorted(WEB.glob('*.mjs')) + sorted((WEB / 'shims').glob('*.mjs'))
for path in modules:
    subprocess.run([args.node, '--check', str(path)], check=True, capture_output=True)
    for specifier in re.findall(r"(?:from\s*|import\s*)['\"]([^'\"]+)['\"]", path.read_text()):
        if specifier.startswith('./engine/'): continue  # build output, checked with --built
        if specifier.startswith('.'): assert (path.parent / specifier).is_file(), f'{path.name}: {specifier}'
if args.built:
    for file in ['engine/game.mjs', 'engine/game.wasm', 'engine/game-build.json']:
        assert (WEB / file).is_file(), file
report = {'passed': True, 'pages': list(pages), 'javascriptModules': len(modules), 'builtChecked': args.built,
          'checked': ['local HTML links', 'DOM ID references in native.html and lobby.mjs', 'module import paths', 'JavaScript parse'],
          'not_checked': ['layout', 'pointer events', 'worker startup in browser', 'visual output', 'browser console']}
(ROOT / 'reports').mkdir(exist_ok=True)
(ROOT / 'reports/web-static.json').write_text(json.dumps(report, indent=2) + '\n'); print(json.dumps(report))
