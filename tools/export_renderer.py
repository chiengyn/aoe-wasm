#!/usr/bin/env python3
"""Copy the user's private native viewport tables into the ignored asset tree."""
import argparse,hashlib,json,shutil
from pathlib import Path
def prepare(source):
 root=Path(__file__).resolve().parents[1];files=[]
 for folder,name in [('data2','TileEdge.Dat'),('data2','BlkEdge.Dat'),('data','shadow.col')]:
  src=source/folder/name;relative=f'{folder}/{name.lower()}';dst=root/'web/game/renderer'/relative;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
  files.append({'path':relative,'url':'game/renderer/'+relative,'sha256':hashlib.sha256(dst.read_bytes()).hexdigest(),'size':dst.stat().st_size})
 (root/'web/game/renderer.json').write_text(json.dumps({'files':files},indent=2)+'\n')
 return {'files':len(files),'bytes':sum(f['size'] for f in files)}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);a=p.parse_args()
 print(json.dumps(prepare(a.source)))
