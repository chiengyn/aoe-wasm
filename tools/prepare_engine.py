#!/usr/bin/env python3
"""Copy local AI inputs into the engine's case-insensitive virtual namespace."""
from pathlib import Path
import argparse, hashlib, json, shutil
ROOT=Path(__file__).resolve().parents[1]
def prepare(source):
    rows=[];seen=set()
    for path in sorted(source.rglob('*')):
        if not path.is_file() or path.suffix.lower() not in ('.ai','.per','.ply'):continue
        relative=path.relative_to(source).as_posix();virtual=relative.lower()
        if virtual in seen:raise ValueError('Case-insensitive filename collision: '+relative)
        seen.add(virtual);target=ROOT/'web/game/engine'/virtual;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
        rows.append({'source':relative,'virtual':'/'+virtual,'url':'game/engine/'+virtual,'size':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    (ROOT/'web/game/engine-files.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(json.dumps({'engine_ai_files':len(rows),'bytes':sum(r['size'] for r in rows)}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('game_directory',type=Path);prepare(p.parse_args().game_directory.resolve())
