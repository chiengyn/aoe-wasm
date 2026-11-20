#!/usr/bin/env python3
"""Regenerate engine/port.patch and port-changes.json against a pristine upstream checkout, and reject
stale probe test reports (each must match the current core-probe.wasm).

Usage: python3 tools/report_engine.py --reference <aoe1-decomp checkout at the pinned commit>
"""
from pathlib import Path
import argparse, difflib, hashlib, json
ROOT=Path(__file__).resolve().parents[1]
ENGINE=ROOT/'engine'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
read=lambda p:json.loads(p.read_text())
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--reference',type=Path,required=True);args=parser.parse_args()
    changes=[];patch=[]
    for folder in ['src','include','third_party']:
        for path in sorted((ENGINE/'upstream'/folder).rglob('*')):
            if not path.is_file():continue
            rel=path.relative_to(ENGINE/'upstream');original=args.reference/rel
            if not original.is_file():raise SystemExit('Missing original: '+str(original))
            if path.read_bytes()==original.read_bytes():continue
            changes.append({'file':'upstream/'+str(rel),'upstream_sha256':sha(original),'ported_sha256':sha(path)})
            patch.extend(difflib.unified_diff(original.read_text().splitlines(True),path.read_text().splitlines(True),fromfile='a/'+str(rel),tofile='b/'+str(rel)))
    (ENGINE/'port.patch').write_text(''.join(patch))
    (ENGINE/'port-changes.json').write_text(json.dumps({'reference_commit':'6d6d8b14023aa74e6ac30a6c88229d5b818fb805','changed_files':len(changes),'changes':changes},indent=2)+'\n')
    provenance=read(ENGINE/'upstream/SOURCE.json');provenance['changes']['changed_files']=len(changes)
    (ENGINE/'upstream/SOURCE.json').write_text(json.dumps(provenance,indent=2)+'\n')
    compile=read(ENGINE/'build/compile.json');link=read(ENGINE/'build/link-probe.json')
    assert compile['passed']==compile['total']==len(list((ENGINE/'upstream/src').glob('*.cpp')))==225
    for unit in compile['units']:
        assert unit['source_sha256']==sha(ENGINE/'upstream/src'/unit['source']),unit['source']
        assert unit['object_sha256']==sha((ENGINE/'build'/unit['source']).with_suffix('.o')),unit['source']
    headers=sorted(p for folder in ['platform','upstream/include','upstream/third_party'] for p in (ENGINE/folder).rglob('*.h'))
    assert compile['header_sha256']==hashlib.sha256(''.join(str(p.relative_to(ENGINE))+sha(p) for p in headers).encode()).hexdigest()
    for path,digest in link['inputs'].items():assert sha(ENGINE/path)==digest,path
    wasm_sha=sha(ENGINE/'build/core-probe.wasm');assert wasm_sha==link['wasm_sha256']
    for report in ['engine-tests.json','session-tests.json','combat-tests.json','menu-tests.json','multiplayer-tests.json','game-ui-tests.json','drawing-tests.json','battlefield-tests.json']:
        result=read(ROOT/'reports'/report);assert result['passed'] and result['wasmSha256']==wasm_sha,report
    result={'passed':True,'compiled_cpp_files':compile['total'],'changed_upstream_files':len(changes),'all_report_inputs_current':True,'compiler':compile['compiler'],'flags':compile['flags'],**link,'retail_parity_verified':False}
    (ROOT/'reports/engine-build.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({key:result[key] for key in ['passed','compiled_cpp_files','changed_upstream_files','wasm_sha256','wasm_bytes','unsupported_apis']}))
if __name__=='__main__':main()
