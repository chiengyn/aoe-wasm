#!/usr/bin/env python3
"""Fetch the third-party files the browser build uses for MIDI music into web/vendor (not in git).

- SpessaSynth (Apache-2.0): spessasynth_lib 4.4.3 + spessasynth_core 4.4.5 from npm, checked against the
  registry's sha512 integrity. Only the built bundles and licenses are kept.
- GeneralUser GS v2.0.3 soundfont (S. Christian Collins; free to use and redistribute, see its LICENSE.txt)
  from GitHub at a pinned commit, checked by sha256.

--cache DIR reuses files already downloaded there (same names) when their hashes match.
Nothing downloaded is executed here; archives are only unpacked and copied.
"""
from pathlib import Path
import argparse, base64, hashlib, io, shutil, tarfile, urllib.request

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'web/vendor'
SOUNDFONT_COMMIT = '684543d5e5efaef08d02be50dcda8d552478fa60'
PACKAGES = [
    {'name': 'spessasynth_lib', 'version': '4.4.3',
     'integrity': 'sha512-l7Z7NOVFLt/1+4e1juAuP1FyoWJxAehM3jTB/OtAe0a7MjfLGga4FsPf1AYAuHQwELnHZg+Tyb4ceAQaOUCxtA==',
     'keep': ['dist/index.js', 'dist/spessasynth_processor.min.js', 'LICENSE']},
    {'name': 'spessasynth_core', 'version': '4.4.5',
     'integrity': 'sha512-XeM+yCvPSUQZwHGB02vrL4oJLrv3CMFpKWYatefRdQbH4YRmx/xgfBM6uRF1BgVHCbN6QTmhnHSGggM/Z1fVUg==',
     'keep': ['dist/index.js', 'LICENSE']},
]
FILES = [
    {'name': 'GeneralUser-GS.sf2', 'target': 'soundfont/GeneralUser-GS.sf2',
     'url': f'https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/{SOUNDFONT_COMMIT}/GeneralUser-GS.sf2',
     'sha256': '9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe'},
    {'name': 'LICENSE.txt', 'target': 'soundfont/LICENSE.txt',
     'url': f'https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/{SOUNDFONT_COMMIT}/documentation/LICENSE.txt',
     'sha256': None},
]


def fetch(url, name, cache, check):
    if cache and (cache / name).is_file():
        data = (cache / name).read_bytes()
        if check(data):
            return data
    with urllib.request.urlopen(url) as response:
        data = response.read()
    if not check(data):
        raise SystemExit('Hash mismatch: ' + url)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cache', type=Path, help='Directory with previously downloaded files')
    args = parser.parse_args()
    if VENDOR.exists():
        shutil.rmtree(VENDOR)
    for p in PACKAGES:
        tgz = f"{p['name']}-{p['version']}.tgz"
        algo, digest = p['integrity'].split('-', 1)
        check = lambda data: base64.b64encode(hashlib.new(algo, data).digest()).decode() == digest
        data = fetch(f"https://registry.npmjs.org/{p['name']}/-/{tgz}", tgz, args.cache, check)
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
            for keep in p['keep']:
                member = archive.getmember('package/' + keep)
                target = VENDOR / p['name'] / Path(keep).name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(member).read())
        (VENDOR / p['name'] / 'VERSION').write_text(p['version'] + '\n')
    for f in FILES:
        check = (lambda data, h=f['sha256']: h is None or hashlib.sha256(data).hexdigest() == h)
        data = fetch(f['url'], f['name'], args.cache, check)
        target = VENDOR / f['target']
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    print('Installed', sorted(str(p.relative_to(ROOT)) for p in VENDOR.rglob('*') if p.is_file()))


if __name__ == '__main__':
    main()
