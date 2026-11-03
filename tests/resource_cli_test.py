"""Exercise the original native resource catalog through the shipped CLI."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys
import tempfile

build=Path(sys.argv[1]).resolve()
binary=build/'nf_resources.exe'
assert binary.exists(),'native resource authoring CLI is not implemented'
example=Path(__file__).resolve().parents[1]/'examples/resources/original-packages.json'
original=example.read_bytes()
first=subprocess.run([str(binary),'plan',str(example)],capture_output=True,timeout=20)
assert first.returncode==0,first.stderr
result=json.loads(first.stdout)
assert result['resources']['original/entity.nf']['fields']['enabled']['value'] is True
assert result['localization']['selected']['Greeting']['text']=='Welcome'
assert result['appearances']['original.body']['selected']['resource']=='original/slim.nf'
assert not result['engine_adapter_ready'] and not result['game_tested']
body,count=re.subn(rb'"cache_key":"[0-9a-f]{64}",',b'',first.stdout.rstrip(b'\n'),count=1)
assert count==1 and hashlib.sha256(body).hexdigest()==result['cache_key']
streamed=subprocess.run([str(binary),'plan','-'],input=original,capture_output=True,timeout=20)
assert streamed.returncode==0 and streamed.stdout==first.stdout,streamed.stderr
with tempfile.TemporaryDirectory(prefix='nf-resources-',dir=build) as temp:
    project=Path(temp)/'project.json'
    document=json.loads(original);document['packages'].reverse()
    project.write_text(json.dumps(document),encoding='utf-8')
    reordered=subprocess.run([str(binary),'plan',str(project)],capture_output=True,timeout=20)
    assert reordered.returncode==0 and reordered.stdout==first.stdout,reordered.stderr
    valid=subprocess.run([str(binary),'validate',str(project)],capture_output=True,timeout=20)
    assert valid.returncode==0 and json.loads(valid.stdout)['cache_key']==result['cache_key']
    project.write_text('{',encoding='utf-8')
    failed=subprocess.run([str(binary),'plan',str(project)],capture_output=True,timeout=20)
    assert failed.returncode!=0 and not failed.stdout and b'byte' in failed.stderr
    document=json.loads(original);document['packages'][1]['requires'][0]['id']='missing.package'
    project.write_text(json.dumps(document),encoding='utf-8')
    missing=subprocess.run([str(binary),'plan',str(project)],capture_output=True,timeout=20)
    assert missing.returncode!=0 and not missing.stdout and b'missing.package' in missing.stderr
closed=subprocess.Popen([str(binary),'plan',str(example)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
closed.stdout.close();closed.stderr.read();assert closed.wait(timeout=20)!=0
assert example.read_bytes()==original
print('PASS native resource composition, semantic cache hash, dependency errors and original examples')
